// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dsp/automation/automation_point.hpp"

#include <element/juce/core.hpp>
#include <element/juce/data_structures.hpp>

#include <atomic>
#include <deque>
#include <memory>
#include <vector>

namespace element::dsp::automation {

/** A contiguous span of automation data on the timeline, owned by an
 *  AutomationTrack.  Mirrors `services/timeline/region.hpp` Region's
 *  shape (positionBeats / lengthBeats / looped) but stores a list of
 *  AutomationPoints instead of pointing at a Source.
 *
 *  Thread model: ONE writer (UI / message thread), ONE reader (audio
 *  thread).  The point list is published via a raw pointer atomic
 *  swap.  UI thread builds a new immutable PointList, atomic_exchanges
 *  the live pointer, and pushes the displaced pointer onto a UI-thread
 *  trash deque drained on AsyncUpdater.  Audio thread atomic_loads
 *  once per block and uses the snapshot for the entire render.
 *
 *  Why leaked-pointer + trash-bin instead of std::atomic<shared_ptr>:
 *  libstdc++ implements atomic<shared_ptr> with an internal mutex --
 *  not wait-free and not RT-safe on the audio path.  Raw atomic ptr
 *  swap + deferred reclaim is wait-free for the audio reader and
 *  alloc-free for everyone except the UI-thread edit/sweep path. */
class AutomationRegion
{
public:
    using PointList = std::vector<AutomationPoint>;

    /** Construct an empty region.  The empty PointList snapshot is
     *  allocated up-front so the audio thread never sees a null
     *  active-points pointer. */
    AutomationRegion();

    /** Destructor.  Frees the live snapshot + drains the trash deque.
     *  Caller's responsibility to ensure no audio thread is reading
     *  this region at destruction time (e.g. unbind from the
     *  AutomationEngine first). */
    ~AutomationRegion();

    AutomationRegion (const AutomationRegion&)            = delete;
    AutomationRegion& operator= (const AutomationRegion&) = delete;

    //==========================================================================
    // Identity + timeline placement (UI thread mutates; audio thread reads
    // these as plain copies on its snapshot tick -- the engine stores them
    // in its own per-block working state, so atomicity here is not required
    // for race-freedom on the read path).

    juce::Uuid    id;
    double        positionBeats { 0.0 };
    double        lengthBeats   { 0.0 };
    bool          looped        { false };

    //==========================================================================
    // Sampling -- audio-thread safe.

    /** Sample the region's value at a local-beat offset (0 == region
     *  start, lengthBeats == region end).  Returns normalized [0, 1].
     *
     *  Wait-free.  Loads the active PointList snapshot once, finds the
     *  bracketing pair via std::lower_bound, evaluates the from-point's
     *  curve between them.  Single-point snapshot returns the point's
     *  value.  Empty snapshot returns 0.5 (neutral; equivalent to
     *  "no automation present").
     *
     *  Out-of-range offsets clamp to the nearest endpoint (no extra-
     *  polation outside the points).  If looped is true, the caller
     *  is expected to have wrapped localBeats into [0, lengthBeats)
     *  already -- this method does NOT wrap. */
    double sampleAtBeats (double localBeats) const noexcept;

    /** Load the currently-active immutable PointList snapshot.  Audio
     *  thread holds the returned pointer for the duration of one
     *  render block; releases by simply dropping the local copy.
     *  Lifetime guaranteed by the epoch-counter trash discipline --
     *  see advanceAudioEpoch() + sweepTrash(). */
    const PointList* loadSnapshot() const noexcept
    {
        return activePoints_.load (std::memory_order_acquire);
    }

    /** Audio thread: call once at the START of each render block in
     *  which this region might be sampled.  Advances the internal
     *  epoch counter, telling subsequent sweepTrash() calls that any
     *  trash items stamped at OR BEFORE the prior epoch are safe to
     *  reclaim -- the audio thread has loaded a fresh snapshot since.
     *
     *  Lock-free: single atomic fetch_add per block.  Memory order is
     *  acq_rel so the snapshot load that follows is ordered after the
     *  epoch increment from the UI thread's perspective. */
    void advanceAudioEpoch() noexcept
    {
        audioEpoch_.fetch_add (1, std::memory_order_acq_rel);
    }

    //==========================================================================
    // UI-thread mutators.  All build a new PointList and publish via
    // publishSnapshot().  Audio thread sees the swap atomically.

    /** Replace the entire point list.  The input vector is sorted by
     *  tBeats; callers can pass an unsorted vector and this method
     *  will sort the local copy before publishing. */
    void setPoints (PointList newPoints);

    /** Append a point and re-sort.  Convenience for UI add-on-click. */
    void addPoint (AutomationPoint p);

    /** Remove all points whose id-equivalent (tBeats within epsilon
     *  AND valueNormalized within epsilon) matches the given example.
     *  Used by UI undo when re-adding a point that was just removed
     *  needs a stable identity.  In Phase 1 the UI doesn't yet exist;
     *  this is exercised by tests. */
    void removePointsMatching (const AutomationPoint& example) noexcept;

    /** Drain the trash deque.  Called on the message thread by the
     *  AutomationEngine's AsyncUpdater tick; safe to call from any
     *  thread that isn't the audio thread, but in practice always
     *  the message thread.  Idempotent. */
    void sweepTrash() noexcept;

    //==========================================================================
    // ValueTree persistence -- sparse-write per the Element convention.

    juce::ValueTree toValueTree() const;
    static std::unique_ptr<AutomationRegion> fromValueTree (const juce::ValueTree&);

private:
    /** Atomic-exchange a new PointList into activePoints_ and push the
     *  displaced pointer onto trash_ for deferred reclaim.  Called only
     *  from UI / message thread. */
    void publishSnapshot (std::unique_ptr<PointList> newSnap);

    /** Publish a new snapshot constructed via the in-place lambda.
     *  Snapshot the active list, hand a mutable copy to `mutate`, then
     *  publish.  The lambda runs on the UI thread.  Convenience for
     *  addPoint / removePointsMatching. */
    template <typename Mutator>
    void mutateAndPublish (Mutator&& mutate);

    /** Live snapshot pointer.  ALWAYS non-null after construction;
     *  ctor publishes an empty PointList up front so the audio thread
     *  never observes a null and never has to null-check. */
    std::atomic<const PointList*> activePoints_ { nullptr };

    /** Trash entry: displaced snapshot + the audioEpoch_ value at the
     *  moment of publish.  Reclaimed only when audioEpoch_ has STRICTLY
     *  advanced past this stamp -- guarantees the audio thread has
     *  loaded a different snapshot at least once since publish, so no
     *  in-flight reader can still hold this pointer. */
    struct TrashEntry
    {
        std::unique_ptr<const PointList> ptr;
        std::uint64_t                    stampEpoch;
    };

    /** UI-thread-owned deque of displaced snapshots awaiting reclaim.
     *  Reclaimed by sweepTrash() on the message thread.  Bounded in
     *  practice by edit rate -- one entry per edit between sweeps. */
    std::deque<TrashEntry> trash_;

    /** Per-region epoch counter.  Audio thread advances via
     *  advanceAudioEpoch() at block start.  Trash stamps + sweep
     *  comparisons use this. */
    std::atomic<std::uint64_t> audioEpoch_ { 0 };
};

} // namespace element::dsp::automation
