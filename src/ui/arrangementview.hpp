// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>
#include <element/juce/data_structures.hpp>
#include <element/juce/audio_formats.hpp>
/* AudioThumbnail / AudioThumbnailCache live in juce_audio_utils;
 * Element doesn't ship an element/juce/audio_utils.hpp wrapper but
 * the module is linked (see src/CMakeLists.txt) so direct include
 * via the JUCE module path works. */
#include <juce_audio_utils/juce_audio_utils.h>
#include <element/node.hpp>
#include <element/ui/content.hpp>
#include <element/services.hpp>
#include <element/transport.hpp>

#include "ui/blocktoolbutton.hpp"

#include "services/timeline/audiolaneadapter.hpp"
#include "services/timeline/lane.hpp"

#define EL_VIEW_ARRANGEMENT "ArrangementView"

namespace element {

class AudioClipNode;
class TrackerNode;

/** Main-window arrangement view.
 *
 *  Multi-lane timeline.  Each Lane binds to one target graph node
 *  (TrackerNode or AudioClipNode) via Lane.targetNodeUuid.  Per-lane
 *  paint + dispatch branch on the resolved target type:
 *
 *   - TrackerNode lanes: regions reference vht sequences; click /
 *     dispatch routes through TrackerNode::schedulePlaying (the same
 *     audio-thread SPSC FIFO SessionView clip-launch uses).
 *   - AudioClipNode lanes: regions reference AudioFileSources via
 *     SourceRegistry; click / dispatch routes through AudioClipNode::
 *     schedulePlay (its own SPSC launch FIFO).
 *
 *  External file drop (juce::FileDragAndDropTarget):
 *   - Drop on an existing audio lane row -> append Region at drop X
 *   - Drop on a tracker lane or empty area -> create a new audio lane
 *     in the ArrangementTracks subgraph, append Region.
 *
 *  Record path:
 *   - Per-lane record-arm toggle (Lane.armed) propagates to the
 *     bound AudioClipNode::setArmed.  Transport-record + armed
 *     triggers capture inside AudioClipNode.  On capture finish,
 *     AudioClipNode invokes onRecordingCommitted with the new file;
 *     ArrangementView registers the file as an AudioFileSource and
 *     appends a Region to the lane's Playlist.
 *
 *  Detection still runs on the 30 Hz UI timer; the actual sequence
 *  flip is sample-accurate within one render block.  Same path
 *  SessionView clip-launch uses (timeline-audio-design.md §2.3).
 */
class ArrangementView : public ContentView,
                        public juce::FileDragAndDropTarget,
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

    /* FileDragAndDropTarget at the outer ContentView level -- belt
     * and suspenders alongside the inner Body's same interface.  X11
     * XDND routing under winelib (juce_DragAndDrop_linux.cpp via
     * winelib_compat.h undefining _WIN32) walks the component tree
     * from peer->getComponent() down to the deepest component at
     * the drop point that implements FileDragAndDropTarget.  Body
     * lives inside a juce::Viewport; covering both the outer view
     * and the inner Body guarantees something always hits. */
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;
    void fileDragEnter (const juce::StringArray&, int x, int y) override;
    void fileDragExit (const juce::StringArray&) override;

private:
    void valueTreeChildAdded   (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

private:
    /** Per-lane transient runtime state.  Held parallel to lanes_;
     *  rebuilt on every graph reconciliation.  NOT persisted.
     *
     *  Resolved kind is implicit: exactly one of trackerCache /
     *  audioClipCache is non-null for live lanes; both null for
     *  orphan lanes whose target node was deleted from the graph. */
    struct LaneRuntimeState
    {
        TrackerNode*     trackerCache    = nullptr;
        AudioClipNode*   audioClipCache  = nullptr;
        AudioLaneAdapter audioAdapter    { nullptr };   // target rebound on each rescan
        juce::Uuid       lastDispatchedRegion;
        int              lastDispatchedSeqIdx = -1;

        bool isTrackerLane() const noexcept { return trackerCache   != nullptr; }
        bool isAudioLane()   const noexcept { return audioClipCache != nullptr; }
        bool isOrphan()      const noexcept { return ! isTrackerLane() && ! isAudioLane(); }
    };

    /* Body: single inline-paint component holding all lanes (no per-
     * lane sub-component; cheap to repaint, easy to partial-invalidate). */
    class Body;
    friend class Body;

    /** Reconcile lanes_ against the current active graph + session
     *  arrangement state.  Steps:
     *   1. Load persisted lanes from tags::arrangement (first call).
     *   2. For every TrackerNode in the graph not already bound to a
     *      lane, append a new auto-filled tracker lane.
     *   3. Rebuild laneRuntime_ caches via graph walk by uuid.  Each
     *      lane gets its trackerCache or audioClipCache populated
     *      from the resolved target node.
     *   4. For audio-clip lanes, rebind the embedded AudioLaneAdapter
     *      to the resolved node + wire its recording-committed handler.
     *  Idempotent; safe to call on graph-topology change. */
    void rescanLaneTargets();

    void attachToActiveGraph();
    void detachFromActiveGraph();
    void updateTransportLabel();
    double computePlayheadBeats() const;

    /** Dispatch region launches for the lane(s) whose playhead just
     *  crossed a region boundary.  Branches per-lane kind:
     *   - Tracker: schedulePlaying(seqIdx, -1.0, true/false)
     *   - Audio:   schedulePlay(regionId, sourceId, -1.0, 0)
     *  Idempotent via LaneRuntimeState::lastDispatchedRegion. */
    void dispatchAtBeat (double beat);

    /** On transport stop, send scheduleStop to all audio lanes so
     *  their AudioClipNodes silence cleanly.  No effect on tracker
     *  lanes (their last sequence stays "playing" in vht state until
     *  the next start triggers a launch).  Per-DAW convention; audio
     *  doesn't continue past transport stop. */
    void stopAllAudioLanes();

    /** Walk lanes_; compute effective mute (lane.muted OR (any
     *  soloed AND ! lane.soloed)); call Processor::setMuted on each
     *  lane's target node accordingly.  Tracker lanes also propagate
     *  via TrackerNode::setUserMuted/setSoloed so the SessionView /
     *  TrackerEditor see the same state.  Called on M/S toggle and
     *  on every rescan + post-mutation. */
    void propagateMuteSolo();

    void loadLanesFromSession();
    void writeLanesToSession();

    void autoFillLaneForTracker (Lane& lane, TrackerNode* trk);

    /** Resolve a target node uuid to a TrackerNode* via graph walk.
     *  Returns nullptr if absent or not a tracker. */
    TrackerNode*   resolveTrackerByUuid    (juce::Uuid targetNodeUuid) const;
    /** Same, for AudioClipNode. */
    AudioClipNode* resolveAudioClipByUuid  (juce::Uuid targetNodeUuid) const;

    /** Header action: create an empty audio lane bound to a fresh
     *  AudioClipNode in the ArrangementTracks subgraph.  Returns the
     *  index of the new lane in lanes_, or -1 on failure (no
     *  Services / no Session / subgraph creation failed). */
    int createEmptyAudioLane (bool stereo = true);

    /** Common path for file-drop and create-from-disk:
     *   - Opens `file` via libsndfile (metadata: sr, channels, length)
     *   - Registers as AudioFileSource via SourceRegistry
     *   - If `laneIdx == -1`: creates a new audio lane via
     *     createEmptyAudioLane, otherwise targets the given lane
     *     (must already be an audio lane).
     *   - Appends a Region of natural length at positionBeats.
     *   - Auto-fires AudioLaneAdapter::launchNow so the user hears
     *     it immediately.  (Subsequent transport-driven dispatch
     *     keeps it firing on loop wraps.)
     *  Returns false on any failure.  Side effects: writeLanesToSession,
     *  repaint body. */
    bool importAudioFileToLane (const juce::File& file,
                                int               laneIdx,
                                double            positionBeats);

    /** Lane index whose strip area contains the given pixel y, or
     *  -1 if y is below all lanes / above the strip. */
    int laneIdxFromY (int yPx) const noexcept;

    void timerCallback() override;

    /** Audio file import via the Disk Op picker -- arms a Request on
     *  DiskOpService, navigates to the Disk Op page, callback fires
     *  back on accept and dispatches to importAudioFileToLane.
     *  Established Element-NSPA pattern (juce::FileChooser is broken
     *  under winelib; see [[diskop-request-pattern-validated]]). */
    void promptLoadAudioFile();

    Services* services_ = nullptr;
    BlockToolButton rescanBtn_     { "Rescan" };
    BlockToolButton addAudioBtn_   { "+ Audio" };
    BlockToolButton loadAudioBtn_  { "Load..." };
    BlockToolButton snapBtn_       { "Snap" };
    juce::ComboBox snapBox_;
    juce::Label posLabel_;
    juce::Label bpmLabel_;
    juce::Viewport viewport_;
    std::unique_ptr<Body> body_;

    /** Snap configuration.  snapEnabled_ toggles whether drag/resize
     *  snap the target beat to the nearest snapDivision_ multiple.
     *  snapDivision_ is in beats (1.0 = whole beat, 0.25 = 16th,
     *  0.5 = 8th, 4.0 = bar at 4/4).  Set via snapBox_; persisted
     *  in the session arrangement tree on change. */
    bool   snapEnabled_  = true;
    double snapDivision_ = 1.0;

    juce::Array<Lane>              lanes_;
    juce::Array<LaneRuntimeState>  laneRuntime_;

    bool lanesLoadedFromSession_ = false;

    Transport::MonitorPtr monitor_;
    bool wasPlaying_   = false;
    bool wasRecording_ = false;
    double lastBeat_   = 0.0;

    /* Snapshot of the playhead position when transport-recording
     * goes true.  Body paints a placeholder growing rect from this
     * beat to the current playhead on each armed audio lane while
     * recording, giving the user immediate visible feedback before
     * the captured file finalises into a real Region. */
    double recordStartBeat_ = 0.0;

    float lastBpmShown_ = -1.0f;
    double lastBeatShown_ = -999.0;

    juce::ValueTree attachedGraphTree_;

    static constexpr int kHeaderH = 36;
    /* kLaneH + kPxPerBeat are instance-zoomable on Body; outer
     * methods read them via body_->kLaneH / body_->kPxPerBeat.  Only
     * kLabelW stays a static constant (it never zooms).  Shrunk from
     * 160 -> 130 once buttons were stacked vertically instead of in
     * a horizontal row; saves 30 px of timeline real estate. */
    static constexpr int kLabelW  = 130;
};

} // namespace element
