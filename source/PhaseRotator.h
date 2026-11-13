#pragma once

#include <juce_dsp/juce_dsp.h>

//==============================================================================
// Uniformly partitioned convolution (overlap-save), fixed latency of one partition.
// The impulse response is set synchronously in prepare(), so the very first block is
// already right (also when the host renders faster than real time).
//==============================================================================
class PartitionedConvolver
{
public:
    static constexpr int blockSize = 256, fftSize = 2 * blockSize;

    void prepare (const float* ir, int length, int numChannels)
    {
        numPartitions = (length + blockSize - 1) / blockSize;
        spectra.assign ((size_t) numPartitions * fftSize * 2, 0.0f);
        std::vector<float> work ((size_t) fftSize * 2);
        for (int p = 0; p < numPartitions; ++p)
        {
            std::fill (work.begin(), work.end(), 0.0f);
            for (int i = 0; i < blockSize && p * blockSize + i < length; ++i)
                work[(size_t) i] = ir[p * blockSize + i];
            fft.performRealOnlyForwardTransform (work.data(), true);
            std::copy (work.begin(), work.begin() + fftSize + 2, spectra.begin() + (size_t) p * fftSize * 2);
        }
        channels.resize ((size_t) numChannels);
        for (auto& c : channels)
        {
            c.input.assign ((size_t) fftSize, 0.0f);
            c.output.assign ((size_t) blockSize, 0.0f);
            c.history.assign ((size_t) numPartitions * fftSize * 2, 0.0f);
            c.newest = 0;
        }
        fill = 0;
    }

    void reset()
    {
        for (auto& c : channels)
        {
            std::fill (c.input.begin(), c.input.end(), 0.0f);
            std::fill (c.output.begin(), c.output.end(), 0.0f);
            std::fill (c.history.begin(), c.history.end(), 0.0f);
        }
        fill = 0;
    }

    // In place; the output is 'blockSize' samples late
    void process (float* const* data, int numChannels, int numSamples)
    {
        for (int i = 0; i < numSamples;)
        {
            const int count = juce::jmin (numSamples - i, blockSize - fill);
            for (int ch = 0; ch < numChannels; ++ch)
            {
                auto& c = channels[(size_t) ch];
                std::copy_n (data[ch] + i, count, c.input.data() + blockSize + fill);
                std::copy_n (c.output.data() + fill, count, data[ch] + i);
            }
            fill += count;
            i += count;
            if (fill == blockSize)
            {
                for (int ch = 0; ch < numChannels; ++ch)
                    runBlock (channels[(size_t) ch]);
                fill = 0;
            }
        }
    }

private:
    struct Channel
    {
        std::vector<float> input, output, history;   // history: the input spectra, newest at 'newest'
        int newest = 0;
    };

    void runBlock (Channel& c)
    {
        // Spectrum of the last two blocks into the history
        c.newest = (c.newest + numPartitions - 1) % numPartitions;
        float* spectrum = c.history.data() + (size_t) c.newest * fftSize * 2;
        std::fill (spectrum, spectrum + fftSize * 2, 0.0f);
        std::copy (c.input.begin(), c.input.end(), spectrum);
        fft.performRealOnlyForwardTransform (spectrum, true);
        std::copy (c.input.begin() + blockSize, c.input.end(), c.input.begin());   // keep the newer half

        // Sum over the partitions: input spectrum k blocks ago times partition k
        accumulator.assign ((size_t) fftSize * 2, 0.0f);
        auto* acc = reinterpret_cast<std::complex<float>*> (accumulator.data());
        for (int k = 0; k < numPartitions; ++k)
        {
            const auto* x = reinterpret_cast<const std::complex<float>*> (c.history.data() + (size_t) ((c.newest + k) % numPartitions) * fftSize * 2);
            const auto* h = reinterpret_cast<const std::complex<float>*> (spectra.data() + (size_t) k * fftSize * 2);
            for (int b = 0; b <= blockSize; ++b)
                acc[b] += x[b] * h[b];
        }
        fft.performRealOnlyInverseTransform (accumulator.data());
        std::copy_n (accumulator.data() + blockSize, blockSize, c.output.data());   // overlap-save: the second half is valid
    }

    juce::dsp::FFT fft { 9 };   // 512 = fftSize
    int numPartitions = 0, fill = 0;
    std::vector<float> spectra, accumulator;
    std::vector<Channel> channels;
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
        hilbert.prepare (h.data(), taps, (int) spec.numChannels);

        delay.prepare (spec);
        delay.setMaximumDelayInSamples (latency + 1);
        delay.setDelay ((float) latency);

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

        juce::dsp::AudioBlock<float> direct (buffer.getArrayOfWritePointers(), (size_t) numChannels, (size_t) n);
        delay.process (juce::dsp::ProcessContextReplacing<float> (direct));
        hilbert.process (quadrature.getArrayOfWritePointers(), numChannels, n);

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
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> delay;
    juce::AudioBuffer<float> quadrature;
    juce::SmoothedValue<float> cosine { 1.0f }, sine { 0.0f };
    float angle = 0.0f;
    int latency = 0;
};
