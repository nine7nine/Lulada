// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "services/sources/source.hpp"

namespace element {

/** Source kind: an audio file on disk.  Stub for Phase 1 -- holds
 *  the canonical metadata (path + intrinsic format info) but does
 *  NOT open the file or decode any audio yet.  Phase 3 lifts NON's
 *  Audio_File (libsndfile-backed, POSIX-only) and plumbs the actual
 *  streaming + decode.
 *
 *  Linux-native I/O rule (timeline-audio-design.md Section 0a):
 *  when Phase 3 lands, file access MUST use POSIX ::open / ::read /
 *  ::close directly (or libsndfile, which is POSIX-backed) -- never
 *  juce::File::createInputStream().  Same pattern as
 *  SamplerInstrument::prepareSlot at src/nodes/sampler.cpp:131-212.
 *
 *  Persistence: the SourceRegistry serialises AudioFileSource as
 *  (uuid, path-as-string, sourceSampleRate, numChannels,
 *  durationSamples).  Reopening a session re-reads the metadata
 *  from disk lazily on first use (NOT at registry restore time)
 *  to avoid blocking session-load on N file opens.
 *
 *  See timeline-audio-design.md Section 1.1.
 */
class AudioFileSource : public Source
{
public:
    using Ptr = juce::ReferenceCountedObjectPtr<AudioFileSource>;

    AudioFileSource (juce::Uuid id,
                     const juce::File& file,
                     int sourceSampleRate,
                     int numChannels,
                     juce::int64 durationSamples) noexcept
        : Source (id),
          file_ (file),
          sourceSampleRate_ (sourceSampleRate),
          numChannels_      (numChannels),
          durationSamples_  (durationSamples) {}

    Kind kind() const noexcept override { return Kind::Audio; }

    const juce::File& file() const noexcept { return file_; }
    int   numChannels()     const noexcept { return numChannels_; }
    int   sourceSampleRate() const noexcept { return sourceSampleRate_; }
    juce::int64 durationSamples() const noexcept { return durationSamples_; }

    juce::String displayName() const override
    {
        return file_.getFileNameWithoutExtension();
    }

    double durationBeats (double /*sessionSampleRate*/, double bpm) const override
    {
        if (sourceSampleRate_ <= 0 || bpm <= 0.0) return 0.0;
        const double seconds = (double) durationSamples_ / (double) sourceSampleRate_;
        return seconds * (bpm / 60.0);
    }

private:
    juce::File  file_;
    int         sourceSampleRate_;
    int         numChannels_;
    juce::int64 durationSamples_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioFileSource)
};

} // namespace element
