// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/sources/sourceregistry.hpp"

#include "services/audiostreaming/audio_file_sf.hpp"

#include <element/session.hpp>
#include <element/node.hpp>

#include <sndfile.h>

#include <cstring>

namespace element {

SourceRegistry& SourceRegistry::get()
{
    static SourceRegistry instance;
    return instance;
}

AudioFileSource::Ptr SourceRegistry::registerAudioFile (juce::Uuid uuid,
                                                         const juce::File& file,
                                                         int sourceSampleRate,
                                                         int numChannels,
                                                         juce::int64 durationSamples)
{
    AudioFileSource::Ptr existing;
    AudioFileSource::Ptr fresh;
    {
        const juce::ScopedLock sl (lock_);
        auto it = audioFiles_.find (uuid);
        if (it != audioFiles_.end())
            existing = it->second;
        else
        {
            fresh = new AudioFileSource (uuid, file, sourceSampleRate,
                                          numChannels, durationSamples);
            audioFiles_.emplace (uuid, fresh);
        }
    }
    if (existing != nullptr) return existing;
    sendChangeMessage();
    return fresh;
}

AudioFileSource::Ptr SourceRegistry::importFromFile (const juce::File& file)
{
    if (! file.existsAsFile())
    {
        juce::Logger::writeToLog (
            juce::String ("SourceRegistry::importFromFile: file not found: ")
            + file.getFullPathName());
        return nullptr;
    }

    /* libsndfile open just for metadata.  POSIX-backed; no
     * juce::File operations on the hot path.  See
     * timeline-audio-design.md Section 0a (Linux-native I/O rule). */
    SF_INFO si;
    std::memset (&si, 0, sizeof (si));
    SNDFILE* sf = sf_open (file.getFullPathName().toRawUTF8(), SFM_READ, &si);
    if (sf == nullptr)
    {
        juce::Logger::writeToLog (
            juce::String ("SourceRegistry::importFromFile: sf_open failed: ")
            + file.getFullPathName() + " (" + sf_strerror (nullptr) + ")");
        return nullptr;
    }

    const int        sr  = si.samplerate;
    const int        ch  = si.channels;
    const juce::int64 dur = (juce::int64) si.frames;
    sf_close (sf);

    if (sr <= 0 || ch <= 0 || dur <= 0)
    {
        juce::Logger::writeToLog (
            juce::String ("SourceRegistry::importFromFile: invalid metadata for ")
            + file.getFullPathName());
        return nullptr;
    }

    /* Fresh uuid for each import.  Dedup by path could be a v2
     * optimisation but two regions referencing the same file via
     * distinct uuids already share libsndfile reads at the
     * Playback_DS layer (each opens its own fd anyway). */
    return registerAudioFile (juce::Uuid(), file, sr, ch, dur);
}

AudioFileSource::Ptr SourceRegistry::findAudioFile (juce::Uuid uuid) const
{
    const juce::ScopedLock sl (lock_);
    auto it = audioFiles_.find (uuid);
    return it != audioFiles_.end() ? it->second : nullptr;
}

Source::Ptr SourceRegistry::findByUuid (juce::Uuid uuid) const
{
    /* v1: only audio sources are stored.  VhtSequenceSource resolves
     * via resolveVhtSequence() against the active graph; callers
     * that have a tracker-region's (trackerNodeId, sequenceIdx) pair
     * use that path directly.  Returning nullptr here for unknown
     * uuids is fine -- adapters skip dispatch on null source. */
    if (auto p = findAudioFile (uuid))
        return p.get();
    return nullptr;
}

namespace {
/* Recursively scan a graph (and any subgraph children) for a node
 * with matching uuid.  Returns the wrapping Node so the caller can
 * resolve it to a Processor.  Single-pass, O(N) over reachable nodes. */
Node findNodeByUuid (const Node& root, juce::Uuid target)
{
    const int n = root.getNumNodes();
    for (int i = 0; i < n; ++i)
    {
        Node child = root.getNode (i);
        if (! child.isValid()) continue;
        if (child.getUuid() == target) return child;
        if (child.isGraph())
        {
            Node nested = findNodeByUuid (child, target);
            if (nested.isValid()) return nested;
        }
    }
    return Node();
}
}

VhtSequenceSource::Ptr SourceRegistry::resolveVhtSequence (const Session& session,
                                                            juce::Uuid trackerNodeId,
                                                            int sequenceIdx) const
{
    /* Bound-check is the caller's responsibility against the live
     * TrackerNode; here we just confirm the node exists.  Returns a
     * fresh VhtSequenceSource on every call -- they're tiny (two
     * ints) so no caching needed. */
    const Node active = session.getActiveGraph();
    if (! active.isValid()) return nullptr;

    const Node target = findNodeByUuid (active, trackerNodeId);
    if (! target.isValid()) return nullptr;

    if (sequenceIdx < 0) return nullptr;
    return new VhtSequenceSource (trackerNodeId, sequenceIdx);
}

void SourceRegistry::getStateInformation (juce::MemoryBlock& dest)
{
    juce::ValueTree tree ("sourceRegistry");

    {
        const juce::ScopedLock sl (lock_);
        for (const auto& kv : audioFiles_)
        {
            const auto& src = kv.second;
            if (src == nullptr) continue;

            juce::ValueTree node ("audioFile");
            node.setProperty ("id",       src->uuid().toString(),                       nullptr);
            node.setProperty ("path",     src->file().getFullPathName(),                nullptr);
            node.setProperty ("sr",       src->sourceSampleRate(),                      nullptr);
            node.setProperty ("ch",       src->numChannels(),                           nullptr);
            node.setProperty ("dur",      (juce::int64) src->durationSamples(),         nullptr);
            tree.appendChild (node, nullptr);
        }
    }

    juce::MemoryOutputStream stream (dest, false);
    {
        juce::GZIPCompressorOutputStream gzip (stream);
        tree.writeToStream (gzip);
    }
}

void SourceRegistry::setStateInformation (const void* data, int size)
{
    {
        const juce::ScopedLock sl (lock_);
        audioFiles_.clear();
        hasLoaded_ = false;
    }

    if (data == nullptr || size <= 0) return;

    juce::ValueTree tree;
    {
        juce::MemoryInputStream stream (data, (size_t) size, false);
        juce::GZIPDecompressorInputStream gunzip (stream);
        tree = juce::ValueTree::readFromStream (gunzip);
    }
    if (! tree.isValid() || tree.getType() != juce::Identifier ("sourceRegistry"))
        return;

    {
        const juce::ScopedLock sl (lock_);
        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            const auto node = tree.getChild (i);
            if (node.getType() != juce::Identifier ("audioFile")) continue;

            const juce::Uuid id (node.getProperty ("id").toString());
            const juce::String path  = node.getProperty ("path").toString();
            const int sr             = (int) node.getProperty ("sr", 0);
            const int ch             = (int) node.getProperty ("ch", 0);
            const juce::int64 dur    = (juce::int64) node.getProperty ("dur", (juce::int64) 0);
            if (path.isEmpty() || id.isNull()) continue;

            AudioFileSource::Ptr p =
                new AudioFileSource (id, juce::File (path), sr, ch, dur);
            audioFiles_.emplace (id, p);
        }
        hasLoaded_ = true;
    }

    sendChangeMessage();
}

void SourceRegistry::clearAll()
{
    {
        const juce::ScopedLock sl (lock_);
        audioFiles_.clear();
        hasLoaded_ = false;
    }
    sendChangeMessage();
}

bool SourceRegistry::hasLoaded() const noexcept
{
    const juce::ScopedLock sl (lock_);
    return hasLoaded_;
}

void SourceRegistry::resetLoadFlag() noexcept
{
    const juce::ScopedLock sl (lock_);
    hasLoaded_ = false;
}

} // namespace element
