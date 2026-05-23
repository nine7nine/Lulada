// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "services/timeline/adapter.hpp"
#include "services/timeline/region.hpp"

#include <element/juce/core.hpp>

namespace element {

class AudioClipNode;
struct Lane;

/** TimelineAdapter for an AudioClipNode.  Translates beat-domain
 *  region launches into sample-domain schedulePlay calls on the
 *  target node's launch FIFO.
 *
 *  Ownership: holds a non-owning AudioClipNode* (the node lives in
 *  the session graph; the adapter is owned by whoever holds the
 *  Lane).  setTargetNode() rebinds when the user moves the lane
 *  to a different node.
 *
 *  Threading: all methods called from the message thread.  Audio
 *  thread reads through the AudioClipNode launch FIFO -- the
 *  adapter never reaches across into the audio thread directly.
 *
 *  See timeline-audio-design.md Section 2.2 + 4.3.
 */
class AudioLaneAdapter final : public TimelineAdapter
{
public:
    /** Build an adapter bound to the given node.  Either argument can
     *  be null (lane temporarily without a target node); calls that
     *  hit a null target are no-ops. */
    explicit AudioLaneAdapter (AudioClipNode* target = nullptr) noexcept
        : target_ (target) {}

    AudioClipNode* targetNode() const noexcept { return target_; }
    void setTargetNode (AudioClipNode* t) noexcept { target_ = t; }

    //==============================================================================
    // TimelineAdapter interface

    void queueLaunches (Lane&  lane,
                        double blockStartBeat,
                        double blockEndBeat,
                        bool   transportJumped) override;

    void onTargetNodeChanged (Lane& lane) override;

    //==============================================================================
    // Convenience entry points for direct-fire flows (DiskOp drag-drop,
    // tests).  Bypass the per-block scheduler -- the caller has a
    // specific region in mind to play immediately.

    /** Launch a region immediately (next block boundary).  Equivalent
     *  to queueLaunches with beatTarget=-1 for just this one region.
     *  Resolves region.sourceId via SourceRegistry; no-op if the
     *  source isn't registered or no target node is bound. */
    void launchNow (const Region& region);

    /** Stop whatever's currently playing on the target node. */
    void stopNow();

private:
    /** Convert a beat-domain source offset into sample-domain.  Uses
     *  the source's intrinsic sample rate (NOT the session sample
     *  rate -- the source's own clock determines how far into the
     *  file the offset lands).  bpm of 0 returns 0 (no-op offset). */
    static juce::int64 beatsToSourceSamples (double  beats,
                                             int     sourceSampleRate,
                                             double  bpm) noexcept;

    AudioClipNode* target_;
};

} // namespace element
