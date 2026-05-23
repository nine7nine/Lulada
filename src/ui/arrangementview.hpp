// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>
#include <element/juce/data_structures.hpp>
#include <element/node.hpp>
#include <element/ui/content.hpp>
#include <element/services.hpp>
#include <element/transport.hpp>

#include "ui/blocktoolbutton.hpp"

#include "services/timeline/lane.hpp"

#define EL_VIEW_ARRANGEMENT "ArrangementView"

namespace element {

class TrackerNode;

/** Main-window arrangement view.
 *
 *  Multi-lane timeline where each lane is bound to one TrackerNode in
 *  the active graph via targetNodeUuid.  Per lane, a Playlist of
 *  Regions is laid out left-to-right; on playback the view drives
 *  each tracker via direct C++ calls (TrackerNode::advanceToPattern).
 *  Phase 2 swaps to the sample-accurate TrackerNode::schedulePlaying
 *  path -- the dispatch site is the only thing that changes.
 *
 *  Phase 1e (this commit): refactored onto the shared
 *  element::Lane / element::Playlist / element::Region data model
 *  in src/services/timeline/.  Lanes persist into the session's
 *  tags::arrangement child; pre-Phase-1e sessions (no persisted
 *  lanes) fall back to graph-walk auto-fill.
 *
 *  Authoring (drag / add / remove regions) remains TODO; the view
 *  currently auto-fills one Region per tracker pattern for any
 *  newly-discovered tracker that has no persisted lane yet.
 */
class ArrangementView : public ContentView,
                        private juce::Timer,
                        private juce::ValueTree::Listener
{
public:
    ArrangementView();
    ~ArrangementView() override;

    void initializeView (Services&) override;
    void didBecomeActive() override;
    void willBeRemoved() override;
    void stabilizeContent() override;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void valueTreeChildAdded   (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

private:
    /** Per-lane transient runtime state.  Held parallel to lanes_;
     *  rebuilt on every graph reconciliation.  NOT persisted -- only
     *  the data parts of element::Lane survive a save/load cycle. */
    struct LaneRuntimeState {
        TrackerNode* trackerCache         = nullptr;   // resolved from lane.targetNodeUuid
        juce::Uuid   lastDispatchedRegion;             // gates idempotent dispatch
        int          lastDispatchedSeqIdx = -1;        // for paint highlighting
    };

    /* Body: single inline-paint component holding all lanes (no per-
     * lane sub-component; cheap to repaint, easy to partial-invalidate). */
    class Body;
    friend class Body;

    /** Reconcile lanes_ against the current active graph + session
     *  arrangement state.  Steps:
     *   1. Load persisted lanes from tags::arrangement (first call).
     *   2. For every TrackerNode in the graph not already bound to a
     *      lane, append a new auto-filled lane.
     *   3. Rebuild laneRuntime_ pointers via graph walk by uuid.
     *  Idempotent; safe to call on graph-topology change. */
    void rescanTrackers();
    void attachToActiveGraph();
    void detachFromActiveGraph();
    void updateTransportLabel();
    double computePlayheadBeats() const;

    /** Dispatch advanceToPattern on any lane that crosses a region
     *  boundary at the given playhead beat.  Idempotent within a
     *  single region span (gated by LaneRuntimeState
     *  ::lastDispatchedRegion). */
    void dispatchAtBeat (double beat);

    /** Read lanes from the session's tags::arrangement child into
     *  lanes_.  Empty-on-first-load is normal (pre-Phase-1e
     *  sessions); rescanTrackers fills in defaults from the graph. */
    void loadLanesFromSession();
    /** Write lanes_ back into the session's tags::arrangement child.
     *  Sparse-write via Lane::toValueTree (omits default fields). */
    void writeLanesToSession();

    /** Build a default Playlist for a newly-discovered tracker --
     *  one Region per existing sequence, 4 beats each, end-to-end.
     *  Mirrors the prior v0 auto-fill behaviour. */
    void autoFillLaneForTracker (Lane& lane, TrackerNode* trk);

    /** Resolve a target node uuid to a live TrackerNode* by walking
     *  the active session graph (descending into subgraphs).  Returns
     *  nullptr when the tracker isn't in the current graph (orphaned
     *  lane -- the view paints these greyed). */
    TrackerNode* resolveTrackerByUuid (juce::Uuid targetNodeUuid) const;

    void timerCallback() override;

    Services* services_ = nullptr;
    BlockToolButton rescanBtn_ { "Rescan" };
    juce::Label posLabel_;
    juce::Label bpmLabel_;
    juce::Viewport viewport_;
    std::unique_ptr<Body> body_;

    /** Persisted lane data + parallel transient runtime state.
     *  Same length; same indices.  Mutations always update both
     *  arrays in lockstep. */
    juce::Array<Lane>              lanes_;
    juce::Array<LaneRuntimeState>  laneRuntime_;

    /** True once we've loaded any persisted lanes from the session.
     *  Gates auto-fill in rescanTrackers to "only for trackers
     *  without an existing lane." */
    bool lanesLoadedFromSession_ = false;

    /* Transport mirror — read from Transport::Monitor on the UI tick. */
    Transport::MonitorPtr monitor_;
    bool wasPlaying_ = false;
    double lastBeat_ = 0.0;

    /* Label diff state — gates juce::Label::setText so we don't keep
     * burning string allocations on idle ticks. */
    float lastBpmShown_ = -1.0f;
    double lastBeatShown_ = -999.0;

    /* Active graph we're attached to as a ValueTree listener — pulled
     * from session->getActiveGraph().data() when the view is opened. */
    juce::ValueTree attachedGraphTree_;

    static constexpr int kHeaderH = 36;
    static constexpr int kLaneH   = 64;
    static constexpr int kLabelW  = 160;
    static constexpr int kPxPerBeat = 24;
};

} // namespace element
