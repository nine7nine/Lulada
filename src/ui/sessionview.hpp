// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include <element/juce/data_structures.hpp>
#include <element/juce/gui_basics.hpp>
#include <element/services.hpp>
#include <element/transport.hpp>
#include <element/ui/content.hpp>


#define EL_VIEW_SESSION_VIEW "SessionView"

namespace element {

class TrackerNode;

/** Bitwig/Ableton-style clip-grid launcher view.
 *
 *  Each column maps 1:1 to a TrackerNode in the active graph.  Each
 *  scene (row) holds a sparse set of clips — one per column at most.
 *  Clicking a clip flips its underlying vht sequence's `playing` flag
 *  via TrackerNode::setSequencePlaying.  No quantisation in Phase 3;
 *  launch is "fire on next render".
 *
 *  Visual language follows the existing tracker editor palette
 *  (monospaced font, dark background, track-tint bars).  Layout is
 *  Bitwig-shaped: top-row column headers, left-column scene labels,
 *  grid body in between.
 *
 *  Design + cookbook: ~/wine-nspa-notes/session-view-design.md
 */
class SessionView : public ContentView,
                    private juce::Timer,
                    private juce::ChangeListener
{
public:
    SessionView();
    ~SessionView() override;

    void initializeView (Services&) override;
    void didBecomeActive() override;
    void willBeRemoved() override;
    void stabilizeContent() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    /* Per-clip launch state.  Driven by the message thread on bang(),
     * reconciled toward engine truth in the UI timer once the audio
     * thread flips seq->playing at the scheduled boundary. */
    enum class LiveState : uint8_t { Stopped, WaitingToStart, Playing, WaitingToStop };

    /* Per-clip launch quantisation.  Off = immediate (next render
     * block).  All other values snap to transport beats: Beat=1,
     * Bar=beatsPerBar, TwoBars=2*bpb, FourBars=4*bpb. */
    enum class LaunchQuant : uint8_t { Off, Beat, Bar, TwoBars, FourBars };

    /* Action applied when a playing clip's sequence wraps past its end.
     *  - None: keep looping (default; vht naturally loops).
     *  - Stop: schedule immediate stop on wrap.
     *  - RestartClip: re-launch from row 0 (same clip).
     *  - NextClip: bang the clip on the next-greater sceneRow in the
     *    same column.  If none exists, falls back to Stop.
     *  - FirstClip: bang the lowest-sceneRow clip on the same column. */
    enum class FollowAction : uint8_t { None, Stop, RestartClip, NextClip, FirstClip };

    struct SessionClip {
        juce::Uuid     id;
        juce::String   name;
        juce::Colour   color { 0xff'4a'7a'b5 };
        juce::uint32   trackerNodeId { 0 };  // resolves via active graph
        int            sequenceIdx   { -1 }; // -1 = unbound
        int            sceneRow      { 0 };
        int            columnIdx     { 0 };
        LaunchQuant    launchQuant   { LaunchQuant::Bar };
        FollowAction   followAction  { FollowAction::None };
        std::atomic<LiveState> state { LiveState::Stopped };
        /* UI-side cached engine state for diff-gated repaint. */
        bool           lastDrawnPlaying { false };
        int            lastDrawnPosRow  { -1 };

        SessionClip() = default;
        SessionClip (const SessionClip&) = delete;
        SessionClip& operator= (const SessionClip&) = delete;
    };

    struct SessionScene {
        juce::Uuid     id;
        juce::String   name;
        juce::Colour   color { 0xff'30'30'30 };
    };

    struct SessionColumn {
        juce::Uuid     id;
        juce::String   name;
        juce::uint32   trackerNodeId { 0 };
    };

    void timerCallback() override;

    /* Walk the active graph, rebuild columns_ from current TrackerNodes
     * (preserving any persisted column the user authored, dropping
     * orphans).  Cheap; called on activate + graph topology changes. */
    void rescanColumns();

    void bangClip   (SessionClip&);
    void bangScene  (int sceneRow);
    void stopAllClips();
    void transitionClip (SessionClip&, double targetBeat);  // internal, shared by bang*
    void applyFollowAction (SessionClip&);
    void addClipAt  (int sceneRow, int columnIdx);  // creates new vht sequence
    void deleteClip (SessionClip&);
    void openPatternEditor (SessionClip&);  // popup tracker editor at clip's seqIdx

    void addScene();                  // append at end
    void insertScene (int beforeRow);
    void deleteScene (int row);
    void renameScene (int row);
    void reorderScene (int fromRow, int toRow);  // drag-drop reorder

    void renameClip      (SessionClip&);
    void cycleClipColor  (SessionClip&);
    void pickClipColour  (SessionClip&);   // full ColourSelector popup
    void assignExistingPattern (int sceneRow, int columnIdx, int sequenceIdx);

    /* juce::ChangeListener — receives ColourSelector edits from the
     * popup picker.  colourPickerClip_ tracks which clip the picker
     * is bound to so we can apply on each broadcast. */
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    /* Drag-and-drop within the grid.  Move = drop on another cell on
     * the SAME column (sequenceIdx remains valid).  Copy (Shift+drag) =
     * clone the underlying vht sequence via TrackerNode::cloneSequence
     * and place a new clip at the target.  Cross-column drags are no-ops
     * for v1 — sequenceIdx is tracker-scoped, so the migration story is
     * still TBD. */
    void moveClip (SessionClip&, int newSceneRow, int newColumnIdx);
    void copyClip (SessionClip&, int newSceneRow, int newColumnIdx);

    TrackerNode* lookupTracker (juce::uint32 nodeId) const;
    SessionClip* findClip (int sceneRow, int columnIdx) const;

    /* Quantisation maths.  beatsPerBar() reads tags::beatsPerBar from
     * the session (default 4).  computeTargetBeat returns -1.0 for
     * Off quant or for a stopped transport — both fire immediately. */
    int    beatsPerBar() const;
    double currentTransportBeat() const;
    double computeTargetBeat (double curBeat, LaunchQuant q) const;

    /* Persistence.  ValueTree is a child of the session's `objectData`
     * created by Session::setMissingProperties (`tags::sessionView`).
     * Read on initializeView; write on every mutation. */
    juce::ValueTree getOrCreateSessionViewTree();
    void readFromSession();
    void writeToSession();

    /* Grid geometry.  Returned by laying out in resized(); also queried
     * by paint() + mouseDown() for hit-testing. */
    juce::Rectangle<int> toolbarBounds() const noexcept;
    juce::Rectangle<int> footerBounds() const noexcept;
    juce::Rectangle<int> headerRowBounds() const noexcept;
    juce::Rectangle<int> sceneLabelStripBounds() const noexcept;
    juce::Rectangle<int> gridBodyBounds() const noexcept;
    juce::Rectangle<int> cellBounds (int sceneRow, int columnIdx) const noexcept;
    juce::Rectangle<int> sceneLabelBounds (int sceneRow) const noexcept;
    juce::Rectangle<int> columnHeaderBounds (int columnIdx) const noexcept;
    juce::Rectangle<int> playButtonBounds (int sceneRow, int columnIdx) const noexcept;
    juce::Rectangle<int> editButtonBounds (int sceneRow, int columnIdx) const noexcept;
    bool hitTestCell (juce::Point<int> p, int& outRow, int& outCol) const noexcept;
    bool hitTestSceneLabel (juce::Point<int> p, int& outRow) const noexcept;
    bool hitTestPlayButton (juce::Point<int> p, int& outRow, int& outCol) const noexcept;
    bool hitTestEditButton (juce::Point<int> p, int& outRow, int& outCol) const noexcept;

    Services* services_ = nullptr;
    Transport::MonitorPtr monitor_;

    /* Top toolbar — tracker-editor-styled (juce::TextButton with the
     * same palette + juce::Label for static name / editable value
     * pairs).  Mirrors `TrackerEditor::Toolbar` so the two views
     * share a visual vocabulary. */
    juce::TextButton stopAllBtn_, rescanBtn_;
    juce::TextButton sceneMinusBtn_, scenePlusBtn_;
    juce::TextButton quantPrevBtn_,  quantNextBtn_;
    juce::Label scenesNameLabel_, scenesValueLabel_;
    juce::Label quantNameLabel_,  quantValueLabel_;

    /* Default launch quant applied to NEW clips when they're added
     * (right-click "Add new pattern" / "Assign existing pattern" or
     * drag-clone).  Persisted to the session XML at sessionView root. */
    LaunchQuant defaultLaunchQuant_ { LaunchQuant::Bar };

    void configureToolbarButton (juce::TextButton&, const juce::String&, std::function<void()>);
    void configureToolbarLabel  (juce::Label&, const juce::String& text, bool editable);
    void refreshToolbarLabels();
    void cycleDefaultQuant (int delta);   // +1 next / -1 prev
    juce::String formatLaunchQuant (LaunchQuant) const noexcept;

    juce::Array<SessionColumn>      columns_;
    juce::OwnedArray<SessionClip>   clips_;
    juce::Array<SessionScene>       scenes_;

    /* Phase animation for WaitingTo* outline pulse (Phase 4 will exercise). */
    int   pulsePhase_ = 0;

    /* Drag state for clip-move/copy.  dragSource_ is set on mouseDown
     * over a clip's body (not its play/edit zones); dragActive_ trips
     * once the cursor moves past the drag threshold.  dragHover{Row,Col}
     * are the current target cell under the cursor, used to highlight
     * the drop target in paint().  mouseUp clears everything. */
    SessionClip*       dragSource_ = nullptr;
    juce::Point<int>   dragStart_  {};
    bool               dragActive_ = false;
    int                dragHoverRow_ = -1;
    int                dragHoverCol_ = -1;

    /* Scene-label drag state — distinct from clip drag.  Mouse-down on
     * a scene label stages a potential reorder; promoted to active
     * past the drag threshold; release drops the scene at the target
     * row index. */
    int    sceneDragSource_ = -1;
    bool   sceneDragActive_ = false;
    int    sceneDragHoverRow_ = -1;

    /* ColourSelector popup state — when a picker is open against a
     * clip, change broadcasts route through changeListenerCallback
     * to mutate clip.color.  Cleared when the picker closes (its
     * ChangeBroadcaster is destroyed, broadcasts stop arriving). */
    SessionClip* colourPickerClip_ = nullptr;

    /* Layout constants — sized to match the tracker editor's visual
     * rhythm (monospaced, dense, dark). */
    static constexpr int kToolbarH     = 36;
    static constexpr int kHeaderH      = 28;
    static constexpr int kSceneLabelW  = 84;
    static constexpr int kColW         = 132;
    static constexpr int kRowH         = 30;
    static constexpr int kSceneFooterH = 22;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionView)
};

} // namespace element
