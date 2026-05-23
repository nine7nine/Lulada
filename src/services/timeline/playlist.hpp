// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "services/timeline/region.hpp"

#include <vector>

namespace element {

/** Time-ordered list of Regions for one Lane.
 *
 *  Regions are kept sorted by positionBeats so range queries +
 *  iteration are linear without per-call sorting.  The class enforces
 *  the invariant on every mutation; callers should not poke into the
 *  underlying vector directly.
 *
 *  V1 policy: rejects overlapping regions.  addRegion returns false
 *  if the new region's [position, position+length) overlaps any
 *  existing region.  v2 may relax to layering (top-wins, Ardour
 *  pattern).
 *
 *  Thread model: owned by Lane (message thread).  Audio thread never
 *  reads Playlist directly -- the TimelineScheduler computes which
 *  regions fall in a block on the message-thread side (or in a tiny
 *  audio-thread snapshot when Phase 2 lands) and dispatches via the
 *  lane's adapter into the target node's launch FIFO.
 *
 *  See timeline-audio-design.md Section 1.3.
 */
class Playlist
{
public:
    Playlist();

    juce::Uuid id() const noexcept { return id_; }
    void setId (juce::Uuid v) noexcept { id_ = v; }

    /** Add a region to the playlist.  Returns false if the region's
     *  span overlaps any existing region (v1 reject-on-overlap
     *  policy).  Maintains positionBeats sort order. */
    bool addRegion (Region r);

    /** Remove the region with this id.  Returns false if not present. */
    bool removeRegion (juce::Uuid regionId);

    /** Move the region to a new position.  Re-evaluates overlap; on
     *  conflict the original position is restored and returns false. */
    bool moveRegion (juce::Uuid regionId, double newPositionBeats);

    /** Resize the region.  Same overlap policy as add/move. */
    bool resizeRegion (juce::Uuid regionId, double newLengthBeats);

    /** Split the region with `regionId` at the absolute timeline
     *  beat `atBeat`.  The original region keeps the head (its
     *  positionBeats unchanged, lengthBeats = atBeat - position),
     *  and a new region is appended for the tail (positionBeats =
     *  atBeat, lengthBeats = original_end - atBeat).  Source-offset
     *  is preserved across both halves so audio playback continues
     *  through the cut seam without re-reading the file from start.
     *  Returns the new tail region's uuid on success, juce::Uuid()
     *  on failure (atBeat outside the region, region not found,
     *  resulting head/tail too short). */
    juce::Uuid splitRegion (juce::Uuid regionId, double atBeat);

    /** Returns the region whose span contains the given beat, or
     *  nullptr.  V1 always returns 0 or 1; v2 may layer. */
    const Region* regionAt (double beat) const noexcept;

    /** Returns the region with the given id, or nullptr. */
    const Region* findRegion (juce::Uuid regionId) const noexcept;
    Region*       findRegion (juce::Uuid regionId) noexcept;

    const std::vector<Region>& regions() const noexcept { return regions_; }

    /** Iterate regions whose positionBeats falls within [beatA, beatB).
     *  Used by TimelineScheduler to find clip launches that hit this
     *  audio block.  Linear scan; v2 may add an interval tree if
     *  hundreds of regions per lane become common. */
    template <typename Fn>
    void forEachStartIn (double beatA, double beatB, Fn&& fn) const
    {
        for (const auto& r : regions_)
        {
            if (r.positionBeats >= beatB) break;     // sorted; no more candidates
            if (r.positionBeats >= beatA) fn (r);
        }
    }

    juce::ValueTree    toValueTree() const;
    static Playlist    fromValueTree (const juce::ValueTree&);

private:
    juce::Uuid          id_;
    std::vector<Region> regions_;

    /** True if the given span overlaps any existing region whose id
     *  is NOT excludeId. */
    bool overlapsExisting (double position, double length,
                           juce::Uuid excludeId = juce::Uuid()) const noexcept;

    /** Re-sort regions_ by positionBeats.  Cheap on small lists;
     *  v2 may switch to insertion-sort-on-mutate. */
    void rebuildOrder() noexcept;
};

} // namespace element
