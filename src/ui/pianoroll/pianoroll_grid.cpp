// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/pianoroll/pianoroll_grid.hpp"
#include "ui/pianoroll/pianoroll_view.hpp"
#include "ui/pianoroll/pianoroll_keyboard.hpp"
#include "ui/fontcache.hpp"
#include "services/timeline/midi_note_region.hpp"
#include "services/timeline/midi_note.hpp"

#include <element/audioengine.hpp>
#include <element/context.hpp>

namespace element {

PianoRollGrid::PianoRollGrid (PianoRollView& parent, Services& services)
    : parent_ (parent), services_ (services)
{
    if (auto* eng = services.context().audio().get())
        monitor_ = eng->getTransportMonitor();

    /* 30 Hz isShowing()-gated repaint.  Q2 design pick -- matches
     * existing ArrangementView pattern; cheap because timerCallback
     * early-exits when not showing.  Session 2's edit-loop boundary
     * may swap this for a snapshot-version short-circuit. */
    startTimerHz (30);
}

PianoRollGrid::~PianoRollGrid()
{
    stopTimer();
}

void PianoRollGrid::boundRegionChanged()
{
    repaint();
}

void PianoRollGrid::timerCallback()
{
    if (! isShowing()) return;
    repaint();
}

void PianoRollGrid::resized()
{
    /* Nothing to lay out -- the grid paints directly into
     * getLocalBounds().  Session 2 may add a child Viewport for
     * horizontal scrolling. */
}

void PianoRollGrid::paint (juce::Graphics& g)
{
    /* Backdrop -- darker than the dock body so the keyboard column
     * reads as a separator strip on the left edge. */
    g.fillAll (juce::Colour (0xff'0a'0a'0a));

    /* Resolve the bound region via PianoRollView's resolver lambda
     * (Q3 design pick: uuid + live lookup, per paint).  Resolver is
     * empty in Session 1 commits A/B/C -- installed in commit D once
     * ArrangementView::findMidiRegion is available. */
    const auto& resolver = parent_.getRegionResolver();
    const auto regionId  = parent_.getBoundRegionId();
    MidiNoteRegion* region = nullptr;
    if (resolver && ! regionId.isNull())
        region = resolver (regionId);

    /* Bar/beat grid is drawn whether or not we have a region -- gives
     * the user visual context even with the empty-state hint up. */
    const int beatsPerBar = monitor_ != nullptr
        ? juce::jmax (1, (int) monitor_->beatsPerBar.get())
        : 4;
    paintBarGrid (g, beatsPerBar);

    if (region == nullptr)
    {
        paintEmptyState (g);
        return;
    }

    paintNotes (g, *region);
}

void PianoRollGrid::paintEmptyState (juce::Graphics& g)
{
    g.setColour (juce::Colours::white.withAlpha (0.35f));
    g.setFont (monoFont (12.0f, juce::Font::plain));
    g.drawText ("Double-click a MIDI region to edit.",
                getLocalBounds(),
                juce::Justification::centred,
                false);
}

void PianoRollGrid::paintBarGrid (juce::Graphics& g, int beatsPerBar)
{
    const int w = getWidth();
    const int h = getHeight();
    if (w <= 0 || h <= 0) return;

    /* Compute the maximum visible beat: the right edge of the grid
     * in beat coords.  We paint vertical lines for every beat
     * boundary in the visible range, with bar boundaries drawn
     * brighter so they stand out. */
    const int maxBeat = (w + kPxPerBeat - 1) / kPxPerBeat;

    /* Beat lines (faint). */
    g.setColour (juce::Colour (0xff'18'18'18));
    for (int beat = 1; beat <= maxBeat; ++beat)
    {
        const int x = beat * kPxPerBeat;
        if (x >= w) break;
        if ((beat % beatsPerBar) == 0) continue;  /* bar line below */
        g.drawVerticalLine (x, 0.0f, (float) h);
    }

    /* Bar lines (brighter). */
    g.setColour (juce::Colour (0xff'2a'2a'2a));
    for (int beat = beatsPerBar; beat <= maxBeat; beat += beatsPerBar)
    {
        const int x = beat * kPxPerBeat;
        if (x >= w) break;
        g.drawVerticalLine (x, 0.0f, (float) h);
    }

    /* Pitch-row separators -- one horizontal line per visible pitch
     * (semitone-equal rows, matching the standard piano-roll
     * convention).  Reads the visible pitch range from the keyboard
     * column so the rows stay in sync with the keyboard view. */
    auto* kb = parent_.getKeyboard();
    if (kb != nullptr)
    {
        const int lo = kb->getLowestVisibleNoteNumber();
        const int hi = kb->getHighestVisibleNoteNumber();
        const int span = juce::jmax (1, hi - lo + 1);
        const float rowH = (float) h / (float) span;

        /* Octave-boundary lines a bit brighter than semitone lines so
         * the eye finds the octaves quickly. */
        g.setColour (juce::Colour (0xff'14'14'14));
        for (int p = lo; p <= hi; ++p)
        {
            const float y = (float) (hi - p) * rowH;
            if ((p % 12) == 0)
                g.setColour (juce::Colour (0xff'22'22'22));
            else
                g.setColour (juce::Colour (0xff'14'14'14));
            g.drawHorizontalLine ((int) y, 0.0f, (float) w);
        }
    }
}

void PianoRollGrid::paintNotes (juce::Graphics& g, const MidiNoteRegion& region)
{
    const auto* snap = region.loadSnapshot();
    if (snap == nullptr || snap->empty()) return;

    auto* kb = parent_.getKeyboard();
    if (kb == nullptr) return;

    const int lo = kb->getLowestVisibleNoteNumber();
    const int hi = kb->getHighestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    const float rowH = (float) getHeight() / (float) span;

    /* Visible viewport: the local grid bounds.  Used to clip-test
     * notes so off-screen entries don't pay paint cost.  Mirrors the
     * ArrangementView region paint at arrangementview.cpp:2375. */
    const juce::Rectangle<int> viewport = getLocalBounds();

    /* Note fill colour: derive from the region's own colour so MIDI
     * regions keep visual identity across the arrangement strip and
     * the piano-roll body.  Brightness lifted slightly so individual
     * notes pop against the dark backdrop. */
    const juce::Colour noteFill = region.colour
                                          .withMultipliedBrightness (1.2f);
    const juce::Colour noteEdge = region.colour
                                          .withMultipliedBrightness (1.6f);

    for (const auto& n : *snap)
    {
        if (n.pitch < lo || n.pitch > hi) continue;

        const int x  = (int) (n.onBeat * kPxPerBeat);
        const int w  = juce::jmax (2, (int) (n.lengthBeats * kPxPerBeat));
        const int y  = (int) ((float) (hi - n.pitch) * rowH);
        const int hh = juce::jmax (1, (int) rowH - 1);

        const juce::Rectangle<int> rect (x, y, w, hh);
        if (! viewport.intersects (rect)) continue;

        g.setColour (noteFill);
        g.fillRect (rect);
        g.setColour (noteEdge);
        g.drawRect (rect, 1);
    }
}

} // namespace element
