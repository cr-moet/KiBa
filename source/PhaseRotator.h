#pragma once

#include <juce_dsp/juce_dsp.h>

//==============================================================================
// Uniformly partitioned convolution (overlap-save) of up to two channels with the same
// real impulse response, fixed latency of one partition. Both channels go through one
// complex FFT (left + i * right): the response is real, so the result is
//      left * h + i * (right * h)
// and one forward and one inverse FFT per block serve both channels.
// The response is set synchronously in prepare(), so the very first block is already
// right (also when the host renders faster than real time).
//==============================================================================
class PartitionedConvolver
{
public:
    static constexpr int blockSize = 256, fftSize = 2 * blockSize;

    void prepare (const float* ir, int length)
    {
        numPartitions = (length + blockSize - 1) / blockSize;
        spectra.assign ((size_t) numPartitions * fftSize * 2, 0.0f);
        std::vector<Complex> time ((size_t) fftSize), frequency ((size_t) fftSize);
        for (int p = 0; p < numPartitions; ++p)
        {
            std::fill (time.begin(), time.end(), Complex());
            for (int i = 0; i < blockSize && p * blockSize + i < length; ++i)
                time[(size_t) i] = ir[p * blockSize + i];
            fft.perform (time.data(), frequency.data(), false);
            std::memcpy (spectra.data() + (size_t) p * fftSize * 2, frequency.data(), sizeof (Complex) * fftSize);
        }
        history.assign ((size_t) numPartitions * fftSize * 2, 0.0f);
        input.assign ((size_t) fftSize, Complex());
        output.assign ((size_t) blockSize, Complex());
        work.assign ((size_t) fftSize, Complex());
        accumulator.assign ((size_t) fftSize * 2, 0.0f);
        newest = 0;
        fill = 0;
    }

    void reset()
    {
        std::fill (history.begin(), history.end(), 0.0f);
        std::fill (input.begin(), input.end(), Complex());
        std::fill (output.begin(), output.end(), Complex());
        fill = 0;
    }

    // In place on one or two channels; the output is 'blockSize' samples late
    void process (float* left, float* right, int numSamples)
    {
        for (int i = 0; i < numSamples;)
        {
            const int count = juce::jmin (numSamples - i, blockSize - fill);
            for (int k = 0; k < count; ++k)
            {
                const auto j = (size_t) (fill + k);
                input[(size_t) blockSize + j] = { left[i + k], right != nullptr ? right[i + k] : 0.0f };
                left[i + k] = output[j].real();
                if (right != nullptr)
                    right[i + k] = output[j].imag();
            }
            fill += count;
            i += count;
            if (fill == blockSize)
            {
                runBlock();
                fill = 0;
            }
        }
    }

private:
    using Complex = std::complex<float>;

    void runBlock()
    {
        // Spectrum of the last two blocks into the history (newest first)
        newest = (newest + numPartitions - 1) % numPartitions;
        fft.perform (input.data(), reinterpret_cast<Complex*> (history.data() + (size_t) newest * fftSize * 2), false);
        std::copy (input.begin() + blockSize, input.end(), input.begin());   // keep the newer half

        // Sum over the partitions: input spectrum k blocks ago times partition k
        // (plain float arrays, so the compiler can vectorise the loop)
        std::fill (accumulator.begin(), accumulator.end(), 0.0f);
        float* acc = accumulator.data();
        for (int k = 0; k < numPartitions; ++k)
        {
            const float* x = history.data() + (size_t) ((newest + k) % numPartitions) * fftSize * 2;
            const float* h = spectra.data() + (size_t) k * fftSize * 2;
            for (int b = 0; b < 2 * fftSize; b += 2)
            {
                acc[b]     += x[b] * h[b]     - x[b + 1] * h[b + 1];
                acc[b + 1] += x[b] * h[b + 1] + x[b + 1] * h[b];
            }
        }
        fft.perform (reinterpret_cast<const Complex*> (accumulator.data()), work.data(), true);
        std::copy_n (work.begin() + blockSize, blockSize, output.begin());   // overlap-save: the second half is valid
    }

    juce::dsp::FFT fft { 9 };   // 512 = fftSize
    int numPartitions = 0, fill = 0, newest = 0;
    std::vector<float> spectra, history, accumulator;   // interleaved complex
    std::vector<Complex> input, output, work;
};

//==============================================================================
// Whole-sample delay for one or two channels
//==============================================================================
class SimpleDelay
{
public:
    void prepare (int delaySamples, int numChannels)
    {
        length = delaySamples + 1;
        buffers.assign ((size_t) numChannels, std::vector<float> ((size_t) length, 0.0f));
        position = 0;
    }

    void reset()
    {
        for (auto& b : buffers)
            std::fill (b.begin(), b.end(), 0.0f);
    }

    void process (float* const* data, int numChannels, int numSamples) noexcept
    {
        const int start = position;
        for (int c = 0; c < numChannels; ++c)
        {
            auto* ring = buffers[(size_t) c].data();
            int p = start;
            for (int i = 0; i < numSamples; ++i)
            {
                const float x = data[c][i];
                const int read = p + 1 == length ? 0 : p + 1;   // the oldest sample: 'length - 1' ago
                ring[p] = x;
                data[c][i] = ring[read];
                p = read;
            }
            position = p;
        }
    }

private:
    std::vector<std::vector<float>> buffers;
    int length = 1, position = 0;
};

//==============================================================================
// Phase rotation of all frequencies by the same angle (bass alignment):
//      y = cos(phi) * x  -  sin(phi) * H{x}      (H{sin} = -cos: positive angles lead)
// H is a Hilbert transformer (linear phase FIR, Kaiser window); x is delayed by its
// latency, so at 0 degrees the signal comes out unchanged (only delayed). About 3600
// taps at 48 kHz: the 90 degree path is flat within 0.2 dB from 20 Hz upwards.
// The latency is constant (reported to the host), whatever the angle.
//==============================================================================
class PhaseRotator
{
public:
    static int lengthFor (double sampleRate) noexcept
    {
        int factor = 1;
        while (sampleRate > 48000.0 * factor * 1.1)
            factor *= 2;
        return 4096 * factor - 1;
    }

    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        const int length = lengthFor (spec.sampleRate);
        latency = (length - 1) / 2;

        // The Hilbert path: the convolver adds one partition of delay, so its centre sits
        // that much earlier and the response is shorter by that on each side
        const int centre = latency - PartitionedConvolver::blockSize, taps = 2 * centre + 1;
        std::vector<float> h ((size_t) taps);
        const double beta = 6.0, i0Beta = bessel0 (beta);
        for (int n = 0; n < taps; ++n)
        {
            const int m = n - centre;
            const double r = (double) m / (double) centre;
            const double window = bessel0 (beta * std::sqrt (juce::jmax (0.0, 1.0 - r * r))) / i0Beta;
            h[(size_t) n] = (m % 2 != 0) ? (float) (2.0 / (juce::MathConstants<double>::pi * m) * window) : 0.0f;
        }
        hilbert.prepare (h.data(), taps);
        delay.prepare (latency, (int) spec.numChannels);

        quadrature.setSize ((int) spec.numChannels, (int) spec.maximumBlockSize, false, false, true);
        cosine.reset (spec.sampleRate, 0.03);
        sine.reset (spec.sampleRate, 0.03);
        cosine.setCurrentAndTargetValue (std::cos (angle));
        sine.setCurrentAndTargetValue (-std::sin (angle));
    }

    void reset()
    {
        hilbert.reset();
        delay.reset();
    }

    void setAngleDegrees (float degrees) noexcept
    {
        angle = juce::degreesToRadians (degrees);
        cosine.setTargetValue (std::cos (angle));
        sine.setTargetValue (-std::sin (angle));
    }

    int getLatency() const noexcept   { return latency; }

    void process (juce::AudioBuffer<float>& buffer, int numChannels)
    {
        const int n = buffer.getNumSamples();
        for (int c = 0; c < numChannels; ++c)
            quadrature.copyFrom (c, 0, buffer, c, 0, n);

        delay.process (buffer.getArrayOfWritePointers(), numChannels, n);
        hilbert.process (quadrature.getWritePointer (0), numChannels > 1 ? quadrature.getWritePointer (1) : nullptr, n);

        if (! cosine.isSmoothing() && ! sine.isSmoothing())
        {
            const float c = cosine.getTargetValue(), s = sine.getTargetValue();
            for (int ch = 0; ch < numChannels; ++ch)
            {
                juce::FloatVectorOperations::multiply (buffer.getWritePointer (ch), c, n);
                juce::FloatVectorOperations::addWithMultiply (buffer.getWritePointer (ch), quadrature.getReadPointer (ch), s, n);
            }
            return;
        }
        for (int i = 0; i < n; ++i)
        {
            const float c = cosine.getNextValue(), s = sine.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer (ch)[i] = c * buffer.getReadPointer (ch)[i] + s * quadrature.getReadPointer (ch)[i];
        }
    }

private:
    static double bessel0 (double x)   // modified Bessel function of the first kind, order 0
    {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 50; ++k)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    }

    PartitionedConvolver hilbert;
    SimpleDelay delay;
    juce::AudioBuffer<float> quadrature;
    juce::SmoothedValue<float> cosine { 1.0f }, sine { 0.0f };
    float angle = 0.0f;
    int latency = 0;
};
