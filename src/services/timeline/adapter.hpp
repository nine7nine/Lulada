// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/core.hpp>

namespace element {

struct Lane;

/** Abstract dispatcher that connects a Lane's Playlist to its target
 *  graph node.  One concrete subclass per source kind:
 *
 *   - AudioLaneAdapter -> drives an AudioClipNode via its launch FIFO
 *     (services/timeline/audiolaneadapter.hpp).
 *   - MidiLaneAdapter  -> drives a TrackerNode via schedulePlaying.
 *     Pending -- Phase 2 minimum already routes
 *     ArrangementView::dispatchAtBeat through TrackerNode directly
 *     (7bab8c27); promotion to the adapter abstraction lands when
 *     TimelineScheduler does.
 *
 *  Ownership: adapters are owned by ArrangementView (or whatever
 *  client holds the Lane), keyed by Lane.id.  Lane itself is pure
 *  data (no adapter pointer member) so the playlist + UI state
 *  remain trivially copyable / serialisable.
 *
 *  Threading: implementations dispatch into the target node's launch
 *  FIFO, which carries an SPSC ownership transfer (message thread ->
 *  audio thread).  The Adapter's own methods run on the message
 *  thread.  See timeline-audio-design.md Section 2.2.
 */
class TimelineAdapter
{
public:
    virtual ~TimelineAdapter() = default;

    /** Push region launches for the audio block whose transport-beat
     *  range is [blockStartBeat, blockEndBeat) into the target node.
     *  Called from the TimelineScheduler each block.
     *
     *  transportJumped = true means the transport just located /
     *  looped; the adapter should re-evaluate what region is
     *  currently active rather than relying on per-block deltas. */
    virtual void queueLaunches (Lane&  lane,
                                double blockStartBeat,
                                double blockEndBeat,
                                bool   transportJumped) = 0;

    /** Called from the message thread when the lane's targetNodeUuid
     *  changes (user re-binds a lane to a different node).  Adapters
     *  cache their target pointer; refresh it from the live graph
     *  here. */
    virtual void onTargetNodeChanged (Lane& lane) = 0;
};

} // namespace element
