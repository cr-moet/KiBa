#include "PluginEditor.h"

KiBaEditor::KiBaEditor (KiBaProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p), scope (p)
{
    setLookAndFeel (&lookAndFeel);
    auto& apvts = processorRef.apvts;
    zoom       = apvts.getRawParameterValue (ParamIDs::zoom);
    duckOn     = apvts.getRawParameterValue (ParamIDs::duckOn);
    duckShape  = apvts.getRawParameterValue (ParamIDs::duckShape);
    duckDepth  = apvts.getRawParameterValue (ParamIDs::duckDepth);
    duckOffset = apvts.getRawParameterValue (ParamIDs::duckOffset);
    offsetParameter = apvts.getParameter (ParamIDs::duckOffset);
    depthParameter  = apvts.getParameter (ParamIDs::duckDepth);

    // Scope: drag the ducking curve (sideways: offset, up / down: depth)
    addAndMakeVisible (scope);
    scope.setTooltip ("Drag sideways to shift the ducking curve, up / down for its depth; double click: no shift");
    scope.onDragStart = [this] { offsetParameter->beginChangeGesture(); depthParameter->beginChangeGesture(); };
    scope.onDragEnd   = [this] { offsetParameter->endChangeGesture();   depthParameter->endChangeGesture(); };
    scope.onDrag = [this] (float offsetChange, float depthChange)
    {
        const auto set = [] (juce::RangedAudioParameter* parameter, float value)
        {
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (parameter->getNormalisableRange().snapToLegalValue (value)));
        };
        // The offset wraps around (a period further is the same curve)
        float offset = duckOffset->load() + offsetChange;
        offset -= 100.0f * std::floor ((offset + 50.0f) / 100.0f);
        set (offsetParameter, offset);
        set (depthParameter, duckDepth->load() + depthChange);
    };
    scope.onResetOffset = [this]
    {
        offsetParameter->beginChangeGesture();
        offsetParameter->setValueNotifyingHost (offsetParameter->convertTo0to1 (0.0f));
        offsetParameter->endChangeGesture();
    };

    const auto setUpLabel = [this] (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        label.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (label);
    };
    const auto setUpKnob = [this, &apvts, &setUpLabel] (LabelledKnob& k, const char* id, const juce::String& text)
    {
        k.knob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 20);
        k.knob.setColour (juce::Slider::textBoxBackgroundColourId, Colours::text);
        k.knob.setColour (juce::Slider::textBoxTextColourId, Colours::accent);
        k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, id, k.knob);
        addAndMakeVisible (k.knob);
        setUpLabel (k.label, text);
    };
    setUpKnob (zoomKnob, ParamIDs::zoom, "ZOOM");
    setUpKnob (depthKnob, ParamIDs::duckDepth, "DEPTH");
    setUpKnob (offsetKnob, ParamIDs::duckOffset, "OFFSET");
    offsetKnob.knob.setDoubleClickReturnValue (true, 0.0);

    // Phase fader
    phaseFader.setTextBoxStyle (juce::Slider::TextBoxRight, false, 84, 26);
    phaseFader.setColour (juce::Slider::textBoxBackgroundColourId, Colours::text);
    phaseFader.setColour (juce::Slider::textBoxTextColourId, Colours::accent);
    phaseAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, ParamIDs::phase, phaseFader);
    phaseFader.setDoubleClickReturnValue (true, 0.0);
    phaseFader.setTooltip ("Phase rotation of the bass (all frequencies); double click: 0");
    addAndMakeVisible (phaseFader);
    setUpLabel (phaseLabel, "BASS PHASE");

    // Choices
    for (auto [box, label, id, text] : { std::tuple { &windowBox, &windowLabel, ParamIDs::window, "WINDOW" },
                                         std::tuple { &rateBox, &rateLabel, ParamIDs::duckRate, "RATE" } })
    {
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (id)))
            box->addItemList (choice->choices, 1);
        addAndMakeVisible (*box);
        setUpLabel (*label, text);
    }
    windowAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, ParamIDs::window, windowBox);
    rateAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, ParamIDs::duckRate, rateBox);
    windowBox.setTooltip ("Length of the scope window (follows the bar grid of the host)");
    rateBox.setTooltip ("Period of the ducking curve");

    // Ducking curves: radio buttons with LEDs on the choice parameter
    duckOnButton.setClickingTogglesState (true);
    duckOnButton.getProperties().set ("led", true);
    duckOnAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, ParamIDs::duckOn, duckOnButton);
    addAndMakeVisible (duckOnButton);

    auto* shapeParameter = apvts.getParameter (ParamIDs::duckShape);
    for (int i = 0; i < Ducking::shapeNames.size(); ++i)
    {
        auto* b = shapeButtons.add (new juce::TextButton (Ducking::shapeNames[i]));
        b->getProperties().set ("led", true);
        b->setClickingTogglesState (false);
        b->onClick = [this, i] { shapeAttachment->setValueAsCompleteGesture ((float) i); };
        addAndMakeVisible (b);
    }
    shapeAttachment = std::make_unique<juce::ParameterAttachment> (*shapeParameter, [this] (float value)
    {
        for (int i = 0; i < shapeButtons.size(); ++i)
            shapeButtons[i]->setToggleState (i == juce::roundToInt (value), juce::dontSendNotification);
    });
    shapeAttachment->sendInitialUpdate();

    setOpaque (true);
    setResizable (true, true);
    setResizeLimits (1000, 640, 2400, 1600);
    setSize (1200, 740);
    sendLookAndFeelChange();
    startTimerHz (30);
}

KiBaEditor::~KiBaEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void KiBaEditor::timerCallback()
{
    ScopeView::Settings s;
    s.windowStart  = processorRef.getWindowStart();
    s.windowLength = processorRef.getWindowLength();
    s.barLength    = processorRef.getBarLength();
    s.period       = processorRef.getDuckPeriod();
    s.tempo        = processorRef.getTempo();
    s.shape        = (Ducking::Shape) juce::jlimit (0, Ducking::shapeNames.size() - 1, juce::roundToInt (duckShape->load()));
    s.depth        = duckDepth->load() * 0.01f;
    s.offset       = duckOffset->load() * 0.01f;
    s.zoom         = juce::Decibels::decibelsToGain (zoom->load());
    s.playPosition = processorRef.getPlayPosition();
    s.duckOn       = duckOn->load() > 0.5f;
    s.sidechain    = processorRef.hasSidechain();
    scope.update (s);
}

void KiBaEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);

    const juce::Rectangle<float> box (14.0f, 10.0f, 80.0f, 34.0f);
    g.setColour (Colours::text);
    g.fillRect (box);
    g.setColour (Colours::background);
    g.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)));
    g.drawText ("KiBa", box, juce::Justification::centred);

    g.setColour (Colours::text.withAlpha (0.8f));
    g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    g.drawText ("KICK BASS ALIGNMENT", juce::Rectangle<float> (108.0f, 10.0f, 300.0f, 34.0f), juce::Justification::centredLeft);

    g.setColour (Colours::text.withAlpha (0.2f));
    g.drawHorizontalLine (headerHeight + 4, 0.0f, (float) getWidth());

    for (auto r : { duckPanel, bottomPanel })
        Plot::drawScrewedPanel (g, r.toFloat());

    g.setColour (Colours::text);
    g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    g.drawText ("DUCKING CURVE", duckPanel.reduced (30, 9).removeFromTop (16), juce::Justification::centred);
}

void KiBaEditor::resized()
{
    auto area = getLocalBounds().reduced (14, 0);
    area.removeFromTop (headerHeight + 14);
    area.removeFromBottom (14);

    bottomPanel = area.removeFromBottom (112);
    area.removeFromBottom (14);
    duckPanel = area.removeFromRight (190);
    area.removeFromRight (18);
    scope.setBounds (area);

    // Ducking curves: DUCK ON, then the curves
    auto d = duckPanel.reduced (30, 12);
    d.removeFromTop (22);
    const int buttonHeight = juce::jmin (44, (d.getHeight() - 16) / (shapeButtons.size() + 1) - 6);
    duckOnButton.setBounds (d.removeFromTop (buttonHeight));
    d.removeFromTop (16);
    for (auto* b : shapeButtons)
    {
        b->setBounds (d.removeFromTop (buttonHeight));
        d.removeFromTop (6);
    }

    // Bottom row, left to right: window, zoom | phase fader | depth, rate, offset
    auto r = bottomPanel.reduced (32, 6);
    const auto place = [] (LabelledKnob& k, juce::Rectangle<int> cell)
    {
        k.label.setBounds (cell.removeFromTop (14));
        k.knob.setBounds (cell.withSizeKeepingCentre (juce::jmin (cell.getWidth(), 96), 78));
        return k.knob.getY() + (78 - 20) / 2;
    };
    const auto at = [] (juce::Rectangle<int> cell, int height, int centreY) { return cell.withHeight (height).withY (centreY - height / 2); };
    const auto choice = [&at] (juce::Label& label, juce::ComboBox& box, juce::Rectangle<int> cell, int centreY)
    {
        label.setBounds (cell.removeFromTop (14));
        box.setBounds (at (cell, 32, centreY));
    };

    auto windowCell = r.removeFromLeft (110);
    r.removeFromLeft (12);
    const int rowCentre = place (zoomKnob, r.removeFromLeft (96));
    choice (windowLabel, windowBox, windowCell, rowCentre);

    auto right = r.removeFromRight (96 + 12 + 110 + 12 + 96);
    place (depthKnob, right.removeFromLeft (96));
    right.removeFromLeft (12);
    choice (rateLabel, rateBox, right.removeFromLeft (110), rowCentre);
    right.removeFromLeft (12);
    place (offsetKnob, right.removeFromLeft (96));

    r.reduce (30, 0);
    phaseLabel.setBounds (r.removeFromTop (14));
    phaseFader.setBounds (at (r, 44, rowCentre));
}
