// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/timeline/audiolaneadapter.hpp"

#include "nodes/audioclip.hpp"
#include "services/sources/sourceregistry.hpp"
#include "services/timeline/lane.hpp"

namespace element {

juce::int64
AudioLaneAdapter::beatsToSourceSamples (double beats,
                                        int    sourceSampleRate,
                                        double bpm) noexcept
{
    if (beats <= 0.0 || sourceSampleRate <= 0 || bpm <= 0.0)
        return 0;

    /* seconds = beats * (60 / bpm)
     * samples = seconds * sampleRate */
    const double seconds = beats * (60.0 / bpm);
    return (juce::int64) (seconds * (double) sourceSampleRate);
}

void
AudioLaneAdapter::queueLaunches (Lane&  lane,
                                 double blockStartBeat,
                                 double blockEndBeat,
                                 bool   transportJumped)
{
    if (target_ == nullptr)
        return;

    /* Phase 3 minimum: TimelineScheduler isn't wired yet (Phase 2
     * full, deferred), so this path is exercised only by future
     * scheduler integration.  v1 keeps the implementation honest:
     *
     *   - On transport jump: stop the current stream so a re-evaluate
     *     starts cleanly at the new transport position.
     *   - For each region whose positionBeats falls in this block,
     *     schedulePlay with beatTarget = positionBeats so the audio
     *     thread fires it sample-accurate within the block.
     *
     * Tempo / startBeats-> sample-offset conversion uses the session
     * BPM that the scheduler will pass in once it lands.  Until then
     * we conservatively pass sampleOffset = 0 (play from start of
     * file).  That matches v1 Region UX -- drag-drop creates a
     * Region with startBeats = 0 anyway. */

    if (transportJumped)
        target_->scheduleStop (juce::Uuid::null(), -1.0);

    lane.playlist.forEachStartIn (blockStartBeat, blockEndBeat,
        [this] (const Region& r)
        {
            /* sampleOffset stays 0 here until scheduler passes bpm in.
             * Region.startBeats > 0 (the trim-into-file case) is a
             * v2 region-editing UX feature. */
            target_->schedulePlay (r.id, r.sourceId,
                                   r.positionBeats /*beatTarget*/,
                                   0 /*sampleOffset*/);
        });
}

void
AudioLaneAdapter::onTargetNodeChanged (Lane& /*lane*/)
{
    /* Caller has already updated target_ via setTargetNode(); nothing
     * further to do.  Hook reserved for future state-sync work (e.g.
     * propagating lane.armed onto target_->setArmed). */
    if (target_ != nullptr)
        target_->setArmed (false);
}

void
AudioLaneAdapter::launchNow (const Region& region)
{
    if (target_ == nullptr)
        return;

    auto source = SourceRegistry::get().findAudioFile (region.sourceId);
    if (source == nullptr)
    {
        juce::Logger::writeToLog (
            juce::String ("AudioLaneAdapter::launchNow: source not registered: ")
            + region.sourceId.toString());
        return;
    }

    target_->schedulePlay (region.id,
                           region.sourceId,
                           -1.0 /*immediate*/,
                           0 /*sampleOffset*/);
}

void
AudioLaneAdapter::stopNow()
{
    if (target_ == nullptr)
        return;
    target_->scheduleStop (juce::Uuid::null(), -1.0);
}

} // namespace element
