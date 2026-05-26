// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <boost/test/unit_test.hpp>

#include "services/timeline/lane.hpp"
#include "services/timeline/midi_note.hpp"
#include "services/timeline/midi_note_region.hpp"
#include "services/timeline/playlist.hpp"

#include <element/juce/core.hpp>

#include <memory>

using element::Lane;
using element::MidiNote;
using element::MidiNoteRegion;
using element::Playlist;

namespace {

std::unique_ptr<MidiNoteRegion> makeRegion (double pos, double len,
                                             int numNotes = 1)
{
    auto r = std::make_unique<MidiNoteRegion>();
    r->id            = juce::Uuid();
    r->positionBeats = pos;
    r->lengthBeats   = len;
    MidiNoteRegion::NoteList notes;
    for (int i = 0; i < numNotes; ++i)
    {
        MidiNote n;
        n.pitch       = 60 + i;
        n.onBeat      = (double) i * 0.25;
        n.lengthBeats = 0.25;
        notes.push_back (n);
    }
    r->setNotes (notes);
    return r;
}

} // namespace

BOOST_AUTO_TEST_SUITE (MidiLaneTests)

/* ---------- Playlist MIDI region API ---------- */

BOOST_AUTO_TEST_CASE (add_midi_region_returns_true_and_stores)
{
    Playlist p;
    BOOST_CHECK (p.addMidiRegion (makeRegion (0.0, 4.0, 4)));
    BOOST_REQUIRE_EQUAL (p.midiRegions().size(), (size_t) 1);
    BOOST_CHECK_EQUAL (p.midiRegions()[0]->noteCount(), (size_t) 4);
}

BOOST_AUTO_TEST_CASE (add_midi_region_rejects_null_or_negative_length)
{
    Playlist p;
    BOOST_CHECK (! p.addMidiRegion (nullptr));

    auto r = std::make_unique<MidiNoteRegion>();
    r->lengthBeats = -1.0;
    BOOST_CHECK (! p.addMidiRegion (std::move (r)));
}

BOOST_AUTO_TEST_CASE (add_midi_region_keeps_positionbeats_sort)
{
    Playlist p;
    p.addMidiRegion (makeRegion (8.0, 4.0));
    p.addMidiRegion (makeRegion (0.0, 4.0));
    p.addMidiRegion (makeRegion (4.0, 4.0));

    BOOST_REQUIRE_EQUAL (p.midiRegions().size(), (size_t) 3);
    BOOST_CHECK_EQUAL (p.midiRegions()[0]->positionBeats, 0.0);
    BOOST_CHECK_EQUAL (p.midiRegions()[1]->positionBeats, 4.0);
    BOOST_CHECK_EQUAL (p.midiRegions()[2]->positionBeats, 8.0);
}

BOOST_AUTO_TEST_CASE (find_midi_region_returns_inserted_pointer)
{
    Playlist p;
    auto r = makeRegion (4.0, 2.0);
    const juce::Uuid id = r->id;
    p.addMidiRegion (std::move (r));

    auto* found = p.findMidiRegion (id);
    BOOST_REQUIRE (found != nullptr);
    BOOST_CHECK_EQUAL (found->positionBeats, 4.0);

    BOOST_CHECK (p.findMidiRegion (juce::Uuid()) == nullptr);
}

BOOST_AUTO_TEST_CASE (remove_midi_region_returns_true_when_present)
{
    Playlist p;
    auto r = makeRegion (0.0, 4.0);
    const juce::Uuid id = r->id;
    p.addMidiRegion (std::move (r));
    BOOST_REQUIRE_EQUAL (p.midiRegions().size(), (size_t) 1);

    BOOST_CHECK (p.removeMidiRegion (id));
    BOOST_CHECK_EQUAL (p.midiRegions().size(), (size_t) 0);

    /* Idempotent: second remove returns false (already gone). */
    BOOST_CHECK (! p.removeMidiRegion (id));
}

BOOST_AUTO_TEST_CASE (for_each_midi_start_in_iterates_window)
{
    Playlist p;
    p.addMidiRegion (makeRegion (0.0, 1.0));
    p.addMidiRegion (makeRegion (2.0, 1.0));
    p.addMidiRegion (makeRegion (4.0, 1.0));
    p.addMidiRegion (makeRegion (8.0, 1.0));

    int seen = 0;
    p.forEachMidiStartIn (1.0, 6.0, [&] (const MidiNoteRegion& m) {
        BOOST_CHECK (m.positionBeats >= 1.0);
        BOOST_CHECK (m.positionBeats <  6.0);
        ++seen;
    });
    BOOST_CHECK_EQUAL (seen, 2);
}

/* ---------- Deep-copy of Playlist with MIDI regions ---------- */

BOOST_AUTO_TEST_CASE (copy_construct_deep_clones_midi_regions)
{
    Playlist src;
    src.addMidiRegion (makeRegion (0.0, 4.0, 3));
    src.addMidiRegion (makeRegion (4.0, 4.0, 5));

    Playlist copy (src);

    /* Same count, but different pointers (deep clone). */
    BOOST_REQUIRE_EQUAL (copy.midiRegions().size(), src.midiRegions().size());
    for (size_t i = 0; i < src.midiRegions().size(); ++i)
    {
        BOOST_CHECK (copy.midiRegions()[i].get() != src.midiRegions()[i].get());
        BOOST_CHECK (copy.midiRegions()[i]->id == src.midiRegions()[i]->id);
        BOOST_CHECK_EQUAL (copy.midiRegions()[i]->noteCount(),
                           src.midiRegions()[i]->noteCount());
    }
}

BOOST_AUTO_TEST_CASE (copy_assign_replaces_existing_midi_regions)
{
    Playlist a;
    a.addMidiRegion (makeRegion (0.0, 4.0, 2));

    Playlist b;
    b.addMidiRegion (makeRegion (8.0, 4.0, 5));
    b.addMidiRegion (makeRegion (12.0, 4.0, 7));

    a = b;
    BOOST_CHECK_EQUAL (a.midiRegions().size(), (size_t) 2);
    BOOST_CHECK_EQUAL (a.midiRegions()[0]->positionBeats, 8.0);
    BOOST_CHECK_EQUAL (a.midiRegions()[1]->positionBeats, 12.0);
    /* And not aliased -- editing a's first region's snapshot does not
     * affect b's first region's snapshot. */
    MidiNote extra;
    extra.pitch  = 99;
    extra.onBeat = 0.0;
    a.midiRegions()[0]->addNote (extra);
    BOOST_CHECK (a.midiRegions()[0]->noteCount() > b.midiRegions()[0]->noteCount());
}

/* ---------- ValueTree round-trip ---------- */

BOOST_AUTO_TEST_CASE (playlist_xml_round_trip_preserves_midi_regions)
{
    Playlist src;
    src.addMidiRegion (makeRegion (0.0, 4.0, 2));
    src.addMidiRegion (makeRegion (8.0, 4.0, 3));

    const auto vt = src.toValueTree();
    Playlist restored = Playlist::fromValueTree (vt);

    BOOST_REQUIRE_EQUAL (restored.midiRegions().size(), (size_t) 2);
    BOOST_CHECK_EQUAL (restored.midiRegions()[0]->positionBeats, 0.0);
    BOOST_CHECK_EQUAL (restored.midiRegions()[0]->noteCount(),   (size_t) 2);
    BOOST_CHECK_EQUAL (restored.midiRegions()[1]->positionBeats, 8.0);
    BOOST_CHECK_EQUAL (restored.midiRegions()[1]->noteCount(),   (size_t) 3);
}

BOOST_AUTO_TEST_CASE (lane_kind_midi_round_trips)
{
    Lane l;
    l.id   = juce::Uuid();
    l.name = juce::String ("midi lane");
    l.kind = Lane::Kind::Midi;
    l.playlist.addMidiRegion (makeRegion (0.0, 4.0, 1));

    const auto vt = l.toValueTree();
    Lane restored = Lane::fromValueTree (vt);

    BOOST_CHECK (restored.kind == Lane::Kind::Midi);
    BOOST_CHECK_EQUAL (restored.playlist.midiRegions().size(), (size_t) 1);
}

BOOST_AUTO_TEST_CASE (lane_kind_default_audio_when_attr_missing)
{
    /* Older sessions (Phase 1 + earlier) have no "kind" attr on the
     * lane element.  fromValueTree must default to Audio.  Verify by
     * removing the attr from a serialised tree before re-parsing. */
    Lane l;
    l.id   = juce::Uuid();
    l.kind = Lane::Kind::Midi;
    auto vt = l.toValueTree();
    vt.removeProperty (juce::Identifier ("kind"), nullptr);
    Lane restored = Lane::fromValueTree (vt);
    BOOST_CHECK (restored.kind == Lane::Kind::Audio);
}

BOOST_AUTO_TEST_SUITE_END()  /* MidiLaneTests */
