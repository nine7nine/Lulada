// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>
#include <element/services.hpp>
#include <element/transport.hpp>

namespace element {

class MidiNoteRegion;
class PianoRollView;

/** Note grid + bar/beat grid + viewport virtualization for the
 *  piano-roll dock.  Resolves the bound MidiNoteRegion via the
 *  PianoRollView's resolver lambda on every paint (Q3 design pick:
 *  uuid + live lookup, robust to region deletion).
 *
 *  Visual axes:
 *   - X = beats from region start, scaled by kPxPerBeat.
 *   - Y = MIDI pitch, scaled by the PianoRollKeyboard's key row
 *     height (so the grid's note rows align 1:1 with the keys to
 *     the left).
 *
 *  Session 1 scope (paint-only):
 *   - No scrolling: paints the full beat range from beat 0 forward,
 *     clipped to the grid's visible width.  If the region is wider
 *     than the dock, the tail clips silently (Session 2 adds a
 *     Viewport).
 *   - No mouse interaction (Session 2 adds the Ardour drag
 *     taxonomy).
 *   - 30 Hz juce::Timer gated on isShowing() drives repaint so
 *     external mutations to the region's note list (Session 2+)
 *     show up without manual invalidation.  Cheap: one snapshot
 *     pointer load per tick, no allocation, virtualized note paint.
 *
 *  Empty state: when no region is bound OR the resolver returns
 *  nullptr (region was deleted, resolver not yet installed in
 *  commits A/B/C), paints the hint "Double-click a MIDI region to
 *  edit." centered. */
class PianoRollGrid : public juce::Component,
                      private juce::Timer
{
public:
    PianoRollGrid (PianoRollView& parent, Services& services);
    ~PianoRollGrid() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Horizontal zoom in pixels per beat.  Bitwig default-ish; large
     *  enough for note labels in Session 2.  Session 2 will likely
     *  promote this to a per-view zoom field. */
    static constexpr int kPxPerBeat = 64;

    /** Refresh hint -- called when the bound region uuid changes so
     *  the grid invalidates immediately even before the next 30 Hz
     *  timer tick. */
    void boundRegionChanged();

private:
    PianoRollView&  parent_;
    Services&       services_;

    Transport::MonitorPtr monitor_;

    /* juce::Timer: 30 Hz repaint when isShowing().  Started in ctor,
     * stopped in dtor; the tick is cheap (early-exits when hidden)
     * so we don't bother with visibilityChanged() gymnastics. */
    void timerCallback() override;

    void paintEmptyState (juce::Graphics&);
    void paintBarGrid    (juce::Graphics&, int beatsPerBar);
    void paintNotes      (juce::Graphics&, const MidiNoteRegion& region);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollGrid)
};

} // namespace element
