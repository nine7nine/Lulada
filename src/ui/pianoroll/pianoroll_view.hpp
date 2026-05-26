// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ui/blocktoolbutton.hpp"

#include <element/juce/gui_basics.hpp>
#include <element/services.hpp>

#include <functional>

namespace element {

class MidiNoteRegion;
class PianoRollKeyboard;
class PianoRollGrid;

/** Bottom-attached piano-roll editor dock.  Peer to TrackerSideDock
 *  (right-attached) -- the bottom slot was reserved for piano-roll
 *  back when the tracker editor moved to the right column.
 *
 *  Layout (inside the dock's own bounds):
 *   - Top edge:   thin drag handle for VERTICAL resize.
 *   - Header:     region label ("MIDI -- <filename>") + tool palette
 *                 (Select / Pencil / Erase) + close X.
 *   - Body:       left = PianoRollKeyboard column (fixed width),
 *                 right = juce::Viewport hosting the PianoRollGrid
 *                 (full region beat span, horizontal scroll + zoom).
 *
 *  Tool palette: radio-toggle on the header, default Select.  The
 *  active tool is stored on PianoRollView and read by PianoRollGrid's
 *  mouse handlers via getActiveTool().
 *
 *  Region binding: per-paint resolver lookup (Q3 design pick from
 *  Session 1).  See setRegionResolver. */
class PianoRollView : public juce::Component
{
public:
    enum class Tool : int { Select = 0, Pencil = 1, Erase = 2 };

    PianoRollView (Services& services);
    ~PianoRollView() override;

    /** Resolver hook -- returns a pointer to the live MidiNoteRegion
     *  for the given uuid, or nullptr.  Called once per paint from
     *  PianoRollGrid + once per gesture commit. */
    using RegionResolver = std::function<MidiNoteRegion* (const juce::Uuid&)>;
    void setRegionResolver (RegionResolver resolver) { regionResolver_ = std::move (resolver); }
    const RegionResolver& getRegionResolver() const noexcept { return regionResolver_; }

    /** Bind the dock to a specific MIDI region. */
    void setRegion (const juce::Uuid& regionId);
    juce::Uuid getBoundRegionId() const noexcept { return boundRegionId_; }

    /** Accessor for the keyboard column. */
    PianoRollKeyboard* getKeyboard() const noexcept { return keyboard_.get(); }

    /** Accessor for the grid (used by gesture-commit code to invoke
     *  region mutation and undo-push paths). */
    PianoRollGrid* getGrid() const noexcept { return grid_.get(); }

    /** Active tool selection.  Mouse handlers in the grid branch on
     *  this; selection paint highlights respect it too. */
    Tool getActiveTool() const noexcept { return activeTool_; }
    void setActiveTool (Tool t);

    /** Drag-handle / close callbacks owned by StandardContent. */
    std::function<void (int /*deltaPx*/)> onResizeDrag;
    std::function<void()>                  onResizeDragEnd;
    std::function<void()>                  onCloseClicked;

    /** Fired after every region-mutating gesture commit (Move, Create,
     *  Resize, Erase, Delete-key) on the bound region.  StandardContent
     *  wires this to ArrangementView::body_->repaint() so the lane
     *  strip's note-count badge tracks live edits. */
    std::function<void()>                  onRegionEdited;
    void notifyRegionEdited()
    {
        if (onRegionEdited) onRegionEdited();
    }

    void paint (juce::Graphics&) override;
    void resized() override;

    /* Layout constants:
     *   kDragHandleH   thin top edge for vertical resize.
     *   kHeaderH       toolbar row (tool palette + snap + zoom + close).
     *   kKeyboardW     left-side piano key column width.
     */
    static constexpr int kHeaderH      = 28;
    static constexpr int kDragHandleH  = 4;
    static constexpr int kKeyboardW    = 56;
    static constexpr int kToolBtnW     = 56;
    static constexpr int kZoomBtnW     = 24;
    static constexpr int kSnapBoxW     = 70;

private:
    Services&    services_;
    juce::Uuid   boundRegionId_;
    Tool         activeTool_ { Tool::Select };

    RegionResolver regionResolver_;

    class DragHandle;
    std::unique_ptr<DragHandle>         dragHandle_;
    juce::Label                         regionLabel_;
    BlockToolButton                     closeBtn_       { "X" };
    BlockToolButton                     selectBtn_      { "Select" };
    BlockToolButton                     pencilBtn_      { "Pencil" };
    BlockToolButton                     eraseBtn_       { "Erase" };
    BlockToolButton                     snapBtn_        { "Snap" };
    juce::ComboBox                      snapBox_;
    BlockToolButton                     zoomOutBtn_     { "-" };
    BlockToolButton                     zoomInBtn_      { "+" };
    BlockToolButton                     zoomFitBtn_     { "Fit" };
    /* Y zoom: shrink / grow visible pitch span around the centre.
     * Independent of the X zoom triplet -- piano-roll users routinely
     * change Y span (more octaves) and X span (more beats) on different
     * axes during edits. */
    BlockToolButton                     yZoomOutBtn_    { "Y-" };
    BlockToolButton                     yZoomInBtn_     { "Y+" };

    std::unique_ptr<PianoRollKeyboard>  keyboard_;
    /** juce::Viewport hosting the grid.  Horizontal scrolling only --
     *  the keyboard column and grid both fill the dock's body height. */
    std::unique_ptr<juce::Viewport>     gridViewport_;
    std::unique_ptr<PianoRollGrid>      grid_;

    void refreshLabel();
    void syncToolToggles();
    void applySnapFromComboBox();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollView)
};

} // namespace element
