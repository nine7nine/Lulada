// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "nodes/baseprocessor.hpp"
#include "services/audiostreaming/playback_ds.hpp"

#include <array>
#include <atomic>
#include <memory>

namespace element {

class Record_DS;

/** Single-region audio playback / capture node for the arrangement
 *  timeline.  Drives a Playback_DS via a lock-free SPSC launch FIFO,
 *  mirroring the TrackerNode pattern (tracker.hpp:197-223 +
 *  tracker.cpp:600-666).  AudioLaneAdapter writes launch requests on
 *  the message thread; the audio thread drains the FIFO at block
 *  start, fires region launches whose beatTarget falls within the
 *  block, and mixes the active Playback_DS into the output.
 *
 *  Two variants register as separate plugin descriptions to satisfy
 *  JUCE's static-IO-per-AudioProcessor contract:
 *   - EL_NODE_ID_AUDIO_CLIP      -> stereo bus
 *   - EL_NODE_ID_AUDIO_CLIP_MONO -> mono bus
 *  Same pattern Element already uses for CombFilter / AllPass / Volume.
 *
 *  ## Threading model
 *
 *  Three thread roles touch this class; their boundaries are
 *  enforced by the FIFO + the graveyard-FIFO + std::atomic state:
 *
 *  - **Message thread (caller, typically AudioLaneAdapter).**
 *    schedulePlay() / scheduleStop() open the Audio_File and spawn
 *    the Playback_DS IO thread synchronously, then write a single
 *    LaunchReq into launchFifo_.  Ownership of the new
 *    Playback_DS* transfers to the audio thread via the FIFO entry.
 *
 *  - **Audio thread (processBlock).**  Drains launchFifo_ into
 *    pendingActions_; for each pending action whose beatTarget
 *    falls in the current block, retires the prior active stream
 *    (sends to graveyardFifo_) and installs the new one.  Then
 *    mixes the active Playback_DS into the output buffer.  NO
 *    locks on this path -- the message thread does not touch
 *    activeStream_ / activeStreamRegionId_ / pendingActions_ at all
 *    outside of construction and destruction.
 *
 *  - **Timer (message thread).**  Drains graveyardFifo_ every
 *    kGraveyardDrainMs and deletes the retired Playback_DS
 *    instances.  ~Playback_DS joins the IO thread (libsndfile sf_close
 *    + thread join, can take tens of ms); doing it here keeps that
 *    cost off both the audio thread and the message thread's
 *    launch-handling path.
 *
 *  ## What's still v1
 *
 *  - One active region at a time.  v1 enforces no-overlap in
 *    Playlist::addRegion; multi-active rendering is a Phase 6
 *    addition (extend pendingActions_ application to keep a
 *    juce::Array<ActiveRegion> instead of a single slot).
 *
 *  - Per-region gain / fades / loop ignored.  LaunchReq carries
 *    sampleOffset + beatTarget only; Region.fadeInBeats /
 *    fadeOutBeats / gainDb apply at the AudioLaneAdapter or post-mix
 *    layer later.
 *
 *  - Recording.  setArmed / isArmed exist as stubs to keep the API
 *    surface stable; actual Record_DS instantiation + capture wiring
 *    lands in Phase 4.  The record_ member slot is reserved.
 */
class AudioClipNode : public BaseProcessor,
                      private juce::Timer
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
    // Launch API.  All called from the message thread (typically
    // AudioLaneAdapter; for now also unit-test code).

    /** Schedule a region for playback.  Synchronous:
     *   1. Resolves sourceId via SourceRegistry::findAudioFile
     *   2. Opens the Audio_File and spawns the Playback_DS IO thread
     *      (Playback_DS::create)
     *   3. Seeks to sampleOffset
     *   4. Writes a LaunchReq into launchFifo_; ownership of the new
     *      Playback_DS* transfers to the audio thread on FIFO read.
     *
     *  beatTarget < 0 = fire at next block boundary (immediate);
     *  beatTarget >= 0 = fire at the audio block whose beat range
     *  contains this target (sample-accurate to +/- one block).
     *
     *  No-op (no FIFO write, fresh stream cleaned up) if the source
     *  isn't in the registry or the Audio_File fails to open. */
    void schedulePlay (juce::Uuid  regionId,
                       juce::Uuid  sourceId,
                       double      beatTarget,
                       juce::int64 sampleOffset);

    /** Schedule a region stop.  beatTarget semantics match
     *  schedulePlay.  Writes a LaunchReq with stream=null; the audio
     *  thread retires the active stream when the request fires. */
    void scheduleStop (juce::Uuid regionId, double beatTarget) noexcept;

    /** Last region UUID the *message thread* requested to play.
     *  Best-effort snapshot for UI use; lags the audio-thread reality
     *  by one FIFO drain. */
    juce::Uuid lastScheduledRegion() const noexcept
    {
        const juce::ScopedLock sl (engineLock_);
        return lastScheduledRegionId_;
    }

    //==============================================================================
    // Record-arm API.  Phase 4 wires the Record_DS path; stubbed
    // here so AudioLaneAdapter can call against a stable surface.
    void setArmed (bool armed) noexcept { armed_.store (armed, std::memory_order_release); }
    bool isArmed() const noexcept       { return armed_.load (std::memory_order_acquire); }

    //==============================================================================
    /** Editor / inspector lock -- audio thread does NOT take this on
     *  the render path (FIFO design).  Used by future editors / state
     *  inspectors that need to read message-thread bookkeeping (e.g.
     *  lastScheduledRegionId_) coherently. */
    juce::CriticalSection& engineLock() noexcept { return engineLock_; }

protected:
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

private:
    static juce::AudioProcessor::BusesProperties busesFor (bool stereo);

    //==============================================================================
    // Audio-thread helpers.

    /** Drain launchFifo_ into pendingActions_.  Called from
     *  processBlock; runs only on the audio thread. */
    void drainLaunchFifo() noexcept;

    /** For each pending action whose beatTarget falls in the block,
     *  swap active stream + retire the prior one to graveyardFifo_.
     *  Drops applied entries from pendingActions_; out-of-block
     *  entries stay queued for a future block. */
    void applyPendingForBlock (double blockStartBeat, double blockEndBeat) noexcept;

    /** Audio-thread helper to retire a stream pointer (push onto
     *  graveyardFifo_ for the message thread to delete). */
    void retireStream (Playback_DS* dead) noexcept;

    //==============================================================================
    // Message-thread helper.

    /** juce::Timer callback -- drains graveyardFifo_, deletes the
     *  retired Playback_DS instances (each dtor joins the IO thread). */
    void timerCallback() override;

    //==============================================================================
    // Construction-time config.
    const bool stereo_;
    const int  numChannels_;

    // Updated by prepareToPlay; read by message thread on
    // schedulePlay to size the new Playback_DS.
    double sampleRate_ { 48000.0 };
    int    blockSize_  { 1024 };

    //==============================================================================
    // Audio-thread-owned state.  Touched ONLY from processBlock and
    // related audio-thread helpers (drainLaunchFifo,
    // applyPendingForBlock, retireStream).  Message thread reads /
    // mutates these only at construction / destruction when the audio
    // thread is guaranteed quiescent.
    std::unique_ptr<Playback_DS> activeStream_;
    juce::Uuid                   activeStreamRegionId_;

    //==============================================================================
    // Message-thread-owned state.  Accessed only from message thread
    // entry points + the Timer callback; engineLock_ pins reads from
    // foreign threads (e.g. UI inspectors).
    mutable juce::CriticalSection engineLock_;
    juce::Uuid                    lastScheduledRegionId_;

    std::atomic<bool> armed_ { false };

    //==============================================================================
    // Message -> audio launch FIFO (SPSC, lock-free).
    struct LaunchReq
    {
        juce::Uuid   regionId;
        double       beatTarget;     // <0 = immediate
        juce::int64  sampleOffset;
        Playback_DS* stream;         // non-null on start, nullptr on stop
        int          wantPlaying;    // 1 = start, 0 = stop
    };

    static constexpr int kLaunchFifoSize = 64;
    std::array<LaunchReq, (std::size_t) kLaunchFifoSize> launchFifoStorage_ {};
    juce::AbstractFifo                                   launchFifo_ { kLaunchFifoSize };

    //==============================================================================
    // Audio-thread pending actions: launches that have been drained
    // from launchFifo_ but whose beatTarget has not yet been reached.
    // v1 enforces single-region playback in Playlist::addRegion, so
    // this array usually holds 0 or 1 entries.
    struct PendingAction
    {
        juce::Uuid   regionId;
        double       beatTarget;
        juce::int64  sampleOffset;
        Playback_DS* stream;
        bool         wantPlaying;
    };

    juce::Array<PendingAction> pendingActions_;

    //==============================================================================
    // Audio -> message graveyard FIFO.  Audio thread pushes retired
    // Playback_DS* here; Timer drains + deletes (the dtor joins the
    // IO thread, which is not safe to do on the audio thread).
    static constexpr int kGraveyardFifoSize = 32;
    static constexpr int kGraveyardDrainMs  = 100;

    std::array<Playback_DS*, (std::size_t) kGraveyardFifoSize> graveyardStorage_ {};
    juce::AbstractFifo                                         graveyardFifo_ { kGraveyardFifoSize };

    //==============================================================================
    // Record_DS slot reserved for Phase 4.  nullptr until then.
    std::unique_ptr<Record_DS> record_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioClipNode)
};

} // namespace element
