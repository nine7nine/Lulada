// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <element/session.hpp>

#include "ui/blocktoolbutton.hpp"
#include "ui/guicommon.hpp"
#include "ui/toolbaricons.hpp"
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
    /* Transport cluster rebuilt on BlockToolButton so the icons
     * render in the same vector-glyph family as the view selector +
     * undo/redo (consistent inset, gradient, hover behaviour).
     * Bright orange Play, warm-white Stop, bright red Record,
     * soft-white SeekZero.  Active tints push the body wash so the
     * armed/playing state stays clearly visible. */
    auto setIcon = [] (BlockToolButton& b,
                        void (*fn)(juce::Graphics&, juce::Rectangle<float>, juce::Colour))
    {
        b.setIcon ([fn] (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fg)
                   { fn (g, r, fg); });
    };

    auto playB = std::make_unique<BlockToolButton> ("", Colour (0xff'ff'8a'30));
    setIcon (*playB, &ui::iconPlay);
    playB->setActiveTint (Colour (0xff'5a'2d'10));
    playB->addListener (this);
    play = std::move (playB);
    addAndMakeVisible (play.get());

    auto stopB = std::make_unique<BlockToolButton> ("", Colour (0xff'b0'b0'b0));
    setIcon (*stopB, &ui::iconStop);
    stopB->addListener (this);
    stop = std::move (stopB);
    addAndMakeVisible (stop.get());

    auto recB = std::make_unique<BlockToolButton> ("", Colour (0xff'ff'4d'4d));
    setIcon (*recB, &ui::iconRecord);
    recB->setActiveTint (Colour (0xff'7a'1a'1a));
    recB->addListener (this);
    record = std::move (recB);
    addAndMakeVisible (record.get());

    auto seekB = std::make_unique<BlockToolButton> ("", Colour (0xff'9a'9a'9a));
    setIcon (*seekB, &ui::iconSeekZero);
    seekB->addListener (this);
    toZero = std::move (seekB);
    addAndMakeVisible (toZero.get());

    barLabel = std::make_unique<BarLabel> (*this);
    addAndMakeVisible (barLabel.get());
    barLabel->setName ("barLabel");

    beatLabel = std::make_unique<BeatLabel>();
    addAndMakeVisible (beatLabel.get());
    beatLabel->setName ("beatLabel");

    subLabel = std::make_unique<SubBeatLabel>();
    addAndMakeVisible (subLabel.get());
    subLabel->setName ("subLabel");

    /* LCD sub-labels under each transport button.  Visible by default;
     * showPositionLabels mode (legacy bar/beat/sub layout) hides them
     * since vertical space is already consumed by the position
     * read-outs. */
    addAndMakeVisible (playLbl_);
    addAndMakeVisible (stopLbl_);
    addAndMakeVisible (recordLbl_);
    addAndMakeVisible (toZeroLbl_);

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
    /* LCD-style faceplate frame matching MainDisplayPanel.  Outer
     * matte-black bezel + cool-grey vertical gradient inside.  The
     * 4 transport buttons sit on top of this inset, so the whole
     * cluster reads as one hardware-style panel just like the
     * central digital display. */
    const auto frect = getLocalBounds().toFloat();

    g.setColour (juce::Colour (0xff'08'08'08));
    g.fillRoundedRectangle (frect, 4.0f);
    g.setColour (juce::Colour (0xff'3a'3a'3a));
    g.drawRoundedRectangle (frect.reduced (0.5f), 4.0f, 1.0f);

    const auto inner = frect.reduced (3.0f);
    juce::ColourGradient lcdGrad (
        juce::Colour (0xff'14'19'1e),
        inner.getX(), inner.getY(),
        juce::Colour (0xff'0c'0f'12),
        inner.getX(), inner.getBottom(),
        false);
    g.setGradientFill (lcdGrad);
    g.fillRoundedRectangle (inner, 3.0f);
}

void TransportBar::resized()
{
    /* Children inset by kFramePad on every side so the LCD frame
     * painted in paint() stays visible around them.  Each button is
     * a square with a 4-px gap between siblings. */
    constexpr int kFramePad     = 5;
    constexpr int kSublabelH    = 13;
    constexpr int kSublabelGap  = 3;
    const int outerH = getHeight();
    const int innerH = juce::jmax (16, outerH - kFramePad * 2);

    /* Sublabels visible only in the buttons-only mode (the Content::
     * Toolbar configuration).  In showPositionLabels mode we keep the
     * legacy full-height buttons; there isn't room for labels there.
     * No upper cap on btnH -- main clusters scale with the toolbar. */
    const bool useSublabels = ! showPositionLabels_;
    const int  btnH = useSublabels
        ? juce::jmax (16, innerH - kSublabelH - kSublabelGap)
        : innerH;
    const int  btnW = btnH;
    /* Centre the icon+label stack vertically so the cluster looks
     * balanced inside the taller frame. */
    const int  stackH = useSublabels ? btnH + kSublabelGap + kSublabelH : btnH;
    const int  stackY = kFramePad + juce::jmax (0, (innerH - stackH) / 2);
    const int  lblY  = stackY + btnH + kSublabelGap;

    const int labelW = juce::jmax (24, juce::roundToInt (innerH * 1.6f));
    constexpr int gap = 4;
    constexpr int groupGap = 10;

    int x = kFramePad;
    const int y = kFramePad;
    if (showPositionLabels_)
    {
        barLabel->setBounds  (x, y, labelW, innerH); x += labelW + 2;
        beatLabel->setBounds (x, y, labelW, innerH); x += labelW + 2;
        subLabel->setBounds  (x, y, labelW, innerH); x += labelW + groupGap;
    }

    auto place = [&] (juce::Button* b, LcdSublabel& lbl)
    {
        b->setBounds (x, stackY, btnW, btnH);
        if (useSublabels)
        {
            lbl.setVisible (true);
            lbl.setBounds (x, lblY, btnW, kSublabelH);
        }
        else
        {
            lbl.setVisible (false);
        }
        x += btnW + gap;
    };
    place (play.get(),   playLbl_);
    place (stop.get(),   stopLbl_);
    place (record.get(), recordLbl_);
    place (toZero.get(), toZeroLbl_);
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
    /* +5 px on the right so the LCD frame's right padding matches
     * the left.  kFramePad in resized() is the same value. */
    setSize (toZero->getRight() + 5, getHeight());
}

} // namespace element
