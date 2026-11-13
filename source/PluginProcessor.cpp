#include "PluginProcessor.h"
#include "PluginEditor.h"

KiBaProcessor::KiBaProcessor()
    : AudioProcessor (BusesProperties().withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
                                       .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "KiBa", createParameterLayout())
{
    window     = apvts.getRawParameterValue (ParamIDs::window);
    phase      = apvts.getRawParameterValue (ParamIDs::phase);
    duckOn     = apvts.getRawParameterValue (ParamIDs::duckOn);
    duckShape  = apvts.getRawParameterValue (ParamIDs::duckShape);
    duckDepth  = apvts.getRawParameterValue (ParamIDs::duckDepth);
    duckRate   = apvts.getRawParameterValue (ParamIDs::duckRate);
    duckOffset = apvts.getRawParameterValue (ParamIDs::duckOffset);
}

juce::AudioProcessorValueTreeState::ParameterLayout KiBaProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    const auto withText = [] (const char* unit, int decimals, bool sign)
    {
        return AudioParameterFloatAttributes().withLabel (unit).withStringFromValueFunction (
            [unit, decimals, sign] (float v, int) { return (sign && v > 0.0f ? "+" : "") + String (v, decimals) + " " + unit; });
    };

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::window, 1 }, "Window", Scope::windowNames, 2));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::zoom, 1 }, "Zoom",
                                                       NormalisableRange<float> (0.0f, 24.0f, 0.1f), 0.0f, withText ("dB", 1, true)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::phase, 1 }, "Phase",
                                                       NormalisableRange<float> (-180.0f, 180.0f, 1.0f), 0.0f,
                                                       AudioParameterFloatAttributes().withLabel ("deg").withStringFromValueFunction (
                                                           [] (float v, int) { return (v > 0.0f ? "+" : "") + String (roundToInt (v)) + String::fromUTF8 (" \xc2\xb0"); })));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::duckOn, 1 }, "Duck On", false));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::duckShape, 1 }, "Duck Shape", Ducking::shapeNames, 0));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::duckDepth, 1 }, "Duck Depth",
                                                       NormalisableRange<float> (0.0f, 100.0f, 1.0f), 60.0f, withText ("%", 0, false)));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::duckRate, 1 }, "Duck Rate", Ducking::rateNames, 2));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::duckOffset, 1 }, "Duck Offset",
                                                       NormalisableRange<float> (-50.0f, 50.0f, 0.1f), 0.0f, withText ("%", 1, true)));
    return layout;
}

void KiBaProcessor::prepareToPlay (double newSampleRate, int samplesPerBlock)
{
    sampleRate = newSampleRate;
    const auto numChannels = (juce::uint32) juce::jmax (1, getMainBusNumInputChannels());
    rotator.prepare ({ sampleRate, (juce::uint32) samplesPerBlock, numChannels });
    rotator.setAngleDegrees (phase->load());
    setLatencySamples (rotator.getLatency());

    kickDelay.prepare ({ sampleRate, (juce::uint32) samplesPerBlock, 1 });
    kickDelay.setMaximumDelayInSamples (rotator.getLatency() + 1);
    kickDelay.setDelay ((float) rotator.getLatency());
    kickMono.setSize (1, samplesPerBlock, false, false, true);

    duckGain = 1.0f;
    duckSmoothing = 1.0f - std::exp (-1.0f / (0.0003f * (float) sampleRate));   // 0.3 ms: no click at the drop
    column = -1;
}

bool KiBaProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    if (in != out || (in != juce::AudioChannelSet::stereo() && in != juce::AudioChannelSet::mono()))
        return false;
    const auto side = layouts.getChannelSet (true, 1);
    return side.isDisabled() || side == juce::AudioChannelSet::stereo() || side == juce::AudioChannelSet::mono();
}

void KiBaProcessor::storeColumn() noexcept
{
    if (column < 0)
        return;
    auto& c = columns[(size_t) column];
    c.bassMin.store (bassLow, std::memory_order_relaxed);
    c.bassMax.store (bassHigh, std::memory_order_relaxed);
    c.kickMin.store (kickLow, std::memory_order_relaxed);
    c.kickMax.store (kickHigh, std::memory_order_relaxed);
}

void KiBaProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    auto main = getBusBuffer (buffer, true, 0);
    const int numChannels = juce::jmin (main.getNumChannels(), getMainBusNumOutputChannels());
    if (n == 0 || numChannels == 0 || n > kickMono.getNumSamples())
        return;

    // Kick: the sidechain as mono, delayed like the bass so both line up
    auto* kick = kickMono.getWritePointer (0);
    const auto side = getBusCount (true) > 1 ? getBusBuffer (buffer, true, 1) : juce::AudioBuffer<float>();
    sidechainActive.store (side.getNumChannels() > 0);
    if (side.getNumChannels() > 0)
    {
        juce::FloatVectorOperations::copy (kick, side.getReadPointer (0), n);
        if (side.getNumChannels() > 1)
        {
            juce::FloatVectorOperations::add (kick, side.getReadPointer (1), n);
            juce::FloatVectorOperations::multiply (kick, 0.5f, n);
        }
    }
    else
        juce::FloatVectorOperations::clear (kick, n);
    {
        juce::dsp::AudioBlock<float> block (kickMono.getArrayOfWritePointers(), 1, (size_t) n);
        kickDelay.process (juce::dsp::ProcessContextReplacing<float> (block));
    }

    // Bass: phase rotation
    rotator.setAngleDegrees (phase->load());
    rotator.process (main, numChannels);

    // Tempo and position (quarter notes). The output is 'latency' late: what comes out now
    // belongs to the musical time 'latency' earlier, so the curve and the scope use that.
    double bpm = 120.0, barQuarters = 4.0;
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
        {
            bpm = position->getBpm().orFallback (120.0);
            if (const auto signature = position->getTimeSignature())
                barQuarters = 4.0 * signature->numerator / juce::jmax (1, signature->denominator);
            if (position->getIsPlaying())
                if (const auto ppq = position->getPpqPosition())
                    beat = *ppq;
        }
    barLength.store (barQuarters);
    const double perSample = bpm / 60.0 / sampleRate;
    const double start = beat - getLatencySamples() * perSample;

    const int windowIndex = juce::jlimit (0, Scope::windowNames.size() - 1, juce::roundToInt (window->load()));
    const double length = Scope::windowNotes[windowIndex] * (windowIndex < 4 ? 4.0 : barQuarters);
    const int rateIndex = juce::jlimit (0, Ducking::rateNames.size() - 1, juce::roundToInt (duckRate->load()));
    const double period = rateIndex == 4 ? barQuarters : Ducking::rateBeats[rateIndex];
    const auto shape = (Ducking::Shape) juce::jlimit (0, Ducking::shapeNames.size() - 1, juce::roundToInt (duckShape->load()));
    const float depth = duckOn->load() > 0.5f ? duckDepth->load() * 0.01f : 0.0f;
    const double offset = duckOffset->load() * 0.01;
    windowLength.store (length);
    duckPeriod.store (period);

    for (int i = 0; i < n; ++i)
    {
        const double t = start + i * perSample;

        // Ducking (smoothed by 0.3 ms against clicks at the steep edges)
        const float target = Ducking::gain (shape, (float) (t / period - offset), depth);
        duckGain += (target - duckGain) * duckSmoothing;
        float bass = 0.0f;
        for (int c = 0; c < numChannels; ++c)
        {
            auto& s = main.getWritePointer (c)[i];
            s *= duckGain;
            bass += s;
        }
        bass /= (float) numChannels;

        // Scope column
        const double position = t / length;
        const double cycle = std::floor (position);
        const int newColumn = juce::jlimit (0, Scope::numColumns - 1, (int) ((position - cycle) * Scope::numColumns));
        if (newColumn != column)
        {
            storeColumn();
            if (newColumn < column || column < 0)   // a new pass through the window
                windowStart.store (cycle * length);
            column = newColumn;
            bassLow = bassHigh = bass;
            kickLow = kickHigh = kick[i];
        }
        bassLow  = juce::jmin (bassLow, bass);   bassHigh = juce::jmax (bassHigh, bass);
        kickLow  = juce::jmin (kickLow, kick[i]); kickHigh = juce::jmax (kickHigh, kick[i]);
    }
    storeColumn();
    playPosition.store ((float) column / (float) Scope::numColumns);
    beat += n * perSample;

    for (int c = numChannels; c < getTotalNumOutputChannels(); ++c)
        buffer.clear (c, 0, n);
}

juce::AudioProcessorEditor* KiBaProcessor::createEditor()
{
    return new KiBaEditor (*this);
}

void KiBaProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void KiBaProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new KiBaProcessor();
}
