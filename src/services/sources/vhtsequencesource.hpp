// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "services/sources/source.hpp"

namespace element {

class TrackerNode;

/** Source kind: a vht sequence (pattern of MIDI rows) inside a
 *  TrackerNode.  The sequence's actual data lives in the owning
 *  TrackerNode's `mod_->seq[]` array -- this source is a thin
 *  reference wrapping (trackerNodeId, sequenceIdx).
 *
 *  Resolution: SourceRegistry::resolveVhtSequence() walks the active
 *  graph each call -- the TrackerNode may be added/removed at
 *  runtime, so we don't cache the pointer here.  Cheap (graph walk
 *  is O(num nodes)); resolve at adapter-bind time, not per render
 *  block.
 *
 *  Lifetime: outlives the TrackerNode in the lazy sense -- a Region
 *  pointing at an unbound VhtSequenceSource just won't dispatch
 *  until the user re-creates a tracker with the same uuid (which is
 *  also a degenerate case the UI should make discoverable).  V1
 *  treats missing-target as "skip this region."
 *
 *  This source is NOT persisted in the SourceRegistry blob -- it's
 *  derived live from the graph on session reload.  Persistence is
 *  implicit via the TrackerNode's own getState/setState (which
 *  already round-trips its sequences).  See
 *  timeline-audio-design.md Section 1.1.
 */
class VhtSequenceSource : public Source
{
public:
    using Ptr = juce::ReferenceCountedObjectPtr<VhtSequenceSource>;

    VhtSequenceSource (juce::Uuid trackerNodeId, int sequenceIdx) noexcept
        : trackerNodeId_ (trackerNodeId), sequenceIdx_ (sequenceIdx) {}

    Kind kind() const noexcept override { return Kind::Midi; }

    juce::Uuid trackerNodeId() const noexcept { return trackerNodeId_; }
    int        sequenceIdx()   const noexcept { return sequenceIdx_; }

    juce::String displayName() const override
    {
        return juce::String ("Seq ") + juce::String (sequenceIdx_ + 1);
    }

    /** vht stores sequence length in ROWS at a given rpb (rows per
     *  beat).  Beats = rows / rpb.  We don't read the live module
     *  here (would need engineLock + a TrackerNode lookup); the
     *  caller passes the rows when constructing or queries the live
     *  TrackerNode separately.  For v1, return 0 -- regions carry
     *  their own length and don't rely on source-derived duration. */
    double durationBeats (double /*sampleRate*/, double /*bpm*/) const override
    {
        return 0.0;
    }

private:
    juce::Uuid trackerNodeId_;
    int        sequenceIdx_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VhtSequenceSource)
};

} // namespace element
