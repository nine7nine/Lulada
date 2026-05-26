// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dsp/automation/automation_track.hpp"

#include <algorithm>

namespace element::dsp::automation {

namespace {

const juce::Identifier kAutomationTrackTag { "automationTrack" };
const juce::Identifier kIdAttr             { "id" };
const juce::Identifier kNodeIdAttr         { "nodeId" };
const juce::Identifier kParamIdAttr        { "paramId" };
const juce::Identifier kMidiChannelAttr    { "midiCh" };
const juce::Identifier kMidiCcAttr         { "midiCc" };
const juce::Identifier kModeAttr           { "mode" };
const juce::Identifier kRecordModeAttr     { "recMode" };

} // namespace

AutomationTrack::AutomationTrack()
{
    activeRegions_.store (new RegionList(), std::memory_order_release);
}

AutomationTrack::~AutomationTrack()
{
    if (auto* live = activeRegions_.exchange (nullptr, std::memory_order_acq_rel))
        delete live;
}

//==============================================================================

void AutomationTrack::addRegion (std::unique_ptr<AutomationRegion> region)
{
    if (region == nullptr)
        return;

    AutomationRegion* raw = region.get();
    ownedRegions_.push_back (std::move (region));

    mutateRegionsAndPublish ([raw] (RegionList& copy)
    {
        copy.push_back (raw);
        std::sort (copy.begin(), copy.end(),
                   [] (AutomationRegion* a, AutomationRegion* b) noexcept
                   { return a->positionBeats < b->positionBeats; });
    });
}

void AutomationTrack::removeRegion (const juce::Uuid& targetId)
{
    /* Republish a region-list snapshot WITHOUT the matching pointer,
     * THEN move the owning unique_ptr out of ownedRegions_ into the
     * epoch-gated removedRegionsTrash_ -- the region object stays
     * alive long enough for any in-flight audio reader to release it. */
    AutomationRegion* erased = nullptr;
    mutateRegionsAndPublish ([&] (RegionList& copy)
    {
        auto it = std::find_if (copy.begin(), copy.end(),
                                [&] (AutomationRegion* r) noexcept
                                { return r->id == targetId; });
        if (it != copy.end())
        {
            erased = *it;
            copy.erase (it);
        }
    });

    if (erased == nullptr)
        return;

    /* Best-effort clear the cache.  The audio thread is the only
     * writer of cachedActiveRegion_; in practice the audio thread will
     * miss the cache next call and refresh from the new snapshot.
     * compare_exchange avoids clobbering a concurrent cache update. */
    AutomationRegion* expected = erased;
    cachedActiveRegion_.compare_exchange_strong (expected, nullptr,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire);

    /* Pull the owning unique_ptr out of ownedRegions_ and stash in
     * the epoch-gated removed trash deque. */
    const auto stamp = audioEpoch_.load (std::memory_order_acquire);
    for (auto it = ownedRegions_.begin(); it != ownedRegions_.end(); ++it)
    {
        if (it->get() == erased)
        {
            removedRegionsTrash_.push_back (RegionTrashEntry { std::move (*it), stamp });
            ownedRegions_.erase (it);
            return;
        }
    }
}

//==============================================================================

AutomationRegion* AutomationTrack::findActiveRegion (double timelineBeats) noexcept
{
    /* Fast path: cached region still under the beat. */
    if (auto* cached = cachedActiveRegion_.load (std::memory_order_acquire))
    {
        const double end = cached->positionBeats + cached->lengthBeats;
        if (timelineBeats >= cached->positionBeats && timelineBeats < end)
            return cached;
    }

    /* Slow path: walk the snapshot.  Snapshot is sorted by
     * positionBeats; binary-search for the first region whose start
     * is > beat, then check the previous one. */
    const auto* snap = activeRegions_.load (std::memory_order_acquire);
    if (snap == nullptr || snap->empty())
    {
        cachedActiveRegion_.store (nullptr, std::memory_order_release);
        return nullptr;
    }

    /* std::upper_bound expects comp(value, *iter).  Returns the first
     * region whose positionBeats > beat; the candidate is (it - 1)
     * since regions are sorted ascending by positionBeats. */
    const auto cmp = [] (double beat, const AutomationRegion* r) noexcept
    {
        return beat < r->positionBeats;
    };
    auto it = std::upper_bound (snap->begin(), snap->end(), timelineBeats, cmp);
    if (it == snap->begin())
    {
        cachedActiveRegion_.store (nullptr, std::memory_order_release);
        return nullptr;
    }

    AutomationRegion* candidate = *(it - 1);
    const double end = candidate->positionBeats + candidate->lengthBeats;
    if (timelineBeats < end)
    {
        cachedActiveRegion_.store (candidate, std::memory_order_release);
        return candidate;
    }

    cachedActiveRegion_.store (nullptr, std::memory_order_release);
    return nullptr;
}

//==============================================================================

void AutomationTrack::advanceAudioEpoch() noexcept
{
    /* Advances ONLY the per-track epoch (single atomic fetch_add).
     * Gates region-list snapshot + removed-region trash reclaim.
     *
     * The engine drives per-region epoch advances explicitly on the
     * ACTIVE region (the one it samples this block), via
     * region->advanceAudioEpoch().  Cascade was rejected because it
     * scales linearly in region count and most regions aren't being
     * read on a given block.  Regions that aren't sampled don't have
     * in-flight readers, so their snapshot trash sits until they're
     * sampled again -- bounded by the user's edits to inactive
     * regions, which is rare. */
    audioEpoch_.fetch_add (1, std::memory_order_acq_rel);
}

//==============================================================================
// Touch-record SPSC FIFO -- juce::AbstractFifo prepareToWrite / write /
// finishedWrite + prepareToRead / read / finishedRead pattern.  Single
// producer (UI/control thread), single consumer (audio thread).  Lock-
// free + wait-free for both sides.

bool AutomationTrack::tryPushWriteEvent (const AutomationWriteEvent& ev) noexcept
{
    int s1Start = 0, s1Size = 0, s2Start = 0, s2Size = 0;
    writeFifo_.prepareToWrite (1, s1Start, s1Size, s2Start, s2Size);
    if (s1Size + s2Size == 0)
        return false;   /* FIFO full */
    /* prepareToWrite(1, ...) returns at most one slot total.  Use
     * whichever segment got the allocation. */
    if (s1Size > 0)
        writeFifoStorage_[(size_t) s1Start] = ev;
    else
        writeFifoStorage_[(size_t) s2Start] = ev;
    writeFifo_.finishedWrite (1);
    return true;
}

int AutomationTrack::drainWriteEvents (AutomationWriteEvent* out, int maxOut) noexcept
{
    if (out == nullptr || maxOut <= 0)
        return 0;

    int s1Start = 0, s1Size = 0, s2Start = 0, s2Size = 0;
    writeFifo_.prepareToRead (maxOut, s1Start, s1Size, s2Start, s2Size);
    const int total = s1Size + s2Size;
    if (total <= 0)
        return 0;

    int n = 0;
    for (int i = 0; i < s1Size; ++i)
        out[n++] = writeFifoStorage_[(size_t) (s1Start + i)];
    for (int i = 0; i < s2Size; ++i)
        out[n++] = writeFifoStorage_[(size_t) (s2Start + i)];

    writeFifo_.finishedRead (total);
    return total;
}

int AutomationTrack::getNumPendingWriteEvents() const noexcept
{
    return writeFifo_.getNumReady();
}

//==============================================================================

void AutomationTrack::sweepTrash() noexcept
{
    /* Message-thread reclaim, gated on audioEpoch_ STRICTLY exceeding
     * the per-entry stamp -- guarantees the audio thread loaded a
     * different snapshot (or no snapshot at all) since publish. */
    const auto safeEpoch = audioEpoch_.load (std::memory_order_acquire);

    while (! regionListTrash_.empty() && regionListTrash_.front().stampEpoch < safeEpoch)
        regionListTrash_.pop_front();

    while (! removedRegionsTrash_.empty() && removedRegionsTrash_.front().stampEpoch < safeEpoch)
        removedRegionsTrash_.pop_front();

    /* Cascade to each surviving owned region's own per-region sweep. */
    for (auto& r : ownedRegions_)
        r->sweepTrash();
}

//==============================================================================

template <typename Mutator>
void AutomationTrack::mutateRegionsAndPublish (Mutator&& mutate)
{
    const auto* live = activeRegions_.load (std::memory_order_acquire);
    auto next = (live != nullptr)
                  ? std::make_unique<RegionList> (*live)
                  : std::make_unique<RegionList>();
    mutate (*next);

    const RegionList* raw   = next.release();
    const auto        stamp = audioEpoch_.load (std::memory_order_acquire);
    const RegionList* old   = activeRegions_.exchange (raw, std::memory_order_acq_rel);
    if (old != nullptr)
        regionListTrash_.push_back (ListTrashEntry { std::unique_ptr<const RegionList> (old), stamp });
}

//==============================================================================

juce::ValueTree AutomationTrack::toValueTree() const
{
    juce::ValueTree v (kAutomationTrackTag);
    v.setProperty (kIdAttr, id.toString(), nullptr);

    if (targetKey.isMidi())
    {
        v.setProperty (kMidiChannelAttr, targetKey.midiCcChannel, nullptr);
        v.setProperty (kMidiCcAttr,      targetKey.midiCcNumber,  nullptr);
    }
    else
    {
        v.setProperty (kNodeIdAttr,  targetKey.nodeId.toString(),  nullptr);
        v.setProperty (kParamIdAttr, targetKey.paramId,            nullptr);
    }

    const auto m = getMode();
    if (m != AutomationMode::Off)
        v.setProperty (kModeAttr, (int) m, nullptr);

    const auto rm = getRecordMode();
    if (rm != AutomationRecordMode::Touch)
        v.setProperty (kRecordModeAttr, (int) rm, nullptr);

    for (const auto& r : ownedRegions_)
        v.appendChild (r->toValueTree(), nullptr);

    return v;
}

std::unique_ptr<AutomationTrack> AutomationTrack::fromValueTree (const juce::ValueTree& v)
{
    if (! v.hasType (kAutomationTrackTag))
        return nullptr;

    auto t = std::make_unique<AutomationTrack>();
    t->id = juce::Uuid (v.getProperty (kIdAttr).toString());

    const int cc = (int) v.getProperty (kMidiCcAttr, -1);
    if (cc >= 0)
    {
        t->targetKey.midiCcChannel = (int) v.getProperty (kMidiChannelAttr, 0);
        t->targetKey.midiCcNumber  = cc;
    }
    else
    {
        t->targetKey.nodeId  = juce::Uuid (v.getProperty (kNodeIdAttr).toString());
        t->targetKey.paramId = v.getProperty (kParamIdAttr).toString();
    }

    const int m  = (int) v.getProperty (kModeAttr,       (int) AutomationMode::Off);
    const int rm = (int) v.getProperty (kRecordModeAttr, (int) AutomationRecordMode::Touch);
    t->setMode ((AutomationMode) juce::jlimit (0, (int) AutomationMode::Record, m));
    t->setRecordMode ((AutomationRecordMode) juce::jlimit (0, (int) AutomationRecordMode::Latch, rm));

    for (int i = 0; i < v.getNumChildren(); ++i)
    {
        auto child = v.getChild (i);
        if (auto r = AutomationRegion::fromValueTree (child))
            t->addRegion (std::move (r));
    }

    return t;
}

} // namespace element::dsp::automation
