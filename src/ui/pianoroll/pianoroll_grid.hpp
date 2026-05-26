// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>
#include <element/services.hpp>
#include <element/transport.hpp>

#include <memory>
#include <unordered_set>
#include <vector>

namespace element {

class MidiNoteRegion;
class PianoRollView;
class NoteDrag;
struct MidiNote;

/** Note grid + bar/beat grid + viewport virtualization + edit
 *  gestures for the piano-roll dock.
 *
 *  ## Sizing
 *
 *  PianoRollGrid sits inside a juce::Viewport managed by
 *  PianoRollView.  The grid's own width = boundRegionLengthBeats *
 *  pxPerBeat_; height = viewport visible-height (no Y scroll).  When
 *  the bound region changes, updateSizeForRegion is called -- it
 *  computes auto-fit pxPerBeat (so the whole region fits in the
 *  viewport at first bind) + sets the grid's size accordingly.  The
 *  viewport's horizontal scrollbar engages when the user zooms in
 *  enough that the region overflows the visible width.
 *
 *  ## Zoom
 *
 *  Mouse-wheel (cmd / ctrl + scroll) scales pxPerBeat in [4, 256].
 *  Auto-fit on first bind picks a zoom that shows the full region.
 *
 *  ## Tools + gestures
 *
 *  Active tool is read from PianoRollView::getActiveTool() per
 *  mouseDown.  Branches:
 *   - Select on note body            -> NoteDragMove
 *   - Select on note right edge      -> NoteDragResize
 *   - Select on empty area           -> NoteDragMarquee (rubber-band)
 *   - Pencil on empty area           -> NoteDragCreate
 *   - Pencil on note body            -> NoteDragMove (drag existing)
 *   - Erase on note body             -> remove note (commit on mouseUp)
 *   - Erase on empty area            -> NoteDragMarquee (erase-rubber-band)
 *
 *  Each NoteDrag is owned by the grid for the duration of the
 *  gesture; on mouseUp the drag's commit() method runs and pushes a
 *  MidiNoteDiffCommand onto the global UndoManager.
 *
 *  ## Selection
 *
 *  selectedNoteIds_ stores stable MidiNote ids (the `id` field
 *  introduced in Session 2).  Selection survives snapshot swaps
 *  because ids are sticky across publishes.  Click on a note adds /
 *  replaces selection; ctrl-click toggles; marquee selects all in
 *  range.  Delete key removes selected notes.
 *
 *  ## Repaint
 *
 *  30 Hz juce::Timer gated on isShowing() drives repaint when
 *  external mutations (e.g. transport playhead, future MIDI live-
 *  record) change the note set.  User-driven edits trigger
 *  immediate repaint via boundRegionChanged / commit paths. */
class PianoRollGrid : public juce::Component,
                      private juce::Timer
{
public:
    PianoRollGrid (PianoRollView& parent, Services& services);
    ~PianoRollGrid() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Called by PianoRollView when the bound region uuid changes
     *  (post-setRegion). */
    void boundRegionChanged();

    /** Called by PianoRollView when the active tool changes. */
    void activeToolChanged();

    /** Called by PianoRollView::resized when the surrounding
     *  viewport's visible bounds change.  visibleW / visibleH are
     *  the viewport's content-area dimensions; on first bind this
     *  drives auto-fit kPxPerBeat selection. */
    void updateSizeForViewport (int visibleW, int visibleH);

    //==========================================================================
    // Mouse handlers (juce::Component).

    void mouseDown      (const juce::MouseEvent&) override;
    void mouseDrag      (const juce::MouseEvent&) override;
    void mouseUp        (const juce::MouseEvent&) override;
    void mouseMove      (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&,
                          const juce::MouseWheelDetails&) override;

    bool keyPressed     (const juce::KeyPress&) override;

    /** Horizontal zoom in pixels per beat.  Mutated by mouseWheelMove
     *  + auto-fit-on-bind.  Public so NoteDrag classes can read it
     *  for beat <-> pixel conversions. */
    int  getPxPerBeat() const noexcept { return pxPerBeat_; }
    void setPxPerBeat (int newPxPerBeat);

    static constexpr int kPxPerBeatMin = 4;
    static constexpr int kPxPerBeatMax = 256;

    /** Zoom step API for toolbar +/- buttons + Fit.  Anchored zoom
     *  (mouseWheelMove) handles its own pivot; these versions zoom
     *  around the viewport's visible centre so the user keeps roughly
     *  the same beat range in view. */
    void zoomBy   (double factor);
    void zoomToFit();

    /** Snap-to-grid resolution in beats.  0.0 = no snap (continuous).
     *  Standard musical values: 0.25 = sixteenth, 0.5 = eighth, 1.0 =
     *  quarter, 2.0 = half, 4.0 = bar (assumes 4/4).  Default = 0.25
     *  (sixteenth) to match Ableton + Zrythm.  Driven by the toolbar
     *  snap selector in PianoRollView. */
    void   setSnapDivision (double beats) noexcept;
    double getSnapDivision() const noexcept { return snapDivision_; }
    void   setSnapEnabled  (bool b) noexcept;
    bool   isSnapEnabled() const noexcept { return snapEnabled_; }

    /** Ruler height (bar/beat strip painted across the top of the
     *  grid).  Public so PianoRollKeyboard can offset its keys to
     *  match -- the keyboard column needs an equivalent gap so the
     *  first pitch row aligns with the grid's first pitch row. */
    static constexpr int kRulerH = 18;

    //==========================================================================
    // Coordinate helpers used by NoteDrag classes.

    /** Convert a grid-local X pixel to region-local beats (>= 0).
     *  Clamps to lengthBeats so notes can't drop past the region's
     *  tail. */
    double beatForX (int x, double lengthBeatsClamp) const noexcept;

    /** Convert a grid-local Y pixel to a MIDI pitch.  Returns the
     *  pitch row whose vertical band contains y (or the nearest if
     *  out of range). */
    int    pitchForY (int y) const noexcept;

    /** Region-local beat -> grid X pixel. */
    int    xForBeat (double localBeat) const noexcept;

    /** Pitch -> grid Y pixel (top edge of the pitch row). */
    int    yForPitch (int pitch) const noexcept;

    /** Pitch row height in pixels.  Derived from grid height /
     *  visible pitch span. */
    int    rowHeight() const noexcept;

    /** Snap a local beat to the current grid resolution.  Returns the
     *  input unchanged when snap is disabled or `snapDivision_` is 0;
     *  otherwise rounds to the nearest multiple of `snapDivision_`. */
    double snapBeat (double localBeat) const noexcept;

    //==========================================================================
    // Selection API.

    bool isSelected (std::uint64_t noteId) const noexcept
    {
        return selectedNoteIds_.count (noteId) > 0;
    }

    void selectOnly         (std::uint64_t noteId);
    void selectToggle       (std::uint64_t noteId);
    void selectAdd          (std::uint64_t noteId);
    void selectClear();
    const std::unordered_set<std::uint64_t>& selection() const noexcept
    {
        return selectedNoteIds_;
    }

    //==========================================================================
    // Resolved-region access.  Returns nullptr if the bound uuid
    // can't be resolved.

    MidiNoteRegion* resolveBoundRegion() const noexcept;

    /** Services accessor for NoteDrag commits (UndoManager lookup
     *  via GuiService).  Lifetime guaranteed for the duration of
     *  PianoRollGrid. */
    Services& getServices() const noexcept { return services_; }

private:
    PianoRollView&  parent_;
    Services&       services_;
    Transport::MonitorPtr monitor_;

    int             pxPerBeat_   { 64 };
    /* Cached region length from the last bind / region edit; used
     * to size the grid component and clamp drag coordinates.  Read
     * lazily via resolveBoundRegion->lengthBeats on bind. */
    double          regionLenBeats_ { 16.0 };

    /* Snap resolution + on/off.  Set by PianoRollView's toolbar
     * snapBox + snap toggle button.  Default sixteenth-note snap
     * matches Ableton + Zrythm defaults. */
    double          snapDivision_ { 0.25 };
    bool            snapEnabled_  { true };

    /* Viewport visible-height cache.  Set by updateSizeForViewport;
     * height of one pitch row = visibleH / span. */
    int             visibleH_ { 1 };

    /* Selection -- stable note ids.  std::unordered_set so the
     * O(1) lookup is fast even with thousands of notes selected. */
    std::unordered_set<std::uint64_t> selectedNoteIds_;

    /* Active drag gesture.  Lifetime is one mouse-down-to-mouse-up
     * cycle.  Cleared on mouseUp after commit. */
    std::unique_ptr<NoteDrag> activeDrag_;

    void timerCallback() override;

    void paintEmptyState    (juce::Graphics&);
    void paintBarGrid       (juce::Graphics&, int beatsPerBar);
    void paintRuler         (juce::Graphics&, int beatsPerBar);
    void paintPlayhead      (juce::Graphics&);
    void paintNotes         (juce::Graphics&, const MidiNoteRegion& region);
    void paintActiveDragOverlay (juce::Graphics&);

    /** Body rectangle (everything below the ruler).  Note paint + hit
     *  test work in body-local Y; the ruler claims the top kRulerH px. */
    juce::Rectangle<int> bodyBounds() const noexcept
    {
        return { 0, kRulerH, getWidth(), juce::jmax (1, getHeight() - kRulerH) };
    }

    /** Hit test -- returns the note id under (x, y) or 0 if none. */
    std::uint64_t hitTestNote (int x, int y, const MidiNoteRegion& region) const noexcept;

    /** True if (x, y) is within the right-edge resize handle (3 px)
     *  of any note. */
    std::uint64_t hitTestResizeHandle (int x, int y, const MidiNoteRegion& region) const noexcept;

    /** Compute the visible viewport rectangle in this grid's local
     *  coords.  Drives virtualization for the paint loop. */
    juce::Rectangle<int> visibleRect() const noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollGrid)
};

} // namespace element
