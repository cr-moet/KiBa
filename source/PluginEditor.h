#pragma once

#include "Views.h"

//==============================================================================
// Scope in the middle, the ducking curves on the right (push buttons with LEDs),
// below: scope window and zoom, the phase fader, depth / rate / offset of the ducking.
//==============================================================================
class KiBaEditor final : public juce::AudioProcessorEditor,
                         private juce::Timer
{
public:
    explicit KiBaEditor (KiBaProcessor&);
    ~KiBaEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    KiBaProcessor& processorRef;
    KiBaLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow { this, 600 };

    ScopeView scope;

    struct LabelledKnob
    {
        juce::Slider knob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };
    LabelledKnob zoomKnob, depthKnob, offsetKnob, syncKnob;
    juce::TextButton syncButton { "SYNC TO KICK" };

    juce::Slider phaseFader { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Label phaseLabel, windowLabel, rateLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> phaseAttachment;

    juce::ComboBox windowBox, rateBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> windowAttachment, rateAttachment;

    // Ducking: on / off and one LED button per curve (radio group on the choice parameter)
    juce::TextButton duckOnButton { "DUCK ON" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> duckOnAttachment;
    juce::OwnedArray<CurveButton> shapeButtons;
    juce::RangedAudioParameter *offsetParameter = nullptr, *depthParameter = nullptr;

    std::atomic<float> *zoom = nullptr, *duckOn = nullptr, *duckDepth = nullptr, *duckOffset = nullptr;

    static constexpr int headerHeight = 48;
    juce::Rectangle<int> duckPanel, bottomPanel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KiBaEditor)
};
