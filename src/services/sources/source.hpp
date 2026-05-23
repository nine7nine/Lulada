// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/core.hpp>
#include <element/juce/data_structures.hpp>

namespace element {

/** Base class for anything a Region can point at: a chunk of MIDI
 *  events (a vht sequence inside a TrackerNode), an audio file on
 *  disk, or future polymorphic kinds (piano-roll buffer, drum-grid
 *  pattern).
 *
 *  Reference-counted via juce::ReferenceCountedObject so a Region
 *  can keep its Source alive across re-registration and audio-thread
 *  reads can hold Ptr captures without locking.
 *
 *  Threading: Source::Ptr is safe to copy from any thread.  Concrete
 *  data access (sample buffers, sequence rows) is subject to each
 *  subclass's contract -- the audio thread reads via captured Ptrs;
 *  the message thread is the only mutator.
 *
 *  See timeline-audio-design.md Section 1.1.
 */
class Source : public juce::ReferenceCountedObject
{
public:
    using Ptr = juce::ReferenceCountedObjectPtr<Source>;

    enum class Kind : juce::uint8 {
        Midi   = 0,
        Audio  = 1,
    };

    ~Source() override = default;

    /** Discriminator for adapter dispatch.  Audio-vs-MIDI determines
     *  which TimelineAdapter handles the region at scheduler time. */
    virtual Kind kind() const noexcept = 0;

    /** Stable id used by Region.sourceId for cross-session reference.
     *  Generated once at construction; persistent in the saved file. */
    juce::Uuid uuid() const noexcept { return uuid_; }

    /** Human-readable label for UI surfaces (lane row, region tip).
     *  Subclass-defined; may change at runtime (e.g. file rename). */
    virtual juce::String displayName() const = 0;

    /** Duration in beats, computed from the Source's intrinsic length
     *  + the supplied session tempo.  Beats are the canonical
     *  arrangement-domain unit; samples come back at the audio
     *  thread.  Pure function -- safe to call from any thread.
     *
     *  bpm is the session tempo at the moment the duration is
     *  queried; this is informational (e.g. for default-length on
     *  drag-drop) -- regions store their own lengthBeats and don't
     *  re-derive from the source. */
    virtual double durationBeats (double sampleRate, double bpm) const = 0;

protected:
    Source() : uuid_ (juce::Uuid()) {}
    explicit Source (juce::Uuid id) : uuid_ (id) {}

private:
    juce::Uuid uuid_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Source)
};

} // namespace element
