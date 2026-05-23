// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "nodes/baseprocessor.hpp"
#include "services/audiostreaming/playback_ds.hpp"

#include <atomic>
#include <memory>

namespace element {

/** Single-region audio playback / capture node for the arrangement
 *  timeline.  Owns one Playback_DS at a time (v1 no-overlap policy);
 *  AudioLaneAdapter drives launches via playRegion() / stopRegion() at
 *  region boundaries.
 *
 *  Two variants register as separate plugin descriptions to satisfy
 *  JUCE's static-IO-per-AudioProcessor contract:
 *   - EL_NODE_ID_AUDIO_CLIP      -> stereo bus
 *   - EL_NODE_ID_AUDIO_CLIP_MONO -> mono bus
 *  Same pattern Element already uses for CombFilter / AllPass / Volume.
 *
 *  ## Threading
 *
 *  - playRegion / stopRegion are message-thread.  They synchronously
 *    open the Audio_File (via Playback_DS::create -> Audio_File::
 *    from_file) and spawn the per-stream IO thread.  The previous
 *    stream's destruction (which joins its IO thread, can take tens
 *    of ms while libsndfile finishes a read) happens OUTSIDE
 *    engineLock_ so the audio callback is never blocked by the join.
 *
 *  - processBlock takes engineLock_ to pin the active-stream pointer
 *    for the duration of one Playback_DS::process call.  JUCE-NSPA
 *    backs juce::CriticalSection with librtpi's pi_mutex_t
 *    (FUTEX_LOCK_PI -- direct Linux kernel futex, no wineserver)
 *    whenever the module is compiled with __WINE__ defined, which
 *    Element-NSPA's canonical wineg++ build does.  When the audio
 *    thread blocks on a swap, the kernel boosts the message-thread
 *    holder to the audio thread's SCHED_FIFO priority for the brief
 *    pointer reassignment.  See JUCE-NSPA juce_CriticalSection.h:
 *    125-132.
 *
 *  ## What's deferred
 *
 *  - Sub-block sample-accurate launches.  v1 fires at the next block
 *    boundary; the design doc's launch FIFO + beatTarget plumbing
 *    (Section 3.1) is a v2 addition once AudioLaneAdapter needs
 *    sample-offset-into-block fire times.
 *
 *  - Overlapping regions / take stacking.  v1 holds one stream at a
 *    time; Playlist::addRegion already rejects overlaps so this
 *    matches the data layer.  Multiple-active-region rendering is a
 *    later phase.
 *
 *  - Recording.  setArmed / isArmed exist as stubs to keep the API
 *    surface stable; actual Record_DS wiring lands in Phase 4.
 */
class AudioClipNode : public BaseProcessor
{
public:
    /** Construct with the bus width baked in.  Stereo = one 2-channel
     *  output bus; mono = one 1-channel output bus.  No MIDI in/out. */
    explicit AudioClipNode (bool stereo);

    ~AudioClipNode() override;

    //==============================================================================
    // BaseProcessor overrides
    const juce::String getName() const override { return "Audio Clip"; }

    void fillInPluginDescription (juce::PluginDescription& desc) const override;

    void prepareToPlay (double newSampleRate, int newBlockSize) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>& buffer,
                       juce::MidiBuffer&         midi) override;

    bool canAddBus    (bool /*isInput*/) const override { return false; }
    bool canRemoveBus (bool /*isInput*/) const override { return false; }

    bool   acceptsMidi()        const override { return false; }
    bool   producesMidi()       const override { return false; }
    bool   supportsMPE()        const override { return false; }
    bool   isMidiEffect()       const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int            getNumPrograms()      override { return 1; }
    int            getCurrentProgram()   override { return 0; }
    void           setCurrentProgram (int)              override {}
    const juce::String getProgramName (int)             override { return getName(); }
    void           changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& dest) override;
    void setStateInformation (const void* data, int sz)    override;

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool                        hasEditor()    const override { return false; }

    //==============================================================================
    // Element-specific public API.  All called from the message thread
    // (typically AudioLaneAdapter; for now also unit-test code).

    /** Start playing `sourceId` (resolved via SourceRegistry) at the
     *  given source-frame offset.  Releases any currently-active
     *  stream first.  No-op if the source isn't registered or the
     *  Audio_File fails to open.
     *
     *  `regionId` is informational -- AudioClipNode stores it so
     *  activeRegion() can report which region the lane scheduled,
     *  but doesn't otherwise use it. */
    void playRegion (juce::Uuid regionId,
                     juce::Uuid sourceId,
                     juce::int64 sourceFrameOffset);

    /** Stop the active stream (if any) at the next block boundary.
     *  IO-thread teardown happens outside engineLock_. */
    void stopRegion();

    /** Currently-playing region UUID, or juce::Uuid::null() when
     *  silent.  Safe to call from any thread. */
    juce::Uuid activeRegion() const noexcept;

    //==============================================================================
    // Record-arm API.  Wired in Phase 4; stubbed here so the surface
    // is stable for AudioLaneAdapter to call against now.
    void setArmed (bool armed) noexcept { armed_.store (armed, std::memory_order_release); }
    bool isArmed() const noexcept       { return armed_.load (std::memory_order_acquire); }

protected:
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

private:
    static juce::AudioProcessor::BusesProperties busesFor (bool stereo);

    const bool stereo_;
    const int  numChannels_;

    double sampleRate_ { 48000.0 };
    int    blockSize_  { 1024 };

    /* librtpi pi_mutex_t in the canonical wineg++ build (juce::
     * CriticalSection gates on __WINE__).  Held BRIEFLY for the
     * active-stream pointer swap; never held across IO-thread join. */
    mutable juce::CriticalSection engineLock_;

    std::unique_ptr<Playback_DS> activeStream_;
    juce::Uuid                   activeRegionId_;

    std::atomic<bool> armed_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioClipNode)
};

} // namespace element
