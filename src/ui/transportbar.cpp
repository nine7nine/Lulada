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
    /* Transport icons are colour-coded so they read against the bold
     * neutral border + dark body: play=green, stop=blue, record=red,
     * rewind=white.  These tint the icon glyph itself, independent of
     * any toggle-on body wash from backgroundOnColourId. */
    play = std::make_unique<PlayButton>();
    addAndMakeVisible (play.get());
    play->setPath (getIcons().fasPlay, 4.4f);
    play->setConnectedEdges (Button::ConnectedOnLeft | Button::ConnectedOnRight | Button::ConnectedOnTop | Button::ConnectedOnBottom);
    play->addListener (this);
    play->setIconColour (Colour (0xff5cd65c));
    play->setColour (TextButton::buttonOnColourId, Colours::chartreuse);
    play->setColour (SettingButton::backgroundOnColourId, Colors::toggleGreen);

    stop = std::make_unique<StopButton>();
    addAndMakeVisible (stop.get());
    stop->setPath (getIcons().fasStop, 4.4f);
    stop->setConnectedEdges (Button::ConnectedOnLeft | Button::ConnectedOnRight | Button::ConnectedOnTop | Button::ConnectedOnBottom);
    stop->addListener (this);
    stop->setIconColour (Colour (0xff4ea1ff));

    record = std::make_unique<RecordButton>();
    addAndMakeVisible (record.get());
    record->setPath (getIcons().fasCircle, 4.4f);
    record->setConnectedEdges (Button::ConnectedOnLeft | Button::ConnectedOnRight | Button::ConnectedOnTop | Button::ConnectedOnBottom);
    record->addListener (this);
    record->setIconColour (Colour (0xffff4d4d));
    record->setColour (SettingButton::backgroundOnColourId, Colours::red);

    toZero = std::make_unique<SeekZeroButton>();
    addAndMakeVisible (toZero.get());
    auto toZeroPath = getIcons().fasChevronRight;
    toZeroPath.applyTransform (AffineTransform().rotated (juce::MathConstants<float>::pi));
    toZero->setPath (toZeroPath, 4.4f);
    toZero->setConnectedEdges (Button::ConnectedOnLeft | Button::ConnectedOnRight | Button::ConnectedOnTop | Button::ConnectedOnBottom);
    toZero->addListener (this);
    toZero->setIconColour (Colours::white);

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
    /* Height-driven layout — fills whatever vertical space the parent
     * toolbar gives us (typically tempoBarHeight ≈ 26px), so transport
     * buttons read as the same family as the tempo / view-selector
     * buttons rather than a hardcoded 16px sliver.  Widths:
     *   - bar/beat/sub labels: 1.6× height (room for 2-3 digits)
     *   - transport icon buttons: 1.25× height (a bit wider than tall
     *     so the icon doesn't crowd the bold edge stroke)
     *   - 2px gaps between cells, 6px gap between the position
     *     labels and the transport buttons. */
    const int h = getHeight();
    const int labelW = juce::jmax (24, juce::roundToInt (h * 1.6f));
    const int btnW   = juce::jmax (22, juce::roundToInt (h * 1.25f));
    constexpr int gap = 2;
    constexpr int groupGap = 6;

    int x = 0;
    barLabel->setBounds  (x, 0, labelW, h); x += labelW + gap;
    beatLabel->setBounds (x, 0, labelW, h); x += labelW + gap;
    subLabel->setBounds  (x, 0, labelW, h); x += labelW + groupGap;

    play->setBounds   (x, 0, btnW, h); x += btnW + gap;
    stop->setBounds   (x, 0, btnW, h); x += btnW + gap;
    record->setBounds (x, 0, btnW, h); x += btnW + gap;
    toZero->setBounds (x, 0, btnW, h);
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
