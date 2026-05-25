// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>
#include <element/node.hpp>
#include <element/services.hpp>

namespace element {

class TrackerNode;
class TrackerEditor;

/** Bottom-attach tracker editor strip.  Bitwig / Ableton / Ardour
 *  pattern: lets the user keep the timeline (or any other main
 *  view) visible while editing a tracker sequence below.
 *
 *  Layout (inside the strip's own bounds):
 *   - Top edge: thin drag handle for vertical resize (mirrors the
 *     SmartLayoutResizeBar between primary + secondary in
 *     ContentContainer; we own ours since the strip lives outside
 *     the container).
 *   - Header: tracker selector dropdown (lists every TrackerNode in
 *     the active graph) + close button.
 *   - Body:   TrackerEditor for the currently-selected tracker.
 *
 *  Position in StandardContent layout:  removed from bottom after
 *  _extra (status / midi blinker overlay) but BEFORE the
 *  ContentContainer's bounds are computed.  Graph Mixer lives
 *  INSIDE the container (as its secondary view), so when both are
 *  visible the order top-to-bottom is:
 *     main view  |  graph mixer  |  tracker strip
 *  Matches the explicit user-requested ordering.
 *
 *  Selection policy:
 *   - At construction / on graph attach, picks the first
 *     TrackerNode encountered in the active graph.
 *   - setTracker(uuid) lets external code (e.g. ArrangementView
 *     clicking on a tracker clip) bind a specific tracker.
 *   - User can change via the header dropdown.
 *   - Selection persists in the session's UI state alongside the
 *     visibility / height fields. */
class TrackerStripView : public juce::Component
{
public:
    TrackerStripView (Services& services);
    ~TrackerStripView() override;

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

    /** Drag callbacks for the top-edge resize handle.  Wired to
     *  StandardContent so the parent owns the height field +
     *  layout-trigger. */
    std::function<void (int /*deltaPx*/)> onResizeDrag;
    std::function<void()>                  onResizeDragEnd;
    std::function<void()>                  onCloseClicked;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Header height (drag handle + title strip).  Constant; the
     *  TrackerEditor fills the rest. */
    static constexpr int kHeaderH       = 22;
    static constexpr int kDragHandleH   = 4;

private:
    Services& services_;
    juce::Uuid boundId_;

    /* Header components. */
    class DragHandle;
    std::unique_ptr<DragHandle>     dragHandle_;
    juce::ComboBox                  trackerCombo_;
    juce::TextButton                closeBtn_ { "X" };

    /* Body: held as unique_ptr<Component> rather than the concrete
     * TrackerEditor type so the header doesn't need to drag the
     * trackereditor full include in.  Rebuilt on each setTracker. */
    std::unique_ptr<juce::Component> editor_;

    void rebuildEditorForBound();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackerStripView)
};

} // namespace element
