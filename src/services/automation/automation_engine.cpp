// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/automation/automation_engine.hpp"

#include <algorithm>

namespace element::automation {

AutomationEngine::AutomationEngine()
{
    /* Pre-publish an empty track-pointer snapshot so the audio thread
     * never observes a null pointer.  Same invariant the data-model
     * types maintain at construction. */
    liveTracks_.store (new TrackList(), std::memory_order_release);
}

AutomationEngine::~AutomationEngine()
{
    if (auto* live = liveTracks_.exchange (nullptr, std::memory_order_acq_rel))
        delete live;
    /* trash deques + ownedTracks_ + ownedTargets_ + muteSlots_ self-
     * reclaim via their unique_ptr destructors. */
}

//==============================================================================

AutomationEngine::AutomationTrack* AutomationEngine::addTrack (std::unique_ptr<AutomationTrack> track)
{
    if (track == nullptr)
        return nullptr;

    AutomationTrack* raw = track.get();
    ownedTracks_.push_back (std::move (track));

    mutateTracksAndPublish ([raw] (TrackList& copy)
    {
        copy.push_back (raw);
    });

    return raw;
}

void AutomationEngine::removeTrack (const juce::Uuid& trackId)
{
    AutomationTrack* erased = nullptr;
    mutateTracksAndPublish ([&] (TrackList& copy)
    {
        auto it = std::find_if (copy.begin(), copy.end(),
                                [&] (AutomationTrack* t) noexcept
                                { return t->id == trackId; });
        if (it != copy.end())
        {
            erased = *it;
            copy.erase (it);
        }
    });

    if (erased == nullptr)
        return;

    /* Force the track Off so the audio thread, if still iterating the
     * old snapshot for this block, doesn't attempt a dispatch via a
     * target whose backing parameter may itself be torn down soon. */
    erased->setMode (AutomationMode::Off);

    /* Unbind first so the retired target moves to retiredTargetsTrash_
     * for epoch-gated reclaim. */
    unbindTarget (erased);

    const auto stamp = audioEpoch_.load (std::memory_order_acquire);
    for (auto it = ownedTracks_.begin(); it != ownedTracks_.end(); ++it)
    {
        if (it->get() == erased)
        {
            removedTracksTrash_.push_back (RemovedTrackEntry { std::move (*it), stamp });
            ownedTracks_.erase (it);
            return;
        }
    }
}

//==============================================================================

void AutomationEngine::bindPluginParam (AutomationTrack* track, juce::AudioProcessorParameter* param)
{
    if (track == nullptr || param == nullptr)
        return;
    ensureMuteSlot (track);
    auto t = std::make_unique<AutomationTarget>();
    t->kind        = AutomationTarget::Kind::PluginParam;
    t->pluginParam = param;
    publishTarget (track, std::move (t));
}

void AutomationEngine::bindNodeParam (AutomationTrack* track, element::ParameterPtr param)
{
    if (track == nullptr || param == nullptr)
        return;
    ensureMuteSlot (track);
    auto t = std::make_unique<AutomationTarget>();
    t->kind      = AutomationTarget::Kind::NodeParam;
    t->nodeParam = param;
    publishTarget (track, std::move (t));
}

void AutomationEngine::bindMidiCc (AutomationTrack* track, int channel, int ccNumber)
{
    if (track == nullptr || ccNumber < 0)
        return;
    ensureMuteSlot (track);
    auto t = std::make_unique<AutomationTarget>();
    t->kind         = AutomationTarget::Kind::MidiCc;
    t->midiChannel  = channel;
    t->midiCcNumber = ccNumber;
    publishTarget (track, std::move (t));
}

void AutomationEngine::unbindTarget (AutomationTrack* track)
{
    if (track == nullptr)
        return;

    /* Clear the mute slot FIRST so MappingEngine immediately stops
     * deferring writes for the (now-unbound) target.  Otherwise a
     * stale mute=true would persist (the engine's per-block mute
     * publish only fires for tracks visited via the live snapshot;
     * if a track is later removed, it disappears from the snapshot
     * and its slot is never written again). */
    const int slot = track->muteSlotIndex.load (std::memory_order_acquire);
    if (slot >= 0 && slot < (int) muteSlots_.size())
        muteSlots_[(size_t) slot]->store (false, std::memory_order_release);

    /* Atomic-swap the published live-target ptr to null; the displaced
     * target goes to retiredTargetsTrash_ stamped at the current
     * engine epoch.  Audio thread that loaded the old pointer before
     * the swap will finish using it in its current block; reclaim
     * waits until the engine epoch has advanced past the stamp. */
    const auto stamp = audioEpoch_.load (std::memory_order_acquire);
    const AutomationTarget* old = track->liveTarget.exchange (nullptr, std::memory_order_acq_rel);
    if (old == nullptr)
        return;

    /* Find the unique_ptr in ownedTargets_ + move into trash. */
    for (auto it = ownedTargets_.begin(); it != ownedTargets_.end(); ++it)
    {
        if (it->get() == old)
        {
            retiredTargetsTrash_.push_back (RetiredTargetEntry { std::move (*it), stamp });
            ownedTargets_.erase (it);
            return;
        }
    }
}

//==============================================================================

int AutomationEngine::numTracks() const noexcept
{
    return (int) ownedTracks_.size();
}

AutomationEngine::AutomationTrack* AutomationEngine::findTrackById (const juce::Uuid& id) noexcept
{
    for (auto& t : ownedTracks_)
        if (t->id == id)
            return t.get();
    return nullptr;
}

//==============================================================================

void AutomationEngine::applyForBlock (double currentBeats,
                                      int    /*numSamples*/,
                                      double /*sampleRate*/,
                                      juce::MidiBuffer* outMidi) noexcept
{
    /* Bump engine epoch FIRST: this serialises any concurrent UI
     * publish/unbind with the audio pass.  Specifically, a UI publish
     * that happens AFTER this fetch_add will stamp its trash with
     * the new epoch -- which sweepTrash() requires the engine epoch
     * to STRICTLY exceed before reclaim.  So this block's snapshot
     * load can't have its backing memory freed while we hold the ptr. */
    audioEpoch_.fetch_add (1, std::memory_order_acq_rel);

    const auto* snap = liveTracks_.load (std::memory_order_acquire);
    if (snap == nullptr || snap->empty())
        return;

    for (AutomationTrack* track : *snap)
    {
        /* Per-track epoch bump (gates the track's own region-list +
         * removed-region trash).  Cheap fetch_add. */
        track->advanceAudioEpoch();

        const auto mode = track->getMode();

        /* Mute-flag publish: true while the track is in Read mode
         * (engine writes the param; MappingEngine must defer), false
         * otherwise.  Atomic store per block per slot -- bounded by
         * track count, not target count, and skipped when slot is
         * unallocated (-1). */
        const int slot = track->muteSlotIndex.load (std::memory_order_acquire);
        if (slot >= 0 && slot < (int) muteSlots_.size())
        {
            const bool shouldMute = (mode == AutomationMode::Read);
            muteSlots_[(size_t) slot]->store (shouldMute, std::memory_order_release);
        }

        if (mode != AutomationMode::Read)
            continue;

        const AutomationTarget* target = track->liveTarget.load (std::memory_order_acquire);
        if (target == nullptr || ! target->isValid())
            continue;

        AutomationRegion* region = track->findActiveRegion (currentBeats);
        if (region == nullptr)
            continue;

        /* Active region's own epoch bump -- gates the region's
         * point-list snapshot trash.  Explicit, not via cascade
         * (cascade scales linearly in region count and we only need
         * the bump for THIS region). */
        region->advanceAudioEpoch();

        const double localBeats = currentBeats - region->positionBeats;
        const double v          = region->sampleAtBeats (localBeats);

        target->writeCoarseValue ((float) v, outMidi);
    }
}

//==============================================================================

bool AutomationEngine::isMappingMuted (int slotIndex) const noexcept
{
    if (slotIndex < 0 || slotIndex >= (int) muteSlots_.size())
        return false;
    return muteSlots_[(size_t) slotIndex]->load (std::memory_order_acquire);
}

//==============================================================================

void AutomationEngine::sweepTrash() noexcept
{
    const auto safeEpoch = audioEpoch_.load (std::memory_order_acquire);

    while (! trackListTrash_.empty() && trackListTrash_.front().stampEpoch < safeEpoch)
        trackListTrash_.pop_front();

    while (! retiredTargetsTrash_.empty() && retiredTargetsTrash_.front().stampEpoch < safeEpoch)
        retiredTargetsTrash_.pop_front();

    while (! removedTracksTrash_.empty() && removedTracksTrash_.front().stampEpoch < safeEpoch)
        removedTracksTrash_.pop_front();

    /* Cascade per-track sweep to surviving owned tracks (they have
     * their own region-list + removed-region trash). */
    for (auto& t : ownedTracks_)
        t->sweepTrash();
}

//==============================================================================

template <typename Mutator>
void AutomationEngine::mutateTracksAndPublish (Mutator&& mutate)
{
    const auto* live = liveTracks_.load (std::memory_order_acquire);
    auto next = (live != nullptr)
                  ? std::make_unique<TrackList> (*live)
                  : std::make_unique<TrackList>();
    mutate (*next);

    const TrackList* raw   = next.release();
    const auto       stamp = audioEpoch_.load (std::memory_order_acquire);
    const TrackList* old   = liveTracks_.exchange (raw, std::memory_order_acq_rel);
    if (old != nullptr)
        trackListTrash_.push_back (TrackListTrashEntry { std::unique_ptr<const TrackList> (old), stamp });
}

void AutomationEngine::ensureMuteSlot (AutomationTrack* track)
{
    if (track == nullptr)
        return;
    if (track->muteSlotIndex.load (std::memory_order_acquire) >= 0)
        return;
    /* Append a new slot.  Slot indices are stable for the engine's
     * lifetime (we never compact).  Bounded by distinct-targets-ever-
     * bound; for a session that's typically tens to low hundreds. */
    muteSlots_.push_back (std::make_unique<std::atomic<bool>> (false));
    const int slot = (int) muteSlots_.size() - 1;
    track->muteSlotIndex.store (slot, std::memory_order_release);
}

void AutomationEngine::publishTarget (AutomationTrack* track, std::unique_ptr<AutomationTarget> target)
{
    if (track == nullptr || target == nullptr)
        return;

    AutomationTarget* raw = target.get();
    ownedTargets_.push_back (std::move (target));

    /* Atomic-swap the live target pointer.  Old (if any) moves to
     * retiredTargetsTrash_ stamped with the current audio epoch. */
    const auto stamp = audioEpoch_.load (std::memory_order_acquire);
    const AutomationTarget* old = track->liveTarget.exchange (raw, std::memory_order_acq_rel);
    if (old != nullptr)
    {
        for (auto it = ownedTargets_.begin(); it != ownedTargets_.end(); ++it)
        {
            if (it->get() == old)
            {
                retiredTargetsTrash_.push_back (RetiredTargetEntry { std::move (*it), stamp });
                ownedTargets_.erase (it);
                break;
            }
        }
    }
}

} // namespace element::automation
