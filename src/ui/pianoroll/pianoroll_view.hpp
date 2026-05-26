// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>
#include <element/services.hpp>

#include <functional>

namespace element {

class MidiNoteRegion;

/** Bottom-attached piano-roll editor dock.  Peer to TrackerSideDock
 *  (right-attached) -- the bottom slot was reserved for piano-roll
 *  back when the tracker editor moved to the right column.
 *
 *  Layout (inside the dock's own bounds):
 *   - Top edge: thin drag handle for VERTICAL resize.
 *   - Header:   region label ("MIDI -- <name>") + close X.
 *   - Body:     left = PianoRollKeyboard column (added Session 1 B),
 *               right = PianoRollGrid (added Session 1 C).
 *
 *  Position in StandardContent layout: removed from the BOTTOM of the
 *  inner content area AFTER `_extra` (if visible) but BEFORE the
 *  tracker side-dock width is removed from the right.  This keeps the
 *  piano roll full-width but stops short of the right tracker dock
 *  when both are visible.
 *
 *  Binding policy (Phase 3 Session 1 -- paint-only):
 *   - The dock binds to a single MidiNoteRegion by uuid.
 *   - Resolution is performed per-paint via `regionResolver_` so the
 *     dock survives region deletion / lane mutation without dangling
 *     pointers.  StandardContent installs the resolver lambda when
 *     ArrangementView::findMidiRegion lands (Session 1 commit D); in
 *     commits A/B/C the resolver is empty and the body paints the
 *     empty-state hint.
 *
 *  Session 1 is paint-only.  Session 2 adds the Ardour-style drag
 *  taxonomy + selection + undo; Session 3 adds audio-thread MIDI emit
 *  for playback through the live snapshot. */
class PianoRollView : public juce::Component
{
public:
    PianoRollView (Services& services);
    ~PianoRollView() override;

    /** Resolver hook -- returns a pointer to the live MidiNoteRegion
     *  for the given uuid, or nullptr if the region has been removed
     *  from the arrangement (lane deletion, region delete, session
     *  switch).  Called once per paint from PianoRollGrid (Session 1
     *  commit C) so the pointer lifetime is the single paint pass.
     *
     *  StandardContent installs this lambda once ArrangementView's
     *  findMidiRegion lookup is available (Session 1 commit D). */
    using RegionResolver = std::function<MidiNoteRegion* (const juce::Uuid&)>;
    void setRegionResolver (RegionResolver resolver) { regionResolver_ = std::move (resolver); }

    /** Returns the installed resolver; empty std::function means
     *  resolver has not been wired yet (Session 1 commits A/B/C). */
    const RegionResolver& getRegionResolver() const noexcept { return regionResolver_; }

    /** Bind the dock to a specific MIDI region.  Subsequent paint
     *  passes will look up the live region by this uuid.  Pass
     *  juce::Uuid::null() to clear the binding (grid then paints the
     *  empty-state hint). */
    void setRegion (const juce::Uuid& regionId);

    /** Returns the currently-bound region uuid, or null Uuid when no
     *  region is bound (initial state, or post-clear). */
    juce::Uuid getBoundRegionId() const noexcept { return boundRegionId_; }

    /** Drag-handle / close callbacks owned by StandardContent (so the
     *  parent holds the height field + layout trigger).  `deltaPx` is
     *  positive when the user is dragging DOWN (dock shrinks) and
     *  negative when dragging UP (dock grows). */
    std::function<void (int /*deltaPx*/)> onResizeDrag;
    std::function<void()>                  onResizeDragEnd;
    std::function<void()>                  onCloseClicked;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kHeaderH      = 22;
    static constexpr int kDragHandleH  = 4;
    static constexpr int kKeyboardW    = 56;

private:
    Services&   services_;
    juce::Uuid  boundRegionId_;

    RegionResolver regionResolver_;

    class DragHandle;
    std::unique_ptr<DragHandle>         dragHandle_;
    juce::Label                         regionLabel_;
    juce::TextButton                    closeBtn_ { "X" };

    void refreshLabel();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollView)
};

} // namespace element
