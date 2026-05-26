// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "services/timeline/midi_note_region.hpp"
#include "services/timeline/region.hpp"

#include <memory>
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

    /** Deep-copy.  Required for juce::Array<Lane> undo snapshots in
     *  ArrangementView -- MidiNoteRegion is non-copyable so Playlist
     *  cannot rely on the implicit copy.  Audio Regions copy by
     *  value; MIDI regions are cloned via MidiNoteRegion::clone(). */
    Playlist (const Playlist& other);
    Playlist& operator= (const Playlist& other);

    /** Move ops stay default -- transfers the unique_ptr vector
     *  cheaply.  noexcept so juce::Array can pick the move path. */
    Playlist (Playlist&&) noexcept            = default;
    Playlist& operator= (Playlist&&) noexcept = default;

    ~Playlist() = default;

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

    //==========================================================================
    // MIDI region API.  MidiNoteRegion is non-copyable (owns COW snapshot
    // pointer + atomic epoch + trash deque), so the storage shape diverges
    // from audio/tracker regions: unique_ptr in a parallel vector.  Same
    // overlap-allowed policy as audio regions.

    /** Append a MIDI note region.  Takes ownership.  Returns false if
     *  the region is null, has negative length, OR if its time range
     *  [position, position+length) overlaps an existing MIDI region
     *  on this playlist (mirrors the audio path's no-overlap policy). */
    bool addMidiRegion (std::unique_ptr<MidiNoteRegion> r);

    /** Remove the MIDI region with this id.  Returns false if not
     *  present. */
    bool removeMidiRegion (juce::Uuid regionId);

    /** Cleave the MIDI region at the given absolute beat into two.
     *  The left half keeps the original id + positionBeats; the right
     *  half gets a fresh uuid + positionBeats = original.positionBeats
     *  + atBeatOffset.  Notes split between halves: notes whose onBeat
     *  falls before the cut stay with the left, notes whose onBeat is
     *  at or after the cut move to the right (with onBeat re-based to
     *  the new region's coords).  Notes that STRADDLE the cut are
     *  truncated at the cut (left half) and not duplicated -- matches
     *  Ableton + Bitwig convention.  Returns the new right-half uuid
     *  on success, null on failure (out-of-range cut, region not
     *  found, region not splittable). */
    juce::Uuid splitMidiRegion (juce::Uuid regionId, double atBeat);

    /** Re-sort midiRegions_ by positionBeats.  Call after any direct
     *  mutation of an existing region's positionBeats (e.g. an
     *  arrangement-view drag) so the sorted invariant holds for
     *  forEachMidiStartIn + paint order. */
    void rebuildMidiOrder() noexcept;

    MidiNoteRegion*       findMidiRegion (juce::Uuid regionId) noexcept;
    const MidiNoteRegion* findMidiRegion (juce::Uuid regionId) const noexcept;

    const std::vector<std::unique_ptr<MidiNoteRegion>>& midiRegions() const noexcept
    {
        return midiRegions_;
    }

    /** Iterate MIDI regions whose positionBeats falls within
     *  [beatA, beatB).  Linear scan; sorted by positionBeats so the
     *  iteration can early-exit once positionBeats >= beatB. */
    template <typename Fn>
    void forEachMidiStartIn (double beatA, double beatB, Fn&& fn) const
    {
        for (const auto& m : midiRegions_)
        {
            if (m == nullptr) continue;
            if (m->positionBeats >= beatB) break;
            if (m->positionBeats >= beatA) fn (*m);
        }
    }

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
    std::vector<std::unique_ptr<MidiNoteRegion>> midiRegions_;

    /** True if the given span overlaps any existing region whose id
     *  is NOT excludeId. */
    bool overlapsExisting (double position, double length,
                           juce::Uuid excludeId = juce::Uuid()) const noexcept;

    /** Re-sort regions_ by positionBeats.  Cheap on small lists;
     *  v2 may switch to insertion-sort-on-mutate. */
    void rebuildOrder() noexcept;
};

} // namespace element
