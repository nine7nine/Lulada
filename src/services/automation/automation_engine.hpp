// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dsp/automation/automation_track.hpp"
#include "services/automation/automation_target.hpp"

#include <element/juce/audio_basics.hpp>
#include <element/juce/core.hpp>
#include <element/parameter.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace element::automation {

/** Per-graph automation engine.  Owns AutomationTracks + their
 *  resolved AutomationTargets + the MappingEngine mute-flag table.
 *
 *  Lifetime: ONE engine per GraphProcessor (lands in commit 5).
 *  Tracks are added by the session loader / UI; targets are bound by
 *  the engine when given a live Parameter / ProcessorParameter
 *  reference (typically at GraphProcessor topology callbacks).
 *
 *  Per-block audio-thread call:
 *      engine.applyForBlock(currentBeats, numSamples, sampleRate,
 *                           outMidi);
 *  The pass:
 *      1. Bump per-engine audio epoch (gates trash reclaim for the
 *         track-list snapshot + retired targets / mute slots).
 *      2. Snapshot-load the live track-pointer vector.
 *      3. For each track: advance track epoch, update mute slot,
 *         and -- if mode == Read AND a target is bound AND an active
 *         region covers the playhead -- advance the active region's
 *         epoch, sample, and dispatch via the target.
 *
 *  Empty state (zero tracks): one atomic_load + one fetch_add + one
 *  pointer-null check.  Total cost ~3 atomic ops.
 *
 *  MappingEngine consultation: handlers cache their slot index at
 *  construction; check isMappingMuted(slot) before writing.  Lookup
 *  is a single atomic_load on a slot pointer indexed array.
 *
 *  RT-safety: every audio-thread path is wait-free.  No locks, no
 *  allocations.  All UI-side mutation goes through the leaked-ptr-
 *  trash-bin pattern with epoch-gated reclaim. */
class AutomationEngine
{
public:
    using AutomationTrack = element::dsp::automation::AutomationTrack;
    using AutomationRegion = element::dsp::automation::AutomationRegion;
    using AutomationMode   = element::dsp::automation::AutomationMode;
    using TrackList        = std::vector<AutomationTrack*>;

    AutomationEngine();
    ~AutomationEngine();

    AutomationEngine (const AutomationEngine&)            = delete;
    AutomationEngine& operator= (const AutomationEngine&) = delete;

    //==========================================================================
    // UI / session-load API (message-thread only).

    /** Take ownership of a track + republish the live track snapshot.
     *  Returns the raw pointer for convenience.  No target binding
     *  happens here -- caller must follow up with a bindXxx() call
     *  to make the track active. */
    AutomationTrack* addTrack (std::unique_ptr<AutomationTrack> track);

    /** Remove by id.  Track is moved to epoch-gated trash; reclaimed
     *  by sweepTrash() once the audio thread has advanced past the
     *  removal epoch. */
    void removeTrack (const juce::Uuid& trackId);

    /** Bind the track to a plugin parameter.  Allocates a mute slot
     *  if the track doesn't have one; publishes the resolved target
     *  via atomic store; old target (if any) moves to epoch-gated
     *  trash. */
    void bindPluginParam (AutomationTrack* track, juce::AudioProcessorParameter* param);

    /** Bind the track to an Element internal node parameter. */
    void bindNodeParam (AutomationTrack* track, element::ParameterPtr param);

    /** Bind the track to a MIDI CC output. */
    void bindMidiCc (AutomationTrack* track, int channel, int ccNumber);

    /** Clear the current binding without removing the track -- used
     *  by graph-topology callbacks before the backing
     *  ProcessorParameter is destroyed.  Track stays alive; its mode
     *  is forced Off until rebound to prevent the audio thread from
     *  trying to dispatch through a stale target. */
    void unbindTarget (AutomationTrack* track);

    //==========================================================================
    // Lookups (message-thread).

    int              numTracks() const noexcept;
    AutomationTrack* findTrackById (const juce::Uuid& id) noexcept;

    /** Iterate all owned tracks in deterministic order.  For XML
     *  save + UI list-view consumption (NOT audio path). */
    template <typename Fn>
    void forEachTrack (Fn&& fn) const
    {
        for (const auto& t : ownedTracks_)
            fn (t.get());
    }

    //==========================================================================
    // Audio-thread per-block call.  outMidi may be null when the
    // engine is operating in a no-MIDI-out context this block.

    void applyForBlock (double currentBeats,
                        int    numSamples,
                        double sampleRate,
                        juce::MidiBuffer* outMidi) noexcept;

    //==========================================================================
    // MappingEngine consultation.  Audio-thread safe -- single atomic
    // load.  Returns false (i.e. NOT muted) for out-of-range slots
    // OR when the engine has zero tracks (the common "no automation
    // configured" case). */

    bool isMappingMuted (int slotIndex) const noexcept;

    /** Lookup the mute state by plugin parameter pointer.  Called by
     *  MappingEngine handlers (MIDI thread) before each
     *  setValueNotifyingHost write.  Fast path: cheap atomic load on
     *  activeTrackCount_ short-circuits to false when zero tracks
     *  are bound -- the common "no automation configured" case.
     *
     *  Slow path: takes a brief juce::CriticalSection (PI-correct on
     *  PREEMPT_RT) + walks ownedTracks_ for a matching binding.  O(N)
     *  in track count; bounded by user-bound automation tracks
     *  (typically <100).  Tolerates UI-thread bind/unbind racing
     *  against this lookup -- both operations take the same lock. */
    bool isMappingMutedForPluginParam (juce::AudioProcessorParameter* p) const noexcept;

    /** Same as above for element::Parameter (internal node params). */
    bool isMappingMutedForNodeParam (element::Parameter* p) const noexcept;

    /** Cheap atomic snapshot of the bound-track count.  MIDI thread
     *  uses this to skip the lock entirely in the common "no
     *  automation" case. */
    int activeTrackCount() const noexcept
    {
        return activeTrackCount_.load (std::memory_order_acquire);
    }

    /** Drain in-flight MIDI-thread lookups by acquiring + releasing
     *  the lookupLock_.  Called by the engine owner (e.g. RootGraph)
     *  during teardown, AFTER the MappingEngine's engine pointer has
     *  been cleared to nullptr -- guarantees no MIDI handler is mid-
     *  deref when the engine is destroyed.  Acquires the same PI-
     *  correct lock the MIDI lookups use; brief contention. */
    void drainPendingLookups() noexcept
    {
        const juce::ScopedLock sl (lookupLock_);
        (void) sl;
    }

    //==========================================================================
    // Message-thread cleanup.  Reclaims trashed track-list snapshots,
    // retired targets, removed tracks -- all epoch-gated against the
    // engine's audio epoch (and each track's own epoch for nested
    // sweeps). */

    void sweepTrash() noexcept;

private:
    /** Republish a new track-pointer snapshot.  Old snapshot moves to
     *  trash stamped with the current engine epoch. */
    template <typename Mutator>
    void mutateTracksAndPublish (Mutator&& mutate);

    /** Allocate (or re-use) a mute slot for the given track.  Slot
     *  index stored on the track.  No-op if track already has a slot. */
    void ensureMuteSlot (AutomationTrack* track);

    /** Publish a freshly-built AutomationTarget heap allocation onto
     *  the track via atomic store; old target (if any) goes to
     *  retiredTargets_ for epoch-gated reclaim. */
    void publishTarget (AutomationTrack* track, std::unique_ptr<AutomationTarget> target);

    /* MIDI-thread lookup lock.  PI-correct juce::CriticalSection.
     * Acquired by isMappingMutedForXxx (MIDI thread, per CC event)
     * AND by addTrack / removeTrack / bindXxx / unbindTarget (UI
     * thread, rare).  NEVER acquired on the audio thread. */
    mutable juce::CriticalSection                  lookupLock_;

    /* Cheap "no tracks at all" short-circuit for MIDI thread.  When
     * zero, MIDI handlers skip the lock + walk entirely. */
    std::atomic<int>                               activeTrackCount_ { 0 };

    /* Owned storage (UI-thread mutated only).  Audio thread never
     * reads these directly. */
    std::vector<std::unique_ptr<AutomationTrack>>  ownedTracks_;
    std::vector<std::unique_ptr<AutomationTarget>> ownedTargets_;

    /* Fixed-size mute-slot table.  Avoids std::vector reallocation
     * race against concurrent audio-thread reads.  256 slots is well
     * above typical per-session automation target counts; ensureMuteSlot
     * silently no-ops past the cap (track stays unmutable from
     * MappingEngine's perspective -- it can still automate, just
     * won't fight MIDI mapping for that target). */
    static constexpr int kMaxMuteSlots = 256;
    std::array<std::atomic<bool>, kMaxMuteSlots>   muteSlots_ {};

    /* Slot allocation cursor.  fetch_add for atomic claim; never
     * shrinks (slots are stable for the engine's lifetime). */
    std::atomic<int>                               muteSlotCount_ { 0 };

    /* Published live track-pointer snapshot.  Pre-published empty so
     * audio thread never sees null. */
    std::atomic<const TrackList*>                  liveTracks_ { nullptr };

    /* Per-engine epoch counter -- advanced once per applyForBlock
     * call.  Gates reclaim for displaced track snapshots + retired
     * targets + removed tracks. */
    std::atomic<std::uint64_t>                     audioEpoch_ { 0 };

    /* Trash deques (message-thread mutated). */
    struct TrackListTrashEntry
    {
        std::unique_ptr<const TrackList> ptr;
        std::uint64_t                    stampEpoch;
    };
    std::deque<TrackListTrashEntry>                trackListTrash_;

    struct RemovedTrackEntry
    {
        std::unique_ptr<AutomationTrack> ptr;
        std::uint64_t                    stampEpoch;
    };
    std::deque<RemovedTrackEntry>                  removedTracksTrash_;

    struct RetiredTargetEntry
    {
        std::unique_ptr<AutomationTarget> ptr;
        std::uint64_t                     stampEpoch;
    };
    std::deque<RetiredTargetEntry>                 retiredTargetsTrash_;
};

} // namespace element::automation
