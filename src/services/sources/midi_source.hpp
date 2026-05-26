// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "services/sources/source.hpp"
#include "services/timeline/midi_note.hpp"

#include <element/juce/audio_basics.hpp>

#include <vector>

namespace element {

/** Source kind: an imported or recorded MIDI file.  Peer to
 *  AudioFileSource (services/sources/audiofilesource.hpp).
 *
 *  Unlike AudioFileSource, MidiSource embeds the actual SMF bytes
 *  in-process (and in the session save).  SMF files are small (KB
 *  range) so we keep the session self-contained -- the user can move
 *  the .sls anywhere without losing referenced MIDI data.  Audio
 *  files stay path-referenced because they're orders of magnitude
 *  larger.
 *
 *  The original on-disk juce::File path is retained for the UI
 *  (region tooltip, region name auto-fill) but the path is not
 *  required after import -- re-loading a session with a missing
 *  original .mid file still works because the bytes are inline.
 *
 *  Persistence: SourceRegistry serialises MidiSource as
 *  (uuid, path-as-string, noteCount, smfBytes-base64).  Base64 keeps
 *  the wrapping ValueTree XML-safe; the gzip pass on the registry
 *  blob mitigates the base64 size overhead.
 *
 *  See piano-roll-automation-design.md Section 4 Phase 2. */
class MidiSource : public Source
{
public:
    using Ptr = juce::ReferenceCountedObjectPtr<MidiSource>;

    /** Construct from raw SMF bytes already in memory.  Computes the
     *  note count by parsing the SMF on the message thread (cheap;
     *  juce::MidiFile parse is in-memory).  Caller retains nothing --
     *  the SMF bytes are moved-in.
     *
     *  noteCount can be supplied directly when the caller has already
     *  parsed (e.g. session restore path that doesn't need a re-parse);
     *  pass -1 to trigger an immediate parse + count. */
    MidiSource (juce::Uuid id,
                const juce::File& originalPath,
                juce::MemoryBlock smfBytes,
                int noteCount = -1);

    Kind kind() const noexcept override { return Kind::Midi; }

    const juce::File&        file()      const noexcept { return originalPath_; }
    const juce::MemoryBlock& smfBytes()  const noexcept { return smfBytes_; }
    int                      noteCount() const noexcept { return noteCount_; }

    juce::String displayName() const override
    {
        if (originalPath_.getFullPathName().isEmpty())
            return juce::String ("MIDI ") + uuid().toString().substring (0, 4);
        return originalPath_.getFileNameWithoutExtension();
    }

    /** Duration of the source in beats.  Walks the parsed MIDI file
     *  end-of-track timestamps and returns the maximum across tracks.
     *  Pure -- safe to call from any thread (re-parses on every call;
     *  cheap on the message thread).  bpm is unused for MIDI (the
     *  SMF carries its own time base). */
    double durationBeats (double sampleRate, double bpm) const override;

    /** Parse the embedded SMF bytes into a juce::MidiFile.  Returns
     *  an empty MidiFile (no tracks) on parse failure.  Message-
     *  thread caller. */
    juce::MidiFile toMidiFile() const;

    /** Convert a parsed juce::MidiFile into a MidiNoteRegion-ready
     *  NoteList in beat domain.  Pairs NoteOn with the matching
     *  NoteOff using juce::MidiMessageSequence::getIndexOfMatchingKeyUp.
     *
     *  PPQ-based timing only (the common case for music files).  SMPTE
     *  timecode (negative timeFormat) falls back to a per-event tick
     *  treated as a quarter-note step -- pragmatic, since SMPTE MIDI
     *  is rare outside film cue lists.
     *
     *  Static helper rather than instance method so the import path
     *  can call it with a freshly-parsed MidiFile before deciding
     *  whether to register a Source (e.g. when validating a drop). */
    static std::vector<MidiNote> extractNotes (const juce::MidiFile& mf);

private:
    juce::File         originalPath_;
    juce::MemoryBlock  smfBytes_;
    int                noteCount_ { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiSource)
};

} // namespace element
