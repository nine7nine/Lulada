// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "services/sources/source.hpp"
#include "services/sources/audiofilesource.hpp"
#include "services/sources/vhtsequencesource.hpp"

#include <unordered_map>

namespace element {

class Node;
class Session;

/** Session-global registry of Sources.
 *
 *  Holds AudioFileSource objects keyed by Uuid; AudioRegions in the
 *  arrangement reference them by id so multiple regions can share a
 *  single Source (Ardour pattern, mirrors how SampleBankPool already
 *  works for sampler banks).
 *
 *  VhtSequenceSources are NOT stored here -- they resolve live from
 *  the active graph (the sequence data lives inside the TrackerNode's
 *  module, persisted through the node's own getState/setState).  The
 *  resolver methods walk the graph each call; cheap O(num nodes).
 *
 *  Singleton.  Lifetime is process-bound; sessions explicitly
 *  serialize the audio-source table via getStateInformation /
 *  setStateInformation, and clearAll() resets at session-new /
 *  session-load-before-setState (parallel to SampleBankPool).
 *
 *  Thread safety: every accessor takes the internal lock_.  Audio
 *  thread reads via Source::Ptr captured at message-thread bind
 *  time -- no registry lock on the audio thread.  ChangeBroadcaster
 *  notifies on the message thread.
 *
 *  Linux-native I/O rule: this class does NOT touch the filesystem.
 *  AudioFileSource construction takes pre-collected metadata
 *  (numChannels, sampleRate, durationSamples); the actual file open
 *  + decode is done by callers using POSIX (or libsndfile in Phase
 *  3) and passed in as constructor args.  See
 *  timeline-audio-design.md Section 0a.
 *
 *  See timeline-audio-design.md Section 1.2.
 */
class SourceRegistry : public juce::ChangeBroadcaster
{
public:
    /** Process-lifetime singleton.  Constructed on first access. */
    static SourceRegistry& get();

    /** Register an audio file source.  Returns the existing Ptr if a
     *  Source with this uuid is already registered; otherwise stores
     *  the new instance and returns its Ptr.  Caller owns the file
     *  open / metadata-read before calling. */
    AudioFileSource::Ptr registerAudioFile (juce::Uuid uuid,
                                            const juce::File& file,
                                            int sourceSampleRate,
                                            int numChannels,
                                            juce::int64 durationSamples);

    /** Look up an AudioFileSource by uuid.  Returns nullptr if not
     *  registered. */
    AudioFileSource::Ptr findAudioFile (juce::Uuid uuid) const;

    /** Generic lookup -- returns base Ptr for any source kind, or
     *  nullptr.  Used by adapters when they only have the Region's
     *  sourceId and don't know the kind up front. */
    Source::Ptr findByUuid (juce::Uuid uuid) const;

    /** Resolve a (trackerNodeId, sequenceIdx) pair to a
     *  VhtSequenceSource.  Walks the active graph for the TrackerNode
     *  with the matching node uuid; cheap, O(N) over nodes.  Returns
     *  nullptr if no matching tracker is in the graph or sequenceIdx
     *  is out of range.
     *
     *  Caller must supply the active session reference so the
     *  registry doesn't need a singleton handle on Services.  Returns
     *  a fresh VhtSequenceSource on each call (cheap; just two ints).
     *  Result is not stored in this registry. */
    VhtSequenceSource::Ptr resolveVhtSequence (const Session& session,
                                               juce::Uuid trackerNodeId,
                                               int sequenceIdx) const;

    /** Persist / restore the audio-source table.  Wire format:
     *  ValueTree("sourceRegistry") with one ("audioFile") child per
     *  source, properties (id, path, sampleRate, channels, duration).
     *  GZip-compressed when written to the session XML's
     *  tags::sourceRegistry property (mirroring SampleBankPool). */
    void getStateInformation (juce::MemoryBlock&);
    void setStateInformation (const void* data, int size);

    /** Drop every source.  Called on session-new and at the top of
     *  setStateInformation before deserialising. */
    void clearAll();

    /** Mirrors SampleBankPool::hasLoaded -- session restore code can
     *  detect whether the registry has been populated this load
     *  cycle.  Currently informational only; reserved for future
     *  legacy-data migration. */
    bool hasLoaded() const noexcept;
    void resetLoadFlag() noexcept;

private:
    SourceRegistry() = default;
    ~SourceRegistry() override = default;

    SourceRegistry (const SourceRegistry&) = delete;
    SourceRegistry& operator= (const SourceRegistry&) = delete;

    /* Uuid hash adapter so juce::Uuid can key std::unordered_map. */
    struct UuidHash {
        std::size_t operator() (const juce::Uuid& u) const noexcept
        {
            const auto s = u.toString();
            return std::hash<std::string>{} (s.toStdString());
        }
    };

    mutable juce::CriticalSection lock_;
    std::unordered_map<juce::Uuid,
                       AudioFileSource::Ptr,
                       UuidHash> audioFiles_;
    bool hasLoaded_ = false;
};

} // namespace element
