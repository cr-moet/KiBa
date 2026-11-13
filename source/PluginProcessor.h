#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Ducking.h"
#include "PhaseRotator.h"

//==============================================================================
// KiBa - kick / bass alignment. Sits on the bass track, the kick comes in through the
// sidechain. The bass is phase rotated and ducked with a tempo synced curve; the scope
// shows bass (output) and kick over a window of the bar grid.
//==============================================================================
namespace ParamIDs
{
    inline constexpr const char* window     = "window";       // scope length (1/16 ... 8 bars)
    inline constexpr const char* zoom       = "zoom";         // dB, scope height
    inline constexpr const char* phase      = "phase";        // degrees, rotation of the bass
    inline constexpr const char* duckOn     = "duckOn";
    inline constexpr const char* duckDepth  = "duckDepth";    // %
    inline constexpr const char* duckRate   = "duckRate";     // period of the curve
    inline constexpr const char* duckOffset = "duckOffset";   // % of the period, shifts the curve in time
    inline constexpr const char* sync       = "sync";         // ms, correction of the host position (scope and curve)
}

namespace Scope
{
    inline const juce::StringArray windowNames { "1/16", "1/8", "1/4", "1/2", "1 BAR", "2 BARS", "4 BARS", "8 BARS" };
    inline constexpr double windowNotes[] { 1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1.0, 2.0, 4.0, 8.0 };   // in whole notes / bars
    constexpr int numColumns = 1024;

    // Minimum and maximum per column; written by the audio thread, read by the editor
    struct Column
    {
        std::atomic<float> bassMin { 0.0f }, bassMax { 0.0f }, kickMin { 0.0f }, kickMax { 0.0f };
    };
}

class KiBaProcessor final : public juce::AudioProcessor,
                            private juce::ValueTree::Listener
{
public:
    KiBaProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                     { return true; }

    const juce::String getName() const override         { return JucePlugin_Name; }
    bool acceptsMidi() const override                   { return false; }
    bool producesMidi() const override                  { return false; }
    bool isMidiEffect() const override                  { return false; }
    double getTailLengthSeconds() const override        { return 0.0; }

    int getNumPrograms() override                       { return 1; }
    int getCurrentProgram() override                    { return 0; }
    void setCurrentProgram (int) override               {}
    const juce::String getProgramName (int) override    { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // For the editor
    const std::array<Scope::Column, Scope::numColumns>& getColumns() const noexcept   { return columns; }
    double getWindowStart() const noexcept       { return windowStart.load(); }     // quarter notes
    double getWindowLength() const noexcept      { return windowLength.load(); }    // quarter notes
    double getDuckPeriod() const noexcept        { return duckPeriod.load(); }      // quarter notes
    double getBarLength() const noexcept         { return barLength.load(); }       // quarter notes
    double getTempo() const noexcept             { return tempo.load(); }           // bpm
    float getPlayPosition() const noexcept       { return playPosition.load(); }    // 0 ... 1 in the window
    bool hasSidechain() const noexcept           { return sidechainActive.load(); }

    // Ducking curve (message thread): the points live in the state, the audio thread
    // gets them as a table
    Ducking::Curve getCurve() const;
    int getPreset() const;                                      // -1: edited (custom)
    void setCurve (const Ducking::Curve& curve, int preset);

    // Start of the last kicks relative to the nearest beat (median, ms); empty without kicks
    std::optional<float> getKickOnsetMs() const;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void storeColumn() noexcept;
    juce::ValueTree curveTree();
    void updateTable();
    void detectKickOnset (float sample, double position) noexcept;

    // Only changes of the curve (the parameters live in the same tree)
    static bool isCurve (const juce::ValueTree& t)   { return t.hasType (Ducking::Curve::treeType) || t.hasType (Ducking::Curve::pointType); }
    void valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier&) override        { if (isCurve (t)) updateTable(); }
    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override          { if (isCurve (parent) || isCurve (child)) updateTable(); }
    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override   { if (isCurve (parent) || isCurve (child)) updateTable(); }
    void valueTreeRedirected (juce::ValueTree&) override                                          { curveTree(); updateTable(); }

    PhaseRotator rotator;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> kickDelay;   // same latency as the bass
    juce::AudioBuffer<float> kickMono;

    std::atomic<float> *window = nullptr, *phase = nullptr, *duckOn = nullptr,
                       *duckDepth = nullptr, *duckRate = nullptr, *duckOffset = nullptr, *sync = nullptr;

    // Curve table: written on the message thread, copied by the audio thread when it changed
    juce::SpinLock tableLock;
    Ducking::Table sharedTable, audioTable;
    std::atomic<int> tableVersion { 0 };
    int audioTableVersion = -1;
    bool writingCurve = false;

    // Kick onset detection (on the delayed kick, in the same time as the scope)
    float fastEnvelope = 0.0f, slowEnvelope = 0.0f, kickPeak = 0.0f;
    float fastRelease = 0.0f, slowCoefficient = 0.0f, peakRelease = 0.0f;
    int holdOff = 0;
    std::array<float, 512> envelopeHistory {};
    std::array<double, 512> positionHistory {};
    int historyPosition = 0;
    std::array<std::atomic<float>, 8> onsets;
    std::atomic<int> onsetCount { 0 };

    // Position of the host (quarter notes); while stopped it stands still
    double beat = 0.0, sampleRate = 48000.0;
    float duckGain = 1.0f, duckSmoothing = 1.0f;

    // Scope: the column being filled
    std::array<Scope::Column, Scope::numColumns> columns;
    int column = -1;
    float bassLow = 0.0f, bassHigh = 0.0f, kickLow = 0.0f, kickHigh = 0.0f;
    std::atomic<double> windowStart { 0.0 }, windowLength { 1.0 }, duckPeriod { 1.0 }, barLength { 4.0 }, tempo { 120.0 };
    std::atomic<float> playPosition { 0.0f };
    std::atomic<bool> sidechainActive { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KiBaProcessor)
};
