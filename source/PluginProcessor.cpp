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
    duckDepth  = apvts.getRawParameterValue (ParamIDs::duckDepth);
    duckRate   = apvts.getRawParameterValue (ParamIDs::duckRate);
    duckOffset = apvts.getRawParameterValue (ParamIDs::duckOffset);
    sync       = apvts.getRawParameterValue (ParamIDs::sync);

    for (auto& o : onsets)
        o.store (0.0f);
    curveTree();   // the default curve (pump)
    apvts.state.addListener (this);
    updateTable();
}

//==============================================================================
juce::ValueTree KiBaProcessor::curveTree()
{
    auto tree = apvts.state.getChildWithName (Ducking::Curve::treeType);
    if (! tree.isValid())
    {
        tree = juce::ValueTree (Ducking::Curve::treeType, { { Ducking::Curve::presetId, 0 } });
        Ducking::Curve::preset (0).writeTo (tree);
        apvts.state.appendChild (tree, nullptr);
    }
    return tree;
}

Ducking::Curve KiBaProcessor::getCurve() const
{
    return Ducking::Curve::readFrom (apvts.state.getChildWithName (Ducking::Curve::treeType));
}

int KiBaProcessor::getPreset() const
{
    return (int) apvts.state.getChildWithName (Ducking::Curve::treeType).getProperty (Ducking::Curve::presetId, -1);
}

void KiBaProcessor::setCurve (const Ducking::Curve& curve, int preset)
{
    auto tree = curveTree();
    {
        const juce::ScopedValueSetter<bool> quiet (writingCurve, true);   // one table update, not one per point
        tree.setProperty (Ducking::Curve::presetId, preset, nullptr);
        curve.writeTo (tree);
    }
    updateTable();
}

void KiBaProcessor::updateTable()
{
    if (writingCurve)
        return;
    Ducking::Table table;
    table.fill (getCurve());
    {
        const juce::SpinLock::ScopedLockType lock (tableLock);
        sharedTable = table;
    }
    ++tableVersion;
}

std::optional<float> KiBaProcessor::getKickOnsetMs() const
{
    const int count = juce::jmin (onsetCount.load(), (int) onsets.size());
    if (count == 0)
        return {};
    std::vector<float> values;
    for (int i = 0; i < count; ++i)
        values.push_back (onsets[(size_t) i].load());
    std::sort (values.begin(), values.end());
    return values[values.size() / 2];
}

// Kick onset: a fast envelope that jumps well above the slow one starts a kick; from there
// back to where the envelope first rose (above 10 % of that value): the first transient.
// The time of that sample relative to the nearest beat is kept (the last 8 kicks).
void KiBaProcessor::detectKickOnset (float sample, double position) noexcept
{
    const float a = std::abs (sample);
    fastEnvelope = juce::jmax (a, fastEnvelope * fastRelease);
    slowEnvelope += (a - slowEnvelope) * slowCoefficient;
    kickPeak = juce::jmax (a, kickPeak * peakRelease);

    historyPosition = (historyPosition + 1) % (int) envelopeHistory.size();
    envelopeHistory[(size_t) historyPosition] = fastEnvelope;
    positionHistory[(size_t) historyPosition] = position;

    if (holdOff > 0)
    {
        --holdOff;
        return;
    }
    if (fastEnvelope < 4.0f * slowEnvelope + 1.0e-4f || fastEnvelope < 0.05f * kickPeak)
        return;

    const float threshold = 0.1f * fastEnvelope;
    int start = historyPosition;
    for (int back = 1; back < (int) envelopeHistory.size(); ++back)
    {
        const int i = (historyPosition - back + (int) envelopeHistory.size()) % (int) envelopeHistory.size();
        if (envelopeHistory[(size_t) i] < threshold)
            break;
        start = i;
    }
    const double q = positionHistory[(size_t) start];
    const float ms = (float) ((q - std::round (q)) * 60000.0 / juce::jmax (1.0, tempo.load()));
    const int n = onsetCount.load();
    onsets[(size_t) (n % (int) onsets.size())].store (ms);
    onsetCount.store (n + 1);
    holdOff = (int) (0.06 * sampleRate);   // one kick: no new onset within 60 ms
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
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::duckOn, 1 }, "Duck On", true));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::duckDepth, 1 }, "Duck Depth",
                                                       NormalisableRange<float> (0.0f, 100.0f, 1.0f), 60.0f, withText ("%", 0, false)));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::duckRate, 1 }, "Duck Rate", Ducking::rateNames, 2));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::duckOffset, 1 }, "Duck Offset",
                                                       NormalisableRange<float> (-50.0f, 50.0f, 0.1f), 0.0f, withText ("%", 1, true)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::sync, 1 }, "Sync",
                                                       NormalisableRange<float> (-250.0f, 250.0f, 0.1f), 0.0f, withText ("ms", 1, true)));
    return layout;
}

void KiBaProcessor::prepareToPlay (double newSampleRate, int samplesPerBlock)
{
    sampleRate = newSampleRate;
    const auto numChannels = (juce::uint32) juce::jmax (1, getMainBusNumInputChannels());
    rotator.prepare ({ sampleRate, (juce::uint32) samplesPerBlock, numChannels });
    rotator.setAngleDegrees (phase->load());
    setLatencySamples (rotator.getLatency());

    kickDelay.prepare (rotator.getLatency(), 1);
    kickMono.setSize (1, samplesPerBlock, false, false, true);

    duckGain = 1.0f;
    duckSmoothing = 1.0f - std::exp (-1.0f / (0.0003f * (float) sampleRate));   // 0.3 ms: no click at the drop
    column = -1;

    fastRelease = std::exp (-1.0f / (0.002f * (float) sampleRate));
    slowCoefficient = 1.0f - std::exp (-1.0f / (0.05f * (float) sampleRate));
    peakRelease = std::exp (-1.0f / (2.0f * (float) sampleRate));
    fastEnvelope = slowEnvelope = kickPeak = 0.0f;
    holdOff = 0;
    onsetCount.store (0);
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
    kickDelay.process (kickMono.getArrayOfWritePointers(), 1, n);

    // Bass: phase rotation
    rotator.setAngleDegrees (phase->load());
    rotator.process (main, numChannels);

    // Tempo and position (quarter notes). The output is 'latency' late: what comes out now
    // belongs to the musical time 'latency' earlier, so the curve and the scope use that.
    double bpm = 120.0, barQuarters = 4.0;
    bool playing = false;
    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
        {
            bpm = position->getBpm().orFallback (120.0);
            if (const auto signature = position->getTimeSignature())
                barQuarters = 4.0 * signature->numerator / juce::jmax (1, signature->denominator);
            if (const auto ppq = position->getPpqPosition(); ppq && position->getIsPlaying())
            {
                beat = *ppq;
                playing = true;
            }
        }
    barLength.store (barQuarters);
    tempo.store (bpm);
    const double perSample = bpm / 60.0 / sampleRate;
    // SYNC: hosts differ in how their position relates to the audio of a plugin with
    // latency (and its sidechain); this correction puts the kick back onto the grid
    const double start = beat - getLatencySamples() * perSample + sync->load() * 0.001 * bpm / 60.0;

    const int windowIndex = juce::jlimit (0, Scope::windowNames.size() - 1, juce::roundToInt (window->load()));
    const double length = Scope::windowNotes[windowIndex] * (windowIndex < 4 ? 4.0 : barQuarters);
    const int rateIndex = juce::jlimit (0, Ducking::rateNames.size() - 1, juce::roundToInt (duckRate->load()));
    const double period = rateIndex == 4 ? barQuarters : Ducking::rateBeats[rateIndex];
    if (const int version = tableVersion.load(); version != audioTableVersion)
    {
        const juce::SpinLock::ScopedTryLockType lock (tableLock);
        if (lock.isLocked())
        {
            audioTable = sharedTable;
            audioTableVersion = version;
        }
    }
    // Stopped: no ducking (back to unity within the smoothing) and the scope stands still
    const float depth = duckOn->load() > 0.5f && playing ? duckDepth->load() * 0.01f : 0.0f;
    const double offset = duckOffset->load() * 0.01;
    if (! juce::approximatelyEqual (length, windowLength.load()))   // another window: start empty
    {
        for (auto& c : columns)
            for (auto* v : { &c.bassMin, &c.bassMax, &c.kickMin, &c.kickMax })
                v->store (0.0f, std::memory_order_relaxed);
        column = -1;
    }
    windowLength.store (length);
    duckPeriod.store (period);

    for (int i = 0; i < n; ++i)
    {
        const double t = start + i * perSample;

        // Ducking (smoothed by 0.3 ms against clicks at the steep edges)
        const float target = 1.0f - depth * (1.0f - audioTable.at ((float) (t / period - offset)));
        duckGain += (target - duckGain) * duckSmoothing;
        float bass = 0.0f;
        for (int c = 0; c < numChannels; ++c)
        {
            auto& s = main.getWritePointer (c)[i];
            s *= duckGain;
            bass += s;
        }
        bass /= (float) numChannels;
        if (! playing)
            continue;
        detectKickOnset (kick[i], t);

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
    if (playing)
    {
        ++dataVersion;
        storeColumn();
        playPosition.store ((float) column / (float) Scope::numColumns);
    }

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
