// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <boost/test/unit_test.hpp>

#include "services/sources/midi_source.hpp"
#include "services/sources/sourceregistry.hpp"

#include <element/juce/audio_basics.hpp>
#include <element/juce/core.hpp>

#include <cstring>

using element::MidiNote;
using element::MidiSource;
using element::SourceRegistry;

namespace {

/* Build a tiny in-memory SMF: format 0, one track, 4 notes (C-E-G-C
 * each holding one quarter at PPQ=96).  Returns the serialised SMF
 * bytes; the test feeds these straight to MidiSource. */
juce::MemoryBlock makeTestSmfBytes (int ppq = 96)
{
    juce::MidiFile mf;
    mf.setTicksPerQuarterNote (ppq);

    juce::MidiMessageSequence seq;
    /* Quarter notes at 60, 64, 67, 72; on/off pairs at tick boundaries
     * 0/96/192/288/384.  Timestamps in the sequence are in ticks. */
    const int pitches[] = { 60, 64, 67, 72 };
    for (int i = 0; i < 4; ++i)
    {
        const double on  = i       * (double) ppq;
        const double off = (i + 1) * (double) ppq;
        auto m1 = juce::MidiMessage::noteOn (1, pitches[i], (juce::uint8) 100);
        m1.setTimeStamp (on);
        auto m2 = juce::MidiMessage::noteOff (1, pitches[i]);
        m2.setTimeStamp (off);
        seq.addEvent (m1);
        seq.addEvent (m2);
    }
    mf.addTrack (seq);

    juce::MemoryBlock bytes;
    juce::MemoryOutputStream out (bytes, false);
    mf.writeTo (out);
    return bytes;
}

} // namespace

BOOST_AUTO_TEST_SUITE (MidiSourceTests)

BOOST_AUTO_TEST_CASE (construction_counts_notes_when_count_is_negative)
{
    /* Passing -1 for noteCount triggers an in-ctor parse + count. */
    auto bytes = makeTestSmfBytes();
    MidiSource src (juce::Uuid(), juce::File(), std::move (bytes), -1);
    BOOST_CHECK_EQUAL (src.noteCount(), 4);
    BOOST_CHECK (src.kind() == element::Source::Kind::Midi);
}

BOOST_AUTO_TEST_CASE (construction_trusts_supplied_note_count)
{
    /* When the caller supplies noteCount >= 0 the ctor does NOT
     * re-parse -- session restore relies on this to skip a parse on
     * every load. */
    auto bytes = makeTestSmfBytes();
    MidiSource src (juce::Uuid(), juce::File(), std::move (bytes), 42);
    BOOST_CHECK_EQUAL (src.noteCount(), 42);
}

BOOST_AUTO_TEST_CASE (duration_beats_returns_track_end)
{
    /* The test SMF ends at tick 384 (= 4 * PPQ).  durationBeats must
     * return 4.0 regardless of session bpm. */
    auto bytes = makeTestSmfBytes (96);
    MidiSource src (juce::Uuid(), juce::File(), std::move (bytes), -1);
    BOOST_CHECK_EQUAL (src.durationBeats (48000.0, 120.0), 4.0);
}

BOOST_AUTO_TEST_CASE (extract_notes_pairs_on_with_matching_off)
{
    /* Parse the test SMF and walk extractNotes; verify each NoteOn
     * picks up the right pair-off length + pitch + channel. */
    auto bytes = makeTestSmfBytes (96);
    MidiSource src (juce::Uuid(), juce::File(), bytes, -1);
    const auto mf = src.toMidiFile();
    auto notes = MidiSource::extractNotes (mf);
    BOOST_REQUIRE_EQUAL (notes.size(), (size_t) 4);
    const int expected[] = { 60, 64, 67, 72 };
    for (int i = 0; i < 4; ++i)
    {
        BOOST_CHECK_EQUAL (notes[(size_t) i].pitch,       expected[i]);
        BOOST_CHECK_EQUAL (notes[(size_t) i].velocity,    100);
        BOOST_CHECK_EQUAL (notes[(size_t) i].channel,     1);
        BOOST_CHECK_EQUAL (notes[(size_t) i].onBeat,      (double) i);
        BOOST_CHECK_EQUAL (notes[(size_t) i].lengthBeats, 1.0);
    }
}

BOOST_AUTO_TEST_CASE (extract_notes_handles_empty_midifile)
{
    juce::MidiFile mf;
    auto notes = MidiSource::extractNotes (mf);
    BOOST_CHECK (notes.empty());
}

/* ---------- Registry round-trip ---------- */

BOOST_AUTO_TEST_CASE (registry_round_trip_preserves_midi_sources)
{
    auto& reg = SourceRegistry::get();
    reg.clearAll();

    auto bytes = makeTestSmfBytes (96);
    const auto bytesCopy = bytes; /* keep a copy for size-compare */
    juce::Uuid id;
    auto p = reg.registerMidiFile (id, juce::File ("/tmp/test.mid"),
                                    std::move (bytes), 4);
    BOOST_REQUIRE (p != nullptr);
    BOOST_CHECK (reg.findMidiFile (id) == p);
    BOOST_CHECK (reg.findByUuid (id) != nullptr);

    /* Serialize + reset + restore. */
    juce::MemoryBlock blob;
    reg.getStateInformation (blob);
    reg.clearAll();
    BOOST_CHECK (reg.findMidiFile (id) == nullptr);

    reg.setStateInformation (blob.getData(), (int) blob.getSize());
    auto restored = reg.findMidiFile (id);
    BOOST_REQUIRE (restored != nullptr);
    BOOST_CHECK_EQUAL (restored->noteCount(), 4);
    BOOST_CHECK_EQUAL (restored->smfBytes().getSize(), bytesCopy.getSize());
    /* Spot-check the SMF header byte ("MThd"). */
    BOOST_CHECK_EQUAL (std::memcmp (restored->smfBytes().getData(),
                                     "MThd", 4), 0);

    reg.clearAll();
}

BOOST_AUTO_TEST_CASE (clearAll_drops_midi_sources_too)
{
    auto& reg = SourceRegistry::get();
    reg.clearAll();
    auto bytes = makeTestSmfBytes();
    juce::Uuid id;
    reg.registerMidiFile (id, juce::File ("/tmp/x.mid"), std::move (bytes), -1);
    BOOST_REQUIRE (reg.findMidiFile (id) != nullptr);
    reg.clearAll();
    BOOST_CHECK (reg.findMidiFile (id) == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()  /* MidiSourceTests */
