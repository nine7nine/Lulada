// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <element/session.hpp>

#include "ui/guicommon.hpp"
#include "ui/transportbar.hpp"

namespace element {

class BarLabel : public DragableIntLabel
{
public:
    BarLabel (TransportBar& t) : owner (t)
    {
        setDragable (false);
    }

    void settingLabelDoubleClicked() override
    {
        if (auto e = owner.engine)
            e->seekToAudioFrame (0);
    }

    TransportBar& owner;
};

class BeatLabel : public DragableIntLabel
{
public:
    BeatLabel()
    {
        setDragable (false);
    }
};

class SubBeatLabel : public DragableIntLabel
{
public:
    SubBeatLabel()
    {
        setDragable (false);
    }
};

TransportBar::TransportBar()
{
    /* Modernised transport cluster -- icons sized at ~55 % of the
     * button via pathReduction = 10 px so they sit inset like the
     * tracker EDIT pill rather than crowding the borders.  Bitwig
     * palette: bright orange Play, warm-white Stop, bright red
     * Record, soft-white SeekZero.  Toggle-on body wash highlights
     * Play when running + Record when armed. */
    constexpr float kIconPad = 10.0f;
    play = std::make_unique<PlayButton>();
    addAndMakeVisible (play.get());
    play->setPath (getIcons().fasPlay, kIconPad);
    play->setConnectedEdges (Button::ConnectedOnLeft | Button::ConnectedOnRight | Button::ConnectedOnTop | Button::ConnectedOnBottom);
    play->addListener (this);
    play->setIconColour (Colour (0xff'ff'8a'30));               // Bitwig orange
    play->setColour (TextButton::buttonOnColourId, Colour (0xff'ff'8a'30));
    play->setColour (SettingButton::backgroundOnColourId, Colour (0xff'4a'2a'10));

    stop = std::make_unique<StopButton>();
    addAndMakeVisible (stop.get());
    stop->setPath (getIcons().fasStop, kIconPad);
    stop->setConnectedEdges (Button::ConnectedOnLeft | Button::ConnectedOnRight | Button::ConnectedOnTop | Button::ConnectedOnBottom);
    stop->addListener (this);
    stop->setIconColour (Colour (0xff'e8'e8'e8));               // bright white

    record = std::make_unique<RecordButton>();
    addAndMakeVisible (record.get());
    record->setPath (getIcons().fasCircle, kIconPad);
    record->setConnectedEdges (Button::ConnectedOnLeft | Button::ConnectedOnRight | Button::ConnectedOnTop | Button::ConnectedOnBottom);
    record->addListener (this);
    record->setIconColour (Colour (0xff'ff'4d'4d));             // bright red
    record->setColour (SettingButton::backgroundOnColourId, Colour (0xff'5a'1a'1a));

    toZero = std::make_unique<SeekZeroButton>();
    addAndMakeVisible (toZero.get());
    auto toZeroPath = getIcons().fasChevronRight;
    toZeroPath.applyTransform (AffineTransform().rotated (juce::MathConstants<float>::pi));
    toZero->setPath (toZeroPath, kIconPad);
    toZero->setConnectedEdges (Button::ConnectedOnLeft | Button::ConnectedOnRight | Button::ConnectedOnTop | Button::ConnectedOnBottom);
    toZero->addListener (this);
    toZero->setIconColour (Colour (0xff'd0'd0'd0));

    barLabel = std::make_unique<BarLabel> (*this);
    addAndMakeVisible (barLabel.get());
    barLabel->setName ("barLabel");

    beatLabel = std::make_unique<BeatLabel>();
    addAndMakeVisible (beatLabel.get());
    beatLabel->setName ("beatLabel");

    subLabel = std::make_unique<SubBeatLabel>();
    addAndMakeVisible (subLabel.get());
    subLabel->setName ("subLabel");

    setBeatTime (0.f);
    /* Default size sized for tempoBarHeight (26 in the main toolbar)
     * — the parent Toolbar will resize us with the real height, but
     * we still need a non-zero initial extent for updateWidth(). */
    setSize (240, 26);
    updateWidth();

    startTimer (88);
}

TransportBar::~TransportBar()
{
    play = nullptr;
    stop = nullptr;
    record = nullptr;
    barLabel = nullptr;
    beatLabel = nullptr;
    subLabel = nullptr;
}

bool TransportBar::checkForMonitor()
{
    if (nullptr == monitor)
    {
        if (auto* w = ViewHelpers::getGlobals (this))
        {
            engine = w->audio();
            monitor = engine->getTransportMonitor();
            session = w->session();
        }
    }

    return monitor != nullptr;
}

void TransportBar::timerCallback()
{
    if (! checkForMonitor())
        return;

    if (play->getToggleState() != monitor->playing.get())
        play->setToggleState (monitor->playing.get(), dontSendNotification);
    if (record->getToggleState() != monitor->recording.get())
        record->setToggleState (monitor->recording.get(), dontSendNotification);

    stabilize();
}

void TransportBar::paint (Graphics& g)
{
}

void TransportBar::resized()
{
    /* Height-driven layout.  In the new MainDisplayPanel-led toolbar
     * the bar/beat/sub labels are hidden (showPositionLabels_=false)
     * so this lays out as a tight Play / Stop / Record / SeekZero
     * cluster on the left.  When labels are shown (e.g. legacy
     * standalone use) they precede the buttons with a small group
     * gap, matching the original layout. */
    const int h = getHeight();
    const int labelW = juce::jmax (24, juce::roundToInt (h * 1.6f));
    const int btnW   = juce::jmax (22, juce::roundToInt (h * 1.25f));
    constexpr int gap = 2;
    constexpr int groupGap = 6;

    int x = 0;
    if (showPositionLabels_)
    {
        barLabel->setBounds  (x, 0, labelW, h); x += labelW + gap;
        beatLabel->setBounds (x, 0, labelW, h); x += labelW + gap;
        subLabel->setBounds  (x, 0, labelW, h); x += labelW + groupGap;
    }

    play->setBounds   (x, 0, btnW, h); x += btnW + gap;
    stop->setBounds   (x, 0, btnW, h); x += btnW + gap;
    record->setBounds (x, 0, btnW, h); x += btnW + gap;
    toZero->setBounds (x, 0, btnW, h);
}

void TransportBar::setShowPositionLabels (bool show)
{
    if (showPositionLabels_ == show) return;
    showPositionLabels_ = show;
    if (barLabel)  barLabel ->setVisible (show);
    if (beatLabel) beatLabel->setVisible (show);
    if (subLabel)  subLabel ->setVisible (show);
    resized();
    updateWidth();
}

void TransportBar::buttonClicked (Button* buttonThatWasClicked)
{
    if (! checkForMonitor())
        return;

    if (buttonThatWasClicked == play.get())
    {
        if (monitor->playing.get())
            engine->seekToAudioFrame (0);
        else
            engine->setPlaying (true);
    }
    else if (buttonThatWasClicked == toZero.get())
    {
        engine->seekToAudioFrame (0);
    }
    else if (buttonThatWasClicked == stop.get())
    {
        if (! monitor->playing.get())
            engine->seekToAudioFrame (0);
        else
            engine->setPlaying (false);
    }
    else if (buttonThatWasClicked == record.get())
    {
        engine->setRecording (! monitor->recording.get());
    }
}

void TransportBar::setBeatTime (const float t)
{
}

void TransportBar::stabilize()
{
    if (! checkForMonitor())
        return;

    int bars = 0, beats = 0, sub = 0;
    monitor->getBarsAndBeats (bars, beats, sub);

    /* juce::Value::setValue only fires its listener chain on actual
     * delta (juce_Value.cpp::ValueSource::setValue), and
     * DragableIntLabel hooks valueChanged → repaint().  So a stable
     * transport position costs three int writes + three Value
     * equality checks per 88ms tick — no paint().  The previous
     * unconditional `c->repaint()` loop forced a paint of all three
     * labels every tick whether bars/beats/sub changed or not. */
    barLabel->tempoValue  = bars + 1;
    beatLabel->tempoValue = beats + 1;
    subLabel->tempoValue  = sub + 1;
}

void TransportBar::updateWidth()
{
    setSize (toZero->getRight(), getHeight());
}

} // namespace element
