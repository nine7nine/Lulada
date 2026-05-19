// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>
#include <element/juce/data_structures.hpp>
#include <element/node.hpp>
#include <element/ui/content.hpp>
#include <element/services.hpp>
#include <element/transport.hpp>

#define EL_VIEW_ARRANGEMENT "ArrangementView"

namespace element {

class TrackerNode;

/** Main-window arrangement view.
 *
 *  Multi-lane timeline where each lane is bound to one TrackerNode in
 *  the active graph.  Per lane, a sequence of pattern blocks is laid
 *  out left-to-right; on playback the view drives each tracker via
 *  direct C++ calls (TrackerNode::advanceToPattern) — no MIDI
 *  program-change plumbing.
 *
 *  v0 surface (this commit): lanes are auto-populated with one block
 *  per pattern present in the tracker, in order.  Block authoring
 *  (drag / add / remove) lands in v1.
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
    struct Block {
        int   patternIdx { 0 };
        double startBeat { 0.0 };
        double lengthBeats { 4.0 };
    };

    struct Lane {
        TrackerNode* tracker = nullptr;
        juce::String name;
        int numPatterns = 0;
        int currentPattern = -1;
        juce::Array<Block> blocks;
        int lastDispatchedBlockIdx = -1;
    };

    /* Body: single inline-paint component holding all lanes (no per-
     * lane sub-component; cheap to repaint, easy to partial-invalidate). */
    class Body;
    friend class Body;

    void rescanTrackers();
    void togglePlay();
    void attachToActiveGraph();
    void detachFromActiveGraph();
    void updateTransportLabel();
    double computePlayheadBeats() const;

    /* Dispatches advanceToPattern on any lane that crosses a block
     * boundary at the given playhead beat.  Idempotent within a single
     * block (gated by Lane::lastDispatchedBlockIdx). */
    void dispatchAtBeat (double beat);

    void timerCallback() override;

    Services* services_ = nullptr;
    juce::TextButton playBtn_ { "Play" };
    juce::TextButton stopBtn_ { "Stop" };
    juce::TextButton rescanBtn_ { "Rescan" };
    juce::Label posLabel_;
    juce::Label bpmLabel_;
    juce::Viewport viewport_;
    std::unique_ptr<Body> body_;

    juce::Array<Lane> lanes_;

    /* Transport mirror — read from Transport::Monitor on the UI tick. */
    Transport::MonitorPtr monitor_;
    bool wasPlaying_ = false;
    double lastBeat_ = 0.0;

    /* Label diff state — gates juce::Label::setText so we don't keep
     * burning string allocations on idle ticks. */
    float lastBpmShown_ = -1.0f;
    double lastBeatShown_ = -999.0;
    bool lastPlayBtnState_ = false;

    /* Active graph we're attached to as a ValueTree listener — pulled
     * from session->getActiveGraph().data() when the view is opened. */
    juce::ValueTree attachedGraphTree_;

    static constexpr int kHeaderH = 36;
    static constexpr int kLaneH   = 64;
    static constexpr int kLabelW  = 160;
    static constexpr int kPxPerBeat = 24;
};

} // namespace element
