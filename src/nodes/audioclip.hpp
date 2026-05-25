// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "nodes/baseprocessor.hpp"
#include "services/audiostreaming/playback_ds.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <memory>

namespace element {

class Record_DS;

/** Audio playback + capture node for the arrangement timeline.
 *
 *  ## Topology
 *
 *  One bidirectional bus: input for capture, output for playback.
 *  Two registered variants (per JUCE's static IO contract):
 *   - EL_NODE_ID_AUDIO_CLIP       -> stereo (2 in / 2 out)
 *   - EL_NODE_ID_AUDIO_CLIP_MONO  -> mono   (1 in / 1 out)
 *
 *  ## Playback
 *
 *  Lock-free SPSC launch FIFO drives a Playback_DS, mirroring the
 *  TrackerNode pattern (tracker.hpp:197-223 + tracker.cpp:600-666).
 *  Message thread opens the Audio_File + spawns the Playback_DS IO
 *  thread in schedulePlay(), then writes a LaunchReq carrying
 *  ownership.  Audio thread drains the FIFO at block start, fires
 *  region launches whose beatTarget falls in the current block, and
 *  mixes the active Playback_DS into the output bus.
 *
 *  ## Recording
 *
 *  Capture is triggered by (armed_ && transport.isRecording).  On
 *  the edge from "not recording" to "recording in this block", the
 *  audio thread lazily instantiates a Record_DS via the
 *  pendingRecordStart_ flag (Record_DS construction must happen on
 *  the message thread since it opens libsndfile + spawns a thread;
 *  see startRecording() called via asyncUpdater).  Each block thereafter
 *  feeds the input bus into Record_DS::process.
 *
 *  On the falling edge (transport stops, lane disarmed) the audio
 *  thread requests Record_DS::stop().  The IO thread drains pending
 *  audio + finalises the file.  A juce::Timer on the message thread
 *  polls Record_DS::recording(); when it transitions to false, the
 *  finalised file is delivered via onRecordingCommitted (caller --
 *  ArrangementView -- registers it with SourceRegistry, appends a
 *  Region to the lane's Playlist, refreshes UI).
 *
 *  Capture file path: `recordingDirectory()` (Documents/Element
 *  Recordings/ by default; configurable via setRecordingDirectory).
 *  Filename = "audioclip-<timestamp>.wav".  Format = "Wav 24".
 *
 *  ## Threading invariants
 *
 *  Three roles:
 *   - Message thread (caller): schedulePlay / scheduleStop / setArmed
 *     / setRecordingDirectory / dtor.  Spawns Playback_DS + Record_DS
 *     IO threads.  Polls record_ via Timer for the finalise commit.
 *   - Audio thread (processBlock): drains launchFifo_, applies
 *     pending actions, feeds input into Record_DS, mixes Playback_DS
 *     into output, retires dead streams to graveyardFifo_.  NO LOCKS.
 *   - Timer (message thread): drains graveyardFifo_, deletes retired
 *     Playback_DS instances; polls record_ for the recording->stopped
 *     transition and fires onRecordingCommitted.
 *
 *  The juce::CriticalSection engineLock_ guards ONLY message-thread
 *  bookkeeping (lastScheduledRegionId_, onRecordingCommitted callback
 *  swap) that may be read by foreign threads (UI inspectors).  Audio
 *  thread never takes it.
 */
class AudioClipNode : public BaseProcessor,
                      private juce::Timer,
                      private juce::AsyncUpdater
{
public:
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
    // Playback API (message thread; typically AudioLaneAdapter).

    void schedulePlay (juce::Uuid  regionId,
                       juce::Uuid  sourceId,
                       double      beatTarget,
                       juce::int64 sampleOffset,
                       bool        looped = false,
                       double      gainDb = 0.0,
                       juce::int64 fadeInSamples  = 0,
                       juce::int64 fadeOutSamples = 0,
                       juce::int64 regionLengthSamples = 0,
                       float       fadeInCurve  = 0.0f,
                       float       fadeOutCurve = 0.0f);

    void scheduleStop (juce::Uuid regionId, double beatTarget) noexcept;

    /** Live gain override (linear, not dB).  Set to NaN to clear (then
     *  the per-launch static gainLinear takes over again).  Message
     *  thread writes -- audio thread reads atomically once per block.
     *
     *  Designed for clip volume envelopes: ArrangementView's 30 Hz
     *  timer evaluates the active region's envelope at the playhead
     *  position and pushes the result here.  Sample-accurate envelope
     *  is a polish; v1 ships with 30 Hz coarseness (matches Bitwig's
     *  display rate). */
    void setLiveGain (float linear) noexcept
    {
        liveGainOverride_.store (linear, std::memory_order_relaxed);
    }

    juce::Uuid lastScheduledRegion() const noexcept
    {
        const juce::ScopedLock sl (engineLock_);
        return lastScheduledRegionId_;
    }

    //==============================================================================
    // Record-arm API (message thread).
    //
    // Capture starts on (armed_ && transport.isRecording).  The
    // arm/disarm toggle alone never starts capture -- the user must
    // also hit transport-record.

    void setArmed (bool armed) noexcept { armed_.store (armed, std::memory_order_release); }
    bool isArmed() const noexcept       { return armed_.load (std::memory_order_acquire); }

    /** Directory where capture files land.  Defaults to
     *  ~/Documents/Element Recordings/.  Pass an absolute path; the
     *  directory is created on demand at recording start.  Caller-
     *  owned; AudioClipNode just stores the path. */
    void       setRecordingDirectory (const juce::File& dir);
    juce::File recordingDirectory() const;

    /** Called on the message thread when a capture finishes and the
     *  Audio_File_SF has been closed.  Argument is the finalised
     *  juce::File for the new recording -- the caller is expected to
     *  register it with SourceRegistry::registerAudioFile, append a
     *  Region to the lane's Playlist, and refresh UI.
     *
     *  The callback is invoked at most once per capture session.  If
     *  recording is restarted without an interim commit (e.g. user
     *  hits stop / record very quickly), the prior callback fires
     *  first, then capture re-arms. */
    void setRecordingCommittedHandler (std::function<void (const juce::File&)> handler);

    //==============================================================================
    /** Editor / inspector lock -- audio thread does NOT take this. */
    juce::CriticalSection& engineLock() noexcept { return engineLock_; }

protected:
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

private:
    static juce::AudioProcessor::BusesProperties busesFor (bool stereo);

    //==============================================================================
    // Audio-thread helpers.

    void drainLaunchFifo() noexcept;
    void applyPendingForBlock (double blockStartBeat, double blockEndBeat) noexcept;
    void retireStream (Playback_DS* dead) noexcept;

    /** Audio-thread side of the record path.  Reads transport state
     *  via getPlayHead; on the rising edge, sets pendingRecordStart_
     *  and triggerAsyncUpdate() to ask the message thread to
     *  instantiate Record_DS.  On the falling edge, calls
     *  record_->stop() so the IO thread starts finalising. */
    void handleRecordingFromTransport (const juce::AudioBuffer<float>& in,
                                       int                              numFrames) noexcept;

    //==============================================================================
    // Message-thread helpers.

    /** Drains graveyardFifo_; polls record_ for recording-finished
     *  edge and fires onRecordingCommitted. */
    void timerCallback() override;

    /** Instantiates a fresh Record_DS for the upcoming capture.
     *  Triggered by the audio thread setting pendingRecordStart_.
     *  Runs on the message thread (AsyncUpdater). */
    void handleAsyncUpdate() override;

    /** Build the capture filename for a new recording.  Format:
     *  recordingDirectory_/"audioclip-YYYYMMDD-HHMMSS-<n>".  The
     *  ".wav" extension is appended by Audio_File_SF::create. */
    juce::File composeRecordingBasename();

    //==============================================================================
    // Construction-time config.
    const bool stereo_;
    const int  numChannels_;

    double sampleRate_ { 48000.0 };
    int    blockSize_  { 1024 };

    //==============================================================================
    // Audio-thread-owned state.
    std::unique_ptr<Playback_DS> activeStream_;
    juce::Uuid                   activeStreamRegionId_;

    /* Per-active-region envelope state.  Reset on each new launch
     * via applyPendingForBlock.  Envelope is computed once per
     * block (not per-sample) for v1 -- fades typically span >> one
     * audio block so coarseness is inaudible.  Sample-accurate
     * envelope ramping is a polish later. */
    float       activeGainLinear_       { 1.0f };
    juce::int64 activeFadeInSamples_    { 0 };
    juce::int64 activeFadeOutSamples_   { 0 };
    juce::int64 activeLengthSamples_    { 0 };
    juce::int64 activeSamplesPlayed_    { 0 };
    /* Power-curve exponents derived from the region's fadeInCurve /
     * fadeOutCurve scalars (p = exp2(curve * 2)).  Latched alongside
     * the fade-length fields at PendingAction commit time so the
     * audio thread never reads from the Region directly. */
    float       activeFadeInExp_        { 1.0f };
    float       activeFadeOutExp_       { 1.0f };

    /* Live gain override -- see setLiveGain.  NaN sentinel means
     * "no override; use activeGainLinear_".  std::atomic<float> on
     * x86-64 is lock-free (no atomic mutex). */
    std::atomic<float> liveGainOverride_ { std::numeric_limits<float>::quiet_NaN() };

    /* Audio thread reads + writes; message thread reads only between
     * capture sessions (after timer observes recording()==false). */
    std::unique_ptr<Record_DS>   record_;

    /* Audio-thread edge-tracker for the (armed && transport.isRecording)
     * signal.  Drives the lazy-instantiate path via AsyncUpdater. */
    bool wasRecording_ { false };

    /* Set by audio thread when transport starts recording while
     * armed; cleared by message thread once handleAsyncUpdate has
     * constructed the Record_DS.  Atomic for the cross-thread flag,
     * but the actual construction happens single-threaded on message. */
    std::atomic<bool> pendingRecordStart_ { false };

    /* Set by message thread when a fresh Record_DS is ready; audio
     * thread swaps it into record_ at the next block.  Hand-off
     * pointer; SPSC. */
    std::atomic<Record_DS*> pendingFreshRecord_ { nullptr };

    //==============================================================================
    // Message-thread-owned state.
    mutable juce::CriticalSection engineLock_;
    juce::Uuid                    lastScheduledRegionId_;

    juce::File                     recordingDirectory_;
    int                            recordingSequence_ { 0 };  // suffix counter within a session

    std::function<void (const juce::File&)> onRecordingCommitted_;
    bool                                    wasRecordingFinalising_ { false };

    std::atomic<bool> armed_ { false };

    //==============================================================================
    // Message -> audio launch FIFO (SPSC, lock-free).
    struct LaunchReq
    {
        juce::Uuid   regionId;
        double       beatTarget;
        juce::int64  sampleOffset;
        Playback_DS* stream;
        int          wantPlaying;
        bool         looped;
        float        gainLinear;            /* dB-converted at queue time */
        juce::int64  fadeInSamples;
        juce::int64  fadeOutSamples;
        juce::int64  regionLengthSamples;
        float        fadeInExp;             /* exp2(curve*2), 1.0 = linear */
        float        fadeOutExp;
    };

    static constexpr int kLaunchFifoSize = 64;
    std::array<LaunchReq, (std::size_t) kLaunchFifoSize> launchFifoStorage_ {};
    juce::AbstractFifo                                   launchFifo_ { kLaunchFifoSize };

    struct PendingAction
    {
        juce::Uuid   regionId;
        double       beatTarget;
        juce::int64  sampleOffset;
        Playback_DS* stream;
        bool         wantPlaying;
        float        gainLinear;
        juce::int64  fadeInSamples;
        juce::int64  fadeOutSamples;
        juce::int64  regionLengthSamples;
        float        fadeInExp;
        float        fadeOutExp;
    };

    juce::Array<PendingAction> pendingActions_;

    //==============================================================================
    // Audio -> message graveyard FIFO.
    static constexpr int kGraveyardFifoSize = 32;
    static constexpr int kTimerIntervalMs   = 100;

    std::array<Playback_DS*, (std::size_t) kGraveyardFifoSize> graveyardStorage_ {};
    juce::AbstractFifo                                         graveyardFifo_ { kGraveyardFifoSize };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioClipNode)
};

} // namespace element
