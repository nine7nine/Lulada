// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/audioclip.hpp"

#include "services/audiostreaming/record_ds.hpp"
#include "services/sources/sourceregistry.hpp"

#include <element/node.h>

namespace element {

namespace {

constexpr const char* kCaptureFormat = "Wav 24";

juce::File defaultRecordingDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
             .getChildFile ("Element Recordings");
}

} // namespace

//==============================================================================
// Bus config

juce::AudioProcessor::BusesProperties
AudioClipNode::busesFor (bool stereo)
{
    BusesProperties b;
    const auto layout = stereo ? juce::AudioChannelSet::stereo()
                               : juce::AudioChannelSet::mono();
    b.addBus (true  /*isInput*/,  "In",  layout, true);
    b.addBus (false /*isInput*/,  "Out", layout, true);
    return b;
}

bool
AudioClipNode::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.inputBuses.size()  != 1) return false;
    if (layouts.outputBuses.size() != 1) return false;
    const auto want = stereo_ ? juce::AudioChannelSet::stereo()
                              : juce::AudioChannelSet::mono();
    if (layouts.inputBuses.getReference (0) != want)  return false;
    if (layouts.outputBuses.getReference (0) != want) return false;
    return true;
}

//==============================================================================
// Lifecycle

AudioClipNode::AudioClipNode (bool stereo)
    : BaseProcessor (busesFor (stereo)),
      stereo_       (stereo),
      numChannels_  (stereo ? 2 : 1),
      recordingDirectory_ (defaultRecordingDirectory())
{
    startTimer (kTimerIntervalMs);
}

AudioClipNode::~AudioClipNode()
{
    stopTimer();
    cancelPendingUpdate();

    /* Quiescent graph teardown -- drain everything. */
    const int pending = launchFifo_.getNumReady();
    if (pending > 0)
    {
        int s1, sz1, s2, sz2;
        launchFifo_.prepareToRead (pending, s1, sz1, s2, sz2);
        for (int i = 0; i < sz1; ++i)
            delete launchFifoStorage_[(std::size_t) (s1 + i)].stream;
        for (int i = 0; i < sz2; ++i)
            delete launchFifoStorage_[(std::size_t) (s2 + i)].stream;
        launchFifo_.finishedRead (pending);
    }

    for (auto& p : pendingActions_)
        delete p.stream;
    pendingActions_.clear();

    activeStream_.reset();

    /* If a recording is still in flight, request stop + wait briefly.
     * The Record_DS dtor does shutdown() which joins the IO thread. */
    if (record_ != nullptr)
    {
        record_->stop();
        record_.reset();
    }

    if (auto* p = pendingFreshRecord_.exchange (nullptr))
        delete p;

    const int gpending = graveyardFifo_.getNumReady();
    if (gpending > 0)
    {
        int s1, sz1, s2, sz2;
        graveyardFifo_.prepareToRead (gpending, s1, sz1, s2, sz2);
        for (int i = 0; i < sz1; ++i)
            delete graveyardStorage_[(std::size_t) (s1 + i)];
        for (int i = 0; i < sz2; ++i)
            delete graveyardStorage_[(std::size_t) (s2 + i)];
        graveyardFifo_.finishedRead (gpending);
    }
}

//==============================================================================
// Plugin description

void
AudioClipNode::fillInPluginDescription (juce::PluginDescription& desc) const
{
    desc.fileOrIdentifier   = stereo_ ? EL_NODE_ID_AUDIO_CLIP : EL_NODE_ID_AUDIO_CLIP_MONO;
    desc.name               = stereo_ ? "Audio Clip" : "Audio Clip (Mono)";
    desc.descriptiveName    = "Arrangement audio clip (record + playback)";
    desc.numInputChannels   = numChannels_;
    desc.numOutputChannels  = numChannels_;
    desc.hasSharedContainer = false;
    desc.isInstrument       = false;
    desc.category           = "Source";
    desc.manufacturerName   = EL_NODE_FORMAT_AUTHOR;
    desc.pluginFormatName   = EL_NODE_FORMAT_NAME;
    desc.version            = "0.1.0";
    desc.uniqueId           = stereo_ ? EL_NODE_UID_AUDIO_CLIP : EL_NODE_UID_AUDIO_CLIP_MONO;
}

//==============================================================================
// prepare / release

void
AudioClipNode::prepareToPlay (double newSampleRate, int newBlockSize)
{
    sampleRate_ = newSampleRate;
    blockSize_  = newBlockSize;
}

void
AudioClipNode::releaseResources()
{
}

//==============================================================================
// Audio thread

void
AudioClipNode::processBlock (juce::AudioBuffer<float>& buffer,
                             juce::MidiBuffer&         /*midi*/)
{
    /* Swap in any message-thread-prepared Record_DS before this block. */
    if (auto* fresh = pendingFreshRecord_.exchange (nullptr))
    {
        /* Defensive: in the normal flow record_ is cleared by the
         * timer once a capture finalises, but a race between fast
         * stop->start could leave the previous one around.  Send it
         * stop() so the timer's next tick finalises it, then drop
         * the unique_ptr -- the IO thread keeps running through
         * stop until it observes _terminate. */
        if (record_ != nullptr)
            record_->stop();
        record_.reset (fresh);
    }

    const int numFrames = buffer.getNumSamples();

    /* Bus slices.  juce::AudioProcessor::getBusBuffer returns a
     * non-owning view over the underlying AudioBuffer, scoped to one
     * bus's channels.  Reading input + writing output is safe even
     * if the host aliases them: we read input first (Record_DS::
     * process), then clear+write output. */
    auto inputBus  = getBusBuffer<float> (buffer, true,  0);
    auto outputBus = getBusBuffer<float> (buffer, false, 0);

    /* Record path first -- captures the input before the output
     * write would clobber it under aliased-bus hosts. */
    handleRecordingFromTransport (inputBus, numFrames);

    outputBus.clear();

    /* Pull transport beat range from the host AudioPlayHead so
     * applyPendingForBlock can fire beatTarget-quantised launches
     * at the block whose range contains the target.  Sample-
     * accurate to +/- one block (same as TrackerNode's launch
     * scheduler).  When no playhead info is available (transport
     * stopped / unhosted), fall back to immediate-only semantics
     * (-1, -1). */
    double blockStartBeat = -1.0;
    double blockEndBeat   = -1.0;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            if (auto ppq = pos->getPpqPosition())
            {
                blockStartBeat = *ppq;
                /* Block span in beats = numFrames / sampleRate * bpm/60. */
                const auto bpmOpt = pos->getBpm();
                const double bpm = (bpmOpt.hasValue() ? *bpmOpt : 120.0);
                if (sampleRate_ > 0.0 && bpm > 0.0)
                    blockEndBeat = blockStartBeat
                        + ((double) numFrames / sampleRate_) * (bpm / 60.0);
                else
                    blockEndBeat = blockStartBeat;
            }
        }
    }

    drainLaunchFifo();
    applyPendingForBlock (blockStartBeat, blockEndBeat);

    if (activeStream_ != nullptr)
    {
        activeStream_->process (outputBus, 0, (nframes_t) numFrames);

        /* Per-block envelope: gain * fade-in * fade-out, applied to
         * the full block.  v1 uses block-rate (not sample-rate)
         * resolution -- a 5 ms block within a 50 ms fade gets 10
         * envelope steps, inaudibly coarse for the typical fade
         * length.  Sample-accurate ramping (juce::AudioBuffer::
         * applyGainRamp) is a polish item. */
        float blockGain = activeGainLinear_;

        if (activeFadeInSamples_ > 0 && activeSamplesPlayed_ < activeFadeInSamples_)
        {
            const float t = (float) activeSamplesPlayed_
                          / (float) activeFadeInSamples_;
            blockGain *= juce::jlimit (0.0f, 1.0f, t);
        }

        if (activeFadeOutSamples_ > 0 && activeLengthSamples_ > 0)
        {
            const juce::int64 fadeOutStart =
                activeLengthSamples_ - activeFadeOutSamples_;
            if (activeSamplesPlayed_ >= fadeOutStart)
            {
                const juce::int64 into = activeSamplesPlayed_ - fadeOutStart;
                const float t = 1.0f - (float) into / (float) activeFadeOutSamples_;
                blockGain *= juce::jlimit (0.0f, 1.0f, t);
            }
        }

        if (std::abs (blockGain - 1.0f) > 1.0e-4f)
            outputBus.applyGain (blockGain);

        activeSamplesPlayed_ += numFrames;
    }
}

void
AudioClipNode::handleRecordingFromTransport (const juce::AudioBuffer<float>& in,
                                             int                              numFrames) noexcept
{
    bool transportRecording = false;
    bool transportPlaying   = false;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            transportRecording = pos->getIsRecording();
            transportPlaying   = pos->getIsPlaying();
        }
    }

    /* Capture requires armed lane AND transport in record mode AND
     * transport actually playing.  The third condition matches
     * Ardour / Ableton / Bitwig behaviour: hitting Stop finalises
     * the recording (transport.isPlaying drops to false even if
     * .isRecording stays true).  Otherwise the user would have to
     * click the record toggle separately to commit -- non-standard
     * DAW UX. */
    const bool armed          = armed_.load (std::memory_order_acquire);
    const bool wantingCapture = armed && transportRecording && transportPlaying;

    /* Rising edge: ask message thread to instantiate Record_DS. */
    if (wantingCapture && ! wasRecording_)
    {
        /* If record_ is somehow still around (shouldn't be), let the
         * timer drain it.  Set pending flag to trigger fresh
         * instantiation. */
        pendingRecordStart_.store (true, std::memory_order_release);
        triggerAsyncUpdate();
    }

    /* Feed input while recording. */
    if (record_ != nullptr && record_->recording())
        record_->process (in, 0, (nframes_t) numFrames);

    /* Falling edge: stop the active capture so the IO thread drains
     * + finalises.  Message-thread Timer picks up the finalise. */
    if (! wantingCapture && wasRecording_ && record_ != nullptr)
        record_->stop();

    wasRecording_ = wantingCapture;
}

//==============================================================================
// Playback API (message thread)

void
AudioClipNode::schedulePlay (juce::Uuid  regionId,
                             juce::Uuid  sourceId,
                             double      beatTarget,
                             juce::int64 sampleOffset,
                             bool        looped,
                             double      gainDb,
                             juce::int64 fadeInSamples,
                             juce::int64 fadeOutSamples,
                             juce::int64 regionLengthSamples)
{
    auto source = SourceRegistry::get().findAudioFile (sourceId);
    if (source == nullptr)
    {
        juce::Logger::writeToLog (
            juce::String ("AudioClipNode::schedulePlay: source not in registry: ")
            + sourceId.toString());
        return;
    }

    /* Loop start = sampleOffset (so a region launched mid-source
     * loops back to that same start, not the absolute file start).
     * Bitwig-style "start" follows the region's content offset. */
    auto fresh = Playback_DS::create (source,
                                      (float) sampleRate_,
                                      (nframes_t) blockSize_,
                                      numChannels_,
                                      looped,
                                      (nframes_t) sampleOffset /*loopStartFrame*/);
    if (fresh == nullptr)
        return;

    fresh->seek ((nframes_t) sampleOffset);

    int s1, sz1, s2, sz2;
    launchFifo_.prepareToWrite (1, s1, sz1, s2, sz2);
    if (sz1 + sz2 < 1)
    {
        juce::Logger::writeToLog (
            "AudioClipNode::schedulePlay: launchFifo full -- dropping request");
        return;
    }

    Playback_DS* freshRaw = fresh.release();

    const float gainLinear = (float) juce::Decibels::decibelsToGain (gainDb);

    const LaunchReq req {
        regionId, beatTarget, sampleOffset, freshRaw, 1 /*wantPlaying*/, looped,
        gainLinear, fadeInSamples, fadeOutSamples, regionLengthSamples
    };

    if (sz1 > 0)
        launchFifoStorage_[(std::size_t) s1] = req;
    else
        launchFifoStorage_[(std::size_t) s2] = req;
    launchFifo_.finishedWrite (1);

    {
        const juce::ScopedLock sl (engineLock_);
        lastScheduledRegionId_ = regionId;
    }
}

void
AudioClipNode::scheduleStop (juce::Uuid regionId, double beatTarget) noexcept
{
    int s1, sz1, s2, sz2;
    launchFifo_.prepareToWrite (1, s1, sz1, s2, sz2);
    if (sz1 + sz2 < 1)
        return;

    const LaunchReq req {
        regionId, beatTarget, 0, nullptr, 0 /*wantPlaying*/, false,
        1.0f /*unused*/, 0, 0, 0
    };

    if (sz1 > 0)
        launchFifoStorage_[(std::size_t) s1] = req;
    else
        launchFifoStorage_[(std::size_t) s2] = req;
    launchFifo_.finishedWrite (1);

    {
        const juce::ScopedLock sl (engineLock_);
        lastScheduledRegionId_ = juce::Uuid::null();
    }
}

//==============================================================================
// Record-arm config

void
AudioClipNode::setRecordingDirectory (const juce::File& dir)
{
    const juce::ScopedLock sl (engineLock_);
    recordingDirectory_ = dir;
}

juce::File
AudioClipNode::recordingDirectory() const
{
    const juce::ScopedLock sl (engineLock_);
    return recordingDirectory_;
}

void
AudioClipNode::setRecordingCommittedHandler (std::function<void (const juce::File&)> handler)
{
    const juce::ScopedLock sl (engineLock_);
    onRecordingCommitted_ = std::move (handler);
}

juce::File
AudioClipNode::composeRecordingBasename()
{
    juce::File dir;
    {
        const juce::ScopedLock sl (engineLock_);
        dir = recordingDirectory_;
    }

    dir.createDirectory();   // no-op if exists

    const auto now      = juce::Time::getCurrentTime();
    const auto stamp    = now.formatted ("%Y%m%d-%H%M%S");
    const int  seq      = ++recordingSequence_;
    const juce::String name = juce::String ("audioclip-") + stamp
                            + (seq > 1 ? ("-" + juce::String (seq)) : "");
    return dir.getChildFile (name);
}

//==============================================================================
// Audio-thread FIFO helpers

void
AudioClipNode::drainLaunchFifo() noexcept
{
    const int ready = launchFifo_.getNumReady();
    if (ready == 0) return;

    int s1, sz1, s2, sz2;
    launchFifo_.prepareToRead (ready, s1, sz1, s2, sz2);

    auto absorb = [this] (const LaunchReq& r) noexcept
    {
        /* v1 single-region: collapse pending list to the latest. */
        for (int i = pendingActions_.size(); --i >= 0;)
        {
            auto& p = pendingActions_.getReference (i);
            if (p.stream != nullptr && p.stream != r.stream)
                retireStream (p.stream);
            pendingActions_.remove (i);
        }
        pendingActions_.add (PendingAction {
            r.regionId, r.beatTarget, r.sampleOffset, r.stream,
            r.wantPlaying != 0,
            r.gainLinear, r.fadeInSamples, r.fadeOutSamples,
            r.regionLengthSamples
        });
    };

    for (int i = 0; i < sz1; ++i)
        absorb (launchFifoStorage_[(std::size_t) (s1 + i)]);
    for (int i = 0; i < sz2; ++i)
        absorb (launchFifoStorage_[(std::size_t) (s2 + i)]);

    launchFifo_.finishedRead (ready);
}

void
AudioClipNode::applyPendingForBlock (double blockStartBeat, double blockEndBeat) noexcept
{
    for (int i = pendingActions_.size(); --i >= 0;)
    {
        auto& p = pendingActions_.getReference (i);

        /* Fire in three cases:
         *   1. beatTarget < 0 -- caller asked for immediate.
         *   2. We have transport info AND beatTarget falls inside
         *      this block's beat range -- sample-accurate launch.
         *   3. We have transport info AND beatTarget is BEFORE this
         *      block's start -- caller queued the target before our
         *      drain caught up; fire now (catch-up).  Without this
         *      branch, a launch scheduled at beat 4 by a UI tick at
         *      beat 4.05 would never fire because beatTarget never
         *      lands inside a future block range. */
        const bool fireNow =
            (p.beatTarget < 0.0)
            || (blockStartBeat >= 0.0
                && p.beatTarget <  blockEndBeat);

        if (! fireNow)
            continue;

        if (p.wantPlaying)
        {
            if (activeStream_ != nullptr)
                retireStream (activeStream_.release());
            activeStream_.reset (p.stream);
            activeStreamRegionId_ = p.regionId;

            /* Latch envelope state for the new region. */
            activeGainLinear_     = p.gainLinear;
            activeFadeInSamples_  = p.fadeInSamples;
            activeFadeOutSamples_ = p.fadeOutSamples;
            activeLengthSamples_  = p.regionLengthSamples;
            activeSamplesPlayed_  = 0;
        }
        else
        {
            if (activeStream_ != nullptr)
                retireStream (activeStream_.release());
            activeStreamRegionId_ = juce::Uuid::null();
            activeGainLinear_     = 1.0f;
            activeFadeInSamples_  = 0;
            activeFadeOutSamples_ = 0;
            activeLengthSamples_  = 0;
            activeSamplesPlayed_  = 0;
        }
        pendingActions_.remove (i);
    }
}

void
AudioClipNode::retireStream (Playback_DS* dead) noexcept
{
    if (dead == nullptr) return;

    int s1, sz1, s2, sz2;
    graveyardFifo_.prepareToWrite (1, s1, sz1, s2, sz2);
    if (sz1 + sz2 < 1)
        return;

    if (sz1 > 0)
        graveyardStorage_[(std::size_t) s1] = dead;
    else
        graveyardStorage_[(std::size_t) s2] = dead;
    graveyardFifo_.finishedWrite (1);
}

//==============================================================================
// Message-thread Timer + AsyncUpdater

void
AudioClipNode::timerCallback()
{
    /* 1. Drain graveyard. */
    const int ready = graveyardFifo_.getNumReady();
    if (ready > 0)
    {
        int s1, sz1, s2, sz2;
        graveyardFifo_.prepareToRead (ready, s1, sz1, s2, sz2);
        for (int i = 0; i < sz1; ++i)
            delete graveyardStorage_[(std::size_t) (s1 + i)];
        for (int i = 0; i < sz2; ++i)
            delete graveyardStorage_[(std::size_t) (s2 + i)];
        graveyardFifo_.finishedRead (ready);
    }

    /* 2. Poll recording finalisation.  State machine:
     *
     *   wasRecordingFinalising_ = false (initial)
     *       record_ exists, recording()==true   -> set to true
     *       record_ exists, recording()==false  -> nothing (between
     *           handleAsyncUpdate creating record_ and IO thread
     *           starting; or finalised already and just hanging
     *           around -- shouldn't happen but defensive)
     *
     *   wasRecordingFinalising_ = true
     *       recording()==true   -> stay true
     *       recording()==false  -> EDGE: finalise.  Read
     *           finalised_file(), invoke commit handler, drop
     *           record_, clear flag.
     *
     *  Edge case: record_->stop() called by audio thread, then IO
     *  thread takes >1 timer-tick to actually close the file.
     *  recording() stays true throughout, so we wait correctly.
     *
     *  Edge case: capture < 100 ms.  Audio thread starts + stops
     *  recording within one timer interval -> we may never observe
     *  recording==true and miss the finalise edge.  Tolerable for
     *  v1 (sub-100ms takes aren't a real DAW use case).  v2 could
     *  add an atomic "ever recorded" flag set by the audio thread. */
    if (record_ != nullptr)
    {
        const bool nowRecording = record_->recording();

        if (wasRecordingFinalising_ && ! nowRecording)
        {
            const auto file = record_->finalised_file();
            juce::Logger::writeToLog (
                juce::String ("[AudioClipNode::timerCallback] recording finalised file=")
                + file.getFullPathName()
                + " existsAsFile=" + (file.existsAsFile() ? "yes" : "no"));

            std::function<void (const juce::File&)> cb;
            {
                const juce::ScopedLock sl (engineLock_);
                cb = onRecordingCommitted_;
            }
            record_.reset();
            wasRecordingFinalising_ = false;
            if (cb && file.existsAsFile())
            {
                juce::Logger::writeToLog (" -> firing onRecordingCommitted handler");
                cb (file);
            }
            else
            {
                juce::Logger::writeToLog (
                    juce::String (" -> NOT firing handler: cb=")
                    + (cb ? "set" : "null")
                    + " file_exists=" + (file.existsAsFile() ? "yes" : "no"));
            }
        }
        else
        {
            wasRecordingFinalising_ = nowRecording;
        }
    }
}

void
AudioClipNode::handleAsyncUpdate()
{
    /* Audio thread requested a fresh Record_DS.  Build one and hand
     * off via pendingFreshRecord_. */
    if (! pendingRecordStart_.exchange (false, std::memory_order_acquire))
        return;

    juce::Logger::writeToLog ("[AudioClipNode::handleAsyncUpdate] record start requested");

    /* If pendingFreshRecord_ already holds one we didn't pick up,
     * drop it (the audio thread will swap in the new one). */
    if (auto* stale = pendingFreshRecord_.exchange (nullptr))
    {
        juce::Logger::writeToLog (" -> dropping stale pendingFreshRecord_");
        delete stale;
    }

    const juce::File basename = composeRecordingBasename();
    juce::Logger::writeToLog (
        juce::String (" -> basename=") + basename.getFullPathName()
        + " sr=" + juce::String (sampleRate_, 0)
        + " block=" + juce::String (blockSize_)
        + " ch=" + juce::String (numChannels_));

    auto fresh = Record_DS::create (basename,
                                    juce::String (kCaptureFormat),
                                    (nframes_t) sampleRate_,
                                    (float) sampleRate_,
                                    (nframes_t) blockSize_,
                                    numChannels_);
    if (fresh == nullptr)
    {
        juce::Logger::writeToLog (
            juce::String (" -> Record_DS::create FAILED for ")
            + basename.getFullPathName());
        return;
    }

    fresh->start (0 /*start_source_frame -- new file starts at 0*/);

    juce::Logger::writeToLog (
        " -> Record_DS armed, handed off to audio thread via pendingFreshRecord_");
    pendingFreshRecord_.store (fresh.release(), std::memory_order_release);
}

//==============================================================================
// State

void
AudioClipNode::getStateInformation (juce::MemoryBlock& dest)
{
    dest.setSize (0);
}

void
AudioClipNode::setStateInformation (const void* /*data*/, int /*sz*/)
{
}

} // namespace element
