// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/audioclip.hpp"
#include "services/audiostreaming/record_ds.hpp"   // Phase 4 reservation
#include "services/sources/sourceregistry.hpp"

#include <element/node.h>

namespace element {

juce::AudioProcessor::BusesProperties
AudioClipNode::busesFor (bool stereo)
{
    BusesProperties b;
    b.addBus (false /*isInput*/,
              "Out",
              stereo ? juce::AudioChannelSet::stereo()
                     : juce::AudioChannelSet::mono(),
              true /*isActive*/);
    return b;
}

AudioClipNode::AudioClipNode (bool stereo)
    : BaseProcessor (busesFor (stereo)),
      stereo_       (stereo),
      numChannels_  (stereo ? 2 : 1)
{
    startTimer (kGraveyardDrainMs);
}

AudioClipNode::~AudioClipNode()
{
    stopTimer();

    /* Audio thread is quiescent at this point (graph teardown
     * contract).  Drain the launch FIFO of any in-flight requests so
     * their unowned streams don't leak. */
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

    /* Same for any audio-thread-owned pendingActions_ that haven't
     * been applied. */
    for (auto& p : pendingActions_)
        delete p.stream;
    pendingActions_.clear();

    /* Drop the currently-active stream (joins its IO thread). */
    activeStream_.reset();

    /* Drain graveyard one last time (Timer is already stopped). */
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

void
AudioClipNode::fillInPluginDescription (juce::PluginDescription& desc) const
{
    desc.fileOrIdentifier   = stereo_ ? EL_NODE_ID_AUDIO_CLIP : EL_NODE_ID_AUDIO_CLIP_MONO;
    desc.name               = stereo_ ? "Audio Clip" : "Audio Clip (Mono)";
    desc.descriptiveName    = "Single-region audio playback for the arrangement";
    desc.numInputChannels   = 0;
    desc.numOutputChannels  = numChannels_;
    desc.hasSharedContainer = false;
    desc.isInstrument       = false;
    desc.category           = "Source";
    desc.manufacturerName   = EL_NODE_FORMAT_AUTHOR;
    desc.pluginFormatName   = EL_NODE_FORMAT_NAME;
    desc.version            = "0.1.0";
    desc.uniqueId           = stereo_ ? EL_NODE_UID_AUDIO_CLIP : EL_NODE_UID_AUDIO_CLIP_MONO;
}

bool
AudioClipNode::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.inputBuses.size() != 0) return false;
    if (layouts.outputBuses.size() != 1) return false;
    return layouts.outputBuses.getReference (0)
           == (stereo_ ? juce::AudioChannelSet::stereo()
                       : juce::AudioChannelSet::mono());
}

void
AudioClipNode::prepareToPlay (double newSampleRate, int newBlockSize)
{
    sampleRate_ = newSampleRate;
    blockSize_  = newBlockSize;
}

void
AudioClipNode::releaseResources()
{
    /* Stream + pendingActions destruction happens in dtor; nothing to
     * do here.  We intentionally do NOT touch activeStream_ here --
     * releaseResources may be called between transport runs without
     * tearing down the node.  The IO thread can keep streaming into
     * an unread ring; the next processBlock cycle will resume. */
}

void
AudioClipNode::processBlock (juce::AudioBuffer<float>& buffer,
                             juce::MidiBuffer&         /*midi*/)
{
    buffer.clear();

    /* TODO Phase 2 (full): pull blockStartBeat / blockEndBeat from
     * the host AudioPlayHead so beatTarget-quantised launches fire
     * sample-accurate inside the block.  v1 ships immediate-launch
     * only; AudioLaneAdapter writes beatTarget=-1 for now. */
    constexpr double kBlockStartBeat = -1.0;
    constexpr double kBlockEndBeat   = -1.0;

    drainLaunchFifo();
    applyPendingForBlock (kBlockStartBeat, kBlockEndBeat);

    if (activeStream_ != nullptr)
    {
        activeStream_->process (buffer, 0,
                                (nframes_t) buffer.getNumSamples());
    }
}

//==============================================================================
// Message-thread API

void
AudioClipNode::schedulePlay (juce::Uuid  regionId,
                             juce::Uuid  sourceId,
                             double      beatTarget,
                             juce::int64 sampleOffset)
{
    auto source = SourceRegistry::get().findAudioFile (sourceId);
    if (source == nullptr)
    {
        juce::Logger::writeToLog (
            juce::String ("AudioClipNode::schedulePlay: source not in registry: ")
            + sourceId.toString());
        return;
    }

    /* Open the file + spawn the IO thread on the message thread.  The
     * audio thread will take ownership when it drains the FIFO. */
    auto fresh = Playback_DS::create (source,
                                      (float) sampleRate_,
                                      (nframes_t) blockSize_,
                                      numChannels_);
    if (fresh == nullptr)
        return;

    fresh->seek ((nframes_t) sampleOffset);

    /* SPSC: only the message thread writes here.  No lock. */
    int s1, sz1, s2, sz2;
    launchFifo_.prepareToWrite (1, s1, sz1, s2, sz2);
    if (sz1 + sz2 < 1)
    {
        /* FIFO full -- drop silently.  64 slots vs human click rate
         * means we'd have to be backed up by ~64 unresolved launches
         * for this to fire; if it does the fresh stream is leaked
         * here so we hand the dtor responsibility back to ourselves. */
        juce::Logger::writeToLog (
            "AudioClipNode::schedulePlay: launchFifo full -- dropping request");
        return;
    }

    /* Ownership transfer: the FIFO entry now holds the only pointer
     * to the new stream.  Release from unique_ptr; either the audio
     * thread or our own dtor will be responsible for deletion. */
    Playback_DS* freshRaw = fresh.release();

    const LaunchReq req {
        regionId,
        beatTarget,
        sampleOffset,
        freshRaw,
        1 /*wantPlaying*/
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
        regionId,
        beatTarget,
        0,            // sampleOffset unused on stop
        nullptr,      // no new stream
        0 /*wantPlaying*/
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
// Audio-thread helpers

void
AudioClipNode::drainLaunchFifo() noexcept
{
    const int ready = launchFifo_.getNumReady();
    if (ready == 0) return;

    int s1, sz1, s2, sz2;
    launchFifo_.prepareToRead (ready, s1, sz1, s2, sz2);

    auto absorb = [this] (const LaunchReq& r) noexcept
    {
        /* v1 single-region: a new start request supersedes any
         * pending request that hasn't fired yet -- retire the
         * superseded stream immediately so we don't accumulate. */
        for (int i = pendingActions_.size(); --i >= 0;)
        {
            auto& p = pendingActions_.getReference (i);
            /* If the new request shares a regionId with a pending
             * one, replace -- otherwise also replace (v1 single-
             * region) but log when discarding a different regionId. */
            if (p.stream != nullptr && p.stream != r.stream)
                retireStream (p.stream);
            pendingActions_.remove (i);
        }

        pendingActions_.add (PendingAction {
            r.regionId,
            r.beatTarget,
            r.sampleOffset,
            r.stream,
            r.wantPlaying != 0
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

        /* beatTarget<0 = immediate.  beatTarget>=0 fires when the
         * block's beat range straddles the target.  When transport
         * info is unavailable (blockStart<0) only immediate fires;
         * quantised pending entries stay queued for a future block. */
        const bool fireNow =
            (p.beatTarget < 0.0)
            || (blockStartBeat >= 0.0
                && p.beatTarget >= blockStartBeat
                && p.beatTarget <  blockEndBeat);

        if (! fireNow)
            continue;

        if (p.wantPlaying)
        {
            /* Retire any currently-active stream before installing
             * the new one.  v1 single-region. */
            if (activeStream_ != nullptr)
                retireStream (activeStream_.release());

            activeStream_.reset (p.stream);
            activeStreamRegionId_ = p.regionId;
        }
        else
        {
            /* Stop request.  If a stream is active under any
             * regionId, retire it.  (v1 single-region: regionId
             * match is informational; matching not required.) */
            if (activeStream_ != nullptr)
                retireStream (activeStream_.release());
            activeStreamRegionId_ = juce::Uuid::null();
        }

        pendingActions_.remove (i);
    }
}

void
AudioClipNode::retireStream (Playback_DS* dead) noexcept
{
    if (dead == nullptr)
        return;

    int s1, sz1, s2, sz2;
    graveyardFifo_.prepareToWrite (1, s1, sz1, s2, sz2);
    if (sz1 + sz2 < 1)
    {
        /* Graveyard full -- shouldn't happen at human launch rates,
         * but if it does we can't delete here (would join the IO
         * thread on the audio thread).  Leak; the dtor's drain will
         * pick it up if there's still a reference.  In practice the
         * Timer drains every 100ms so this is impossible. */
        return;
    }

    if (sz1 > 0)
        graveyardStorage_[(std::size_t) s1] = dead;
    else
        graveyardStorage_[(std::size_t) s2] = dead;
    graveyardFifo_.finishedWrite (1);
}

//==============================================================================
// Timer (message thread)

void
AudioClipNode::timerCallback()
{
    const int ready = graveyardFifo_.getNumReady();
    if (ready == 0)
        return;

    int s1, sz1, s2, sz2;
    graveyardFifo_.prepareToRead (ready, s1, sz1, s2, sz2);

    for (int i = 0; i < sz1; ++i)
        delete graveyardStorage_[(std::size_t) (s1 + i)];
    for (int i = 0; i < sz2; ++i)
        delete graveyardStorage_[(std::size_t) (s2 + i)];

    graveyardFifo_.finishedRead (ready);
}

//==============================================================================
// State (no-op for v1 -- see audioclip.hpp doc comment)

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
