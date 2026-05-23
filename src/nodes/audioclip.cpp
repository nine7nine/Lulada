// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/audioclip.hpp"
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
}

AudioClipNode::~AudioClipNode()
{
    /* Tear down the active stream OUTSIDE engineLock_ so the IO-thread
     * join doesn't race the lock; the destructor is single-threaded
     * with respect to processBlock by the graph teardown contract. */
    activeStream_.reset();
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
    /* One output bus of fixed width; reject any other shape. */
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

    /* If a stream is already active from a prior prepare cycle, its
     * IO thread is sized for the prior block size -- re-create on the
     * next playRegion call instead of trying to resize.  Block-size
     * changes between transport runs are rare enough that paying a
     * fresh stream cost is fine. */
}

void
AudioClipNode::releaseResources()
{
    /* Stop streaming + release the Playback_DS outside engineLock_
     * (its dtor joins the IO thread). */
    std::unique_ptr<Playback_DS> doomed;
    {
        const juce::ScopedLock sl (engineLock_);
        doomed = std::move (activeStream_);
        activeRegionId_ = juce::Uuid::null();
    }
    /* doomed.reset() at end of scope -> IO thread join here. */
}

void
AudioClipNode::processBlock (juce::AudioBuffer<float>& buffer,
                             juce::MidiBuffer&         /*midi*/)
{
    buffer.clear();

    const juce::ScopedLock sl (engineLock_);

    if (activeStream_ != nullptr)
    {
        activeStream_->process (buffer, 0,
                                (nframes_t) buffer.getNumSamples());
    }
}

void
AudioClipNode::playRegion (juce::Uuid regionId,
                           juce::Uuid sourceId,
                           juce::int64 sourceFrameOffset)
{
    /* Resolve source.  AudioClipNode does not hold a reference to
     * the Playlist; the lane / adapter looked up the region's
     * sourceId before calling us. */
    auto source = SourceRegistry::get().findAudioFile (sourceId);
    if (source == nullptr)
    {
        juce::Logger::writeToLog (
            juce::String ("AudioClipNode::playRegion: source not in registry: ")
            + sourceId.toString());
        return;
    }

    /* Spawn the new stream OUTSIDE engineLock_.  Playback_DS::create
     * opens the audio file via libsndfile and spawns the IO thread;
     * both syscalls are non-RT-safe and must not happen while we
     * hold the lock the audio thread will wait on. */
    auto fresh = Playback_DS::create (source,
                                      (float) sampleRate_,
                                      (nframes_t) blockSize_,
                                      numChannels_);
    if (fresh == nullptr)
        return;

    fresh->seek ((nframes_t) sourceFrameOffset);

    /* Swap under engineLock_; old stream gets released OUTSIDE the
     * lock so its IO-thread join doesn't block the audio thread. */
    std::unique_ptr<Playback_DS> doomed;
    {
        const juce::ScopedLock sl (engineLock_);
        doomed = std::move (activeStream_);
        activeStream_   = std::move (fresh);
        activeRegionId_ = regionId;
    }
    /* doomed.reset() at end of scope -> IO thread join here. */
}

void
AudioClipNode::stopRegion()
{
    std::unique_ptr<Playback_DS> doomed;
    {
        const juce::ScopedLock sl (engineLock_);
        doomed = std::move (activeStream_);
        activeRegionId_ = juce::Uuid::null();
    }
    /* doomed.reset() at end of scope -> IO thread join here. */
}

juce::Uuid
AudioClipNode::activeRegion() const noexcept
{
    const juce::ScopedLock sl (engineLock_);
    return activeRegionId_;
}

void
AudioClipNode::getStateInformation (juce::MemoryBlock& dest)
{
    /* v1: no persistent state beyond the bus configuration (which is
     * encoded in the node's plugin identifier, not the state blob).
     * The currently-playing region is transient -- restoring to a
     * mid-region playback state would require source-registry restore
     * to have already happened, and even then the audio engine starts
     * stopped by convention. */
    dest.setSize (0);
}

void
AudioClipNode::setStateInformation (const void* /*data*/, int /*sz*/)
{
    /* No-op: see getStateInformation. */
}

} // namespace element
