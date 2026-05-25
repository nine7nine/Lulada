// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dsp/automation/automation_region.hpp"

#include <element/juce/core.hpp>
#include <element/juce/data_structures.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace element::dsp::automation {

/** Read / Record / Off, mirroring zrythm + ardour.
 *
 *  Off    -- engine ignores this track entirely; ParamChange events
 *            are not produced, mute table is not set.
 *  Read   -- engine samples the active region every block, produces
 *            ParamChange events, sets the mute table so MappingEngine
 *            won't fight the curve.
 *  Record -- engine drains the touch-record FIFO into a soft buffer
 *            this block; UI thread will materialise the buffer into a
 *            hard AutomationRegion snapshot on touch release.  Coexists
 *            with Read for the regions of the timeline that aren't
 *            being touch-overwritten this pass. */
enum class AutomationMode : std::uint8_t
{
    Off = 0,
    Read,
    Record,
};

/** Touch vs Latch when in Record mode (the design adopts ardour's
 *  terminology directly).  Touch ends recording on knob release;
 *  Latch keeps recording until transport stop.  Phase 1 ships the
 *  enum; UI gating happens in Phase 4. */
enum class AutomationRecordMode : std::uint8_t
{
    Touch = 0,
    Latch,
};

/** Stable persistent key identifying an automation target across
 *  session reload + plugin re-scan.  Stored on the track + in XML.
 *  Engine maintains the live (key -> AutomationTarget) binding.
 *
 *  - nodeId: the Element node hosting the parameter.  For internal
 *    node params, the node ID is the binding root.  For plugin params,
 *    the node ID identifies the GraphProcessor instance hosting the
 *    plugin.
 *  - paramId: stable string ID.  For juce::AudioProcessorParameter
 *    this is `getParameterID(index)` (stable across re-scans per JUCE).
 *    For element::Parameter this is the port symbol or numeric index
 *    as a stringified token -- nodes that don't expose a stable
 *    symbol use the numeric port index.
 *  - midiCcChannel + midiCcNumber: populated only when nodeId is null
 *    (target kind = MidiCc).
 *
 *  Equality is by (nodeId, paramId) for plugin/node targets, by
 *  (channel, cc) for MIDI targets. */
struct AutomationTargetKey
{
    juce::Uuid   nodeId;
    juce::String paramId;
    int          midiCcChannel { 0 };  // 0 = unused / not a MIDI target
    int          midiCcNumber  { -1 }; // -1 = unused / not a MIDI target

    bool isMidi() const noexcept { return midiCcNumber >= 0; }

    bool operator== (const AutomationTargetKey& o) const noexcept
    {
        if (isMidi() || o.isMidi())
            return midiCcChannel == o.midiCcChannel && midiCcNumber == o.midiCcNumber;
        return nodeId == o.nodeId && paramId == o.paramId;
    }
};

/** Per-target automation track.  Owns a list of AutomationRegions
 *  along the timeline; only one region is "active" at a given beat.
 *
 *  Thread model: same as AutomationRegion.  UI mutates region list
 *  via add/remove; published via raw-ptr atomic swap with a trash
 *  deque drained on AsyncUpdater.  Audio thread snapshots the region
 *  list once per engine pass and uses cached active-region pointer
 *  for O(1) re-resolution as the playhead advances.
 *
 *  Mode is atomic<uint8_t> -- single-byte lock-free load on every
 *  platform we target.  AutomationEngine checks mode at block start
 *  to gate the per-track work. */
class AutomationTrack
{
public:
    using RegionList = std::vector<AutomationRegion*>;

    AutomationTrack();
    ~AutomationTrack();

    AutomationTrack (const AutomationTrack&)            = delete;
    AutomationTrack& operator= (const AutomationTrack&) = delete;

    //==========================================================================
    // Identity + binding.

    juce::Uuid             id;
    AutomationTargetKey    targetKey;

    /** Slot index in the engine's mute table for this track's target.
     *  Cached at engine-bind time.  -1 = unbound (engine hasn't seen
     *  this track yet or the track is Off-mode). */
    std::atomic<int>       muteSlotIndex { -1 };

    //==========================================================================
    // Mode -- atomic, lock-free on every supported platform.

    void           setMode (AutomationMode m) noexcept       { mode_.store (m, std::memory_order_release); }
    AutomationMode getMode() const noexcept                  { return mode_.load (std::memory_order_acquire); }

    void                 setRecordMode (AutomationRecordMode m) noexcept { recordMode_.store (m, std::memory_order_release); }
    AutomationRecordMode getRecordMode() const noexcept                  { return recordMode_.load (std::memory_order_acquire); }

    //==========================================================================
    // Region ownership.  All UI-thread mutators republish the live
    // region snapshot; audio thread reads via loadRegionSnapshot().

    /** Take ownership of a region.  Republishes the live region list. */
    void addRegion (std::unique_ptr<AutomationRegion> region);

    /** Remove a region by id (UI-side identity).  No-op if not found. */
    void removeRegion (const juce::Uuid& id);

    /** Wait-free snapshot of the live region pointer list.  Audio
     *  thread holds it for a render block; pointers are stable while
     *  the audio thread holds them (epoch-counter trash discipline
     *  guarantees no concurrent reclaim). */
    const RegionList* loadRegionSnapshot() const noexcept
    {
        return activeRegions_.load (std::memory_order_acquire);
    }

    /** Audio thread: call once at the START of each render block in
     *  which this track might be sampled.  Advances ONLY the per-track
     *  epoch (gating region-list snapshot + removed-region trash
     *  reclaim).  Single atomic fetch_add; lock-free.
     *
     *  Per-region epoch advances are driven by the engine on the
     *  ACTIVE region only -- see AutomationRegion::advanceAudioEpoch.
     *  No cascade here. */
    void advanceAudioEpoch() noexcept;

    //==========================================================================
    // Active-region cache -- audio thread updates on cache miss.

    /** Locate the region under the timeline beat.  Returns nullptr if
     *  no region covers this beat.  Pure wait-free.  Updates the
     *  internal cache via release-store; lockless single-writer.
     *
     *  Algorithm: check cache (one-pointer fast path).  If cache miss
     *  OR no cache yet, walk the snapshot and binary-search for the
     *  first region whose [positionBeats, positionBeats+lengthBeats)
     *  contains the beat.  Cache the find for next call. */
    AutomationRegion* findActiveRegion (double timelineBeats) noexcept;

    //==========================================================================
    // Trash sweep -- message thread reclaims displaced region-list
    // snapshots + per-region trash sweeps.  Called by AutomationEngine
    // on its AsyncUpdater tick.

    void sweepTrash() noexcept;

    //==========================================================================
    // ValueTree persistence.  Region children inline.

    juce::ValueTree toValueTree() const;
    static std::unique_ptr<AutomationTrack> fromValueTree (const juce::ValueTree&);

private:
    /** Publish a new region-list snapshot via atomic exchange.  Old
     *  snapshot goes to regionListTrash_ for AsyncUpdater reclaim.
     *  Owned regions themselves move into ownedRegions_; the snapshot
     *  is just a pointer list referencing those owned cells. */
    template <typename Mutator>
    void mutateRegionsAndPublish (Mutator&& mutate);

    /** Backing storage for region ownership.  Mutated only on UI
     *  thread.  Audio thread never reads this directly -- it walks
     *  the published snapshot.  Region pointers stored in the snapshot
     *  point into this container; we never resize it in a way that
     *  invalidates pointers (use unique_ptr, push_back / erase). */
    std::vector<std::unique_ptr<AutomationRegion>> ownedRegions_;

    std::atomic<AutomationMode>       mode_       { AutomationMode::Off };
    std::atomic<AutomationRecordMode> recordMode_ { AutomationRecordMode::Touch };

    /** Live region-pointer snapshot.  Pre-published with an empty
     *  vector at construction so audio thread never sees null. */
    std::atomic<const RegionList*>    activeRegions_ { nullptr };

    /** Per-track epoch counter.  Advanced by the audio thread once
     *  per block via advanceAudioEpoch().  Trash reclaim for both the
     *  region-list snapshot AND removed-region storage is gated on
     *  strict-greater comparison against this. */
    std::atomic<std::uint64_t>        audioEpoch_ { 0 };

    /** Region-list snapshot trash: displaced RegionList vectors
     *  awaiting reclaim once audioEpoch_ has advanced past the stamp. */
    struct ListTrashEntry
    {
        std::unique_ptr<const RegionList> ptr;
        std::uint64_t                     stampEpoch;
    };
    std::deque<ListTrashEntry>        regionListTrash_;

    /** Removed-region trash: unique_ptrs evicted from ownedRegions_
     *  on removeRegion().  Held until audioEpoch_ has advanced past
     *  the stamp -- guarantees the audio thread no longer holds a
     *  raw region pointer it acquired before the removal. */
    struct RegionTrashEntry
    {
        std::unique_ptr<AutomationRegion> ptr;
        std::uint64_t                     stampEpoch;
    };
    std::deque<RegionTrashEntry>      removedRegionsTrash_;

    /** Cached active region pointer -- single-writer (audio thread),
     *  single-reader (audio thread).  Atomic for visibility, not for
     *  cross-thread synchronisation. */
    std::atomic<AutomationRegion*>    cachedActiveRegion_ { nullptr };
};

} // namespace element::dsp::automation
