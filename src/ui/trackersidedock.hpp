// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>
#include <element/node.hpp>
#include <element/services.hpp>

namespace element {

class TrackerNode;
class TrackerEditor;

/** Right-side tracker editor dock.  Mirrors a tracker's natural
 *  vertical orientation (rows flow downward, time forward) by
 *  docking the editor to the right edge of the main window where it
 *  has full-height room for the row grid.
 *
 *  Layout (inside the dock's own bounds):
 *   - Left edge: thin drag handle for HORIZONTAL resize.
 *   - Header: tracker selector dropdown (lists every TrackerNode in
 *     the active graph) + close button.
 *   - Body:   TrackerEditor for the currently-selected tracker.
 *
 *  Position in StandardContent layout: removed from the RIGHT after
 *  the node-channel-strip (if visible) but BEFORE the
 *  ContentContainer's bounds are computed.  This leaves the bottom
 *  strip area free for a future piano-roll editor (which IS time-
 *  horizontal and belongs there).
 *
 *  Selection policy:
 *   - At construction / on graph attach, picks the first
 *     TrackerNode encountered in the active graph.
 *   - setTracker(uuid) lets external code (e.g. ArrangementView
 *     clicking on a tracker clip) bind a specific tracker.
 *   - User can change via the header dropdown.
 *   - Selection persists in the session's UI state alongside the
 *     visibility / width fields. */
class TrackerSideDock : public juce::Component,
                         private juce::ValueTree::Listener
{
public:
    TrackerSideDock (Services& services);
    ~TrackerSideDock() override;

    /** Bind to a specific TrackerNode by uuid.  No-op if the uuid
     *  doesn't resolve to a TrackerNode in the active graph; in that
     *  case the previous tracker stays bound. */
    void setTracker (const juce::Uuid& nodeId);

    /** Bind to nodeId AND navigate the embedded TrackerEditor to
     *  `sequenceIdx` (pass -1 to leave the current pattern alone).
     *  Used by ArrangementView's tracker-clip double-click so the
     *  user lands on the exact pattern they clicked, not pattern 0. */
    void setTrackerAndPattern (const juce::Uuid& nodeId, int sequenceIdx);

    /** Returns the currently-bound TrackerNode's uuid, or a null
     *  uuid when nothing is bound (no tracker in the graph). */
    juce::Uuid getBoundTrackerId() const noexcept { return boundId_; }

    /** Called by StandardContent when the active graph changes
     *  (session reload, graph switch, new TrackerNode added).
     *  Refreshes the selector + falls back to the first tracker
     *  available if the bound one disappeared. */
    void refreshFromGraph();

    /** Drag callbacks for the left-edge resize handle.  Wired to
     *  StandardContent so the parent owns the width field +
     *  layout-trigger.  `deltaPx` is positive when the user is
     *  dragging RIGHT (shrinking the dock) and negative when
     *  dragging LEFT (widening it). */
    std::function<void (int /*deltaPx*/)> onResizeDrag;
    std::function<void()>                  onResizeDragEnd;
    std::function<void()>                  onCloseClicked;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kHeaderH     = 22;
    static constexpr int kDragHandleW = 4;

private:
    Services& services_;
    juce::Uuid boundId_;

    class DragHandle;
    std::unique_ptr<DragHandle>     dragHandle_;
    juce::ComboBox                  trackerCombo_;
    juce::TextButton                closeBtn_ { "X" };

    /* Body: held as unique_ptr<Component> rather than the concrete
     * TrackerEditor type so the header doesn't need to drag the
     * trackereditor full include in.  Rebuilt on each setTracker. */
    std::unique_ptr<juce::Component> editor_;

    juce::ValueTree watchedNodes_;

    void rebuildEditorForBound();
    void attachToActiveGraph();

    /* ValueTree::Listener -- defer the refresh via callAsync to avoid
     * re-entrancy on the VT mutation that triggered us. */
    void valueTreeChildAdded   (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackerSideDock)
};

} // namespace element
