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

MidiSource::Ptr SourceRegistry::registerMidiFile (juce::Uuid uuid,
                                                   const juce::File& originalPath,
                                                   juce::MemoryBlock smfBytes,
                                                   int noteCount)
{
    MidiSource::Ptr existing;
    MidiSource::Ptr fresh;
    {
        const juce::ScopedLock sl (lock_);
        auto it = midiFiles_.find (uuid);
        if (it != midiFiles_.end())
            existing = it->second;
        else
        {
            fresh = new MidiSource (uuid, originalPath,
                                     std::move (smfBytes), noteCount);
            midiFiles_.emplace (uuid, fresh);
        }
    }
    if (existing != nullptr) return existing;
    sendChangeMessage();
    return fresh;
}

MidiSource::Ptr SourceRegistry::importMidiFromFile (const juce::File& file)
{
    if (! file.existsAsFile())
    {
        juce::Logger::writeToLog (
            juce::String ("SourceRegistry::importMidiFromFile: file not found: ")
            + file.getFullPathName());
        return nullptr;
    }

    /* POSIX-backed file read for the raw bytes -- juce::File handles
     * the read here (small SMF files, message thread, no wineserver
     * cost worth avoiding).  The on-disk size is bounded; refuse
     * pathological inputs to avoid silently OOMing the session. */
    constexpr juce::int64 kMaxSmfBytes = 16 * 1024 * 1024;   /* 16 MiB */
    if (file.getSize() > kMaxSmfBytes)
    {
        juce::Logger::writeToLog (
            juce::String ("SourceRegistry::importMidiFromFile: file too large (")
            + juce::String (file.getSize()) + " bytes): "
            + file.getFullPathName());
        return nullptr;
    }

    juce::MemoryBlock bytes;
    if (! file.loadFileAsData (bytes) || bytes.getSize() == 0)
    {
        juce::Logger::writeToLog (
            juce::String ("SourceRegistry::importMidiFromFile: read failed: ")
            + file.getFullPathName());
        return nullptr;
    }

    /* Validate parse before committing to the registry.  An unparseable
     * file should not register an empty MidiSource. */
    juce::MidiFile mf;
    {
        juce::MemoryInputStream in (bytes.getData(), bytes.getSize(), false);
        if (! mf.readFrom (in))
        {
            juce::Logger::writeToLog (
                juce::String ("SourceRegistry::importMidiFromFile: SMF parse failed: ")
                + file.getFullPathName());
            return nullptr;
        }
    }

    int noteCount = 0;
    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        const auto* seq = mf.getTrack (t);
        if (seq == nullptr) continue;
        for (int i = 0; i < seq->getNumEvents(); ++i)
        {
            const auto& msg = seq->getEventPointer (i)->message;
            if (msg.isNoteOn() && msg.getVelocity() > 0)
                ++noteCount;
        }
    }

    return registerMidiFile (juce::Uuid(), file, std::move (bytes), noteCount);
}

MidiSource::Ptr SourceRegistry::findMidiFile (juce::Uuid uuid) const
{
    const juce::ScopedLock sl (lock_);
    auto it = midiFiles_.find (uuid);
    return it != midiFiles_.end() ? it->second : nullptr;
}

Source::Ptr SourceRegistry::findByUuid (juce::Uuid uuid) const
{
    /* Check both stores.  VhtSequenceSource still resolves via
     * resolveVhtSequence() against the active graph; callers with a
     * tracker-region's (trackerNodeId, sequenceIdx) pair use that
     * path directly.  Returning nullptr here for unknown uuids is fine
     * -- adapters skip dispatch on null source. */
    if (auto p = findAudioFile (uuid))
        return p.get();
    if (auto p = findMidiFile (uuid))
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

        /* MIDI sources: embed SMF bytes base64 inside the session.
         * The outer GZIP wrapping (below) mitigates base64's 4/3 size
         * overhead.  Audio files stay path-referenced because they're
         * orders of magnitude larger and benefit from the in-place
         * libsndfile streaming path. */
        for (const auto& kv : midiFiles_)
        {
            const auto& src = kv.second;
            if (src == nullptr) continue;

            juce::ValueTree node ("midiFile");
            node.setProperty ("id",       src->uuid().toString(),         nullptr);
            node.setProperty ("path",     src->file().getFullPathName(),  nullptr);
            node.setProperty ("nc",       src->noteCount(),               nullptr);
            const auto& bytes = src->smfBytes();
            if (bytes.getSize() > 0)
            {
                node.setProperty ("smf",
                                  juce::Base64::toBase64 (bytes.getData(), bytes.getSize()),
                                  nullptr);
            }
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
            if (node.getType() == juce::Identifier ("audioFile"))
            {
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
            else if (node.getType() == juce::Identifier ("midiFile"))
            {
                const juce::Uuid id (node.getProperty ("id").toString());
                if (id.isNull()) continue;

                const juce::String path = node.getProperty ("path").toString();
                const int nc            = (int) node.getProperty ("nc", -1);

                juce::MemoryBlock bytes;
                const juce::String smf64 = node.getProperty ("smf").toString();
                if (smf64.isNotEmpty())
                {
                    juce::MemoryOutputStream out (bytes, false);
                    if (! juce::Base64::convertFromBase64 (out, smf64))
                        bytes.reset();
                }

                MidiSource::Ptr p =
                    new MidiSource (id, juce::File (path), std::move (bytes), nc);
                midiFiles_.emplace (id, p);
            }
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
        midiFiles_.clear();
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
