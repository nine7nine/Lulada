// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include <element/juce/data_structures.hpp>
#include <element/juce/gui_basics.hpp>
#include <element/services.hpp>
#include <element/transport.hpp>
#include <element/ui/content.hpp>

#include "ui/blocktoolbutton.hpp"

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
                    private juce::Timer
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

private:
    /* Per-clip launch state.  Driven by the message thread on bang(),
     * reconciled toward engine truth in the UI timer once the audio
     * thread flips seq->playing at the scheduled boundary. */
    enum class LiveState : uint8_t { Stopped, WaitingToStart, Playing, WaitingToStop };

    /* Per-clip launch quantisation.  Off = immediate (next render
     * block).  All other values snap to transport beats: Beat=1,
     * Bar=beatsPerBar, TwoBars=2*bpb, FourBars=4*bpb. */
    enum class LaunchQuant : uint8_t { Off, Beat, Bar, TwoBars, FourBars };

    struct SessionClip {
        juce::Uuid     id;
        juce::String   name;
        juce::Colour   color { 0xff'4a'7a'b5 };
        juce::uint32   trackerNodeId { 0 };  // resolves via active graph
        int            sequenceIdx   { -1 }; // -1 = unbound
        int            sceneRow      { 0 };
        int            columnIdx     { 0 };
        LaunchQuant    launchQuant   { LaunchQuant::Bar };
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
    void addClipAt  (int sceneRow, int columnIdx);  // creates new vht sequence
    void deleteClip (SessionClip&);
    void openPatternEditor (SessionClip&);  // popup tracker editor at clip's seqIdx

    void addScene();                  // append at end
    void insertScene (int beforeRow);
    void deleteScene (int row);

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
    juce::Rectangle<int> addSceneButtonBounds() const noexcept;
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

    /* Top toolbar buttons + footer "+ Scene".  BlockToolButton matches
     * the tracker editor's toolbar density. */
    BlockToolButton stopAllBtn_   { "Stop All" };
    BlockToolButton rescanBtn_    { "Rescan"   };
    BlockToolButton addSceneBtn_  { "+ Scene"  };

    juce::Array<SessionColumn>      columns_;
    juce::OwnedArray<SessionClip>   clips_;
    juce::Array<SessionScene>       scenes_;

    /* Phase animation for WaitingTo* outline pulse (Phase 4 will exercise). */
    int   pulsePhase_ = 0;

    /* Layout constants — sized to match the tracker editor's visual
     * rhythm (monospaced, dense, dark). */
    static constexpr int kToolbarH     = 28;
    static constexpr int kHeaderH      = 28;
    static constexpr int kSceneLabelW  = 84;
    static constexpr int kColW         = 132;
    static constexpr int kRowH         = 30;
    static constexpr int kSceneFooterH = 26;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionView)
};

} // namespace element
