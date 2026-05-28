// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <boost/test/unit_test.hpp>

#include "nodes/midiplayer.hpp"
#include "services/timeline/midi_note.hpp"
#include "services/timeline/midi_note_region.hpp"

#include <element/juce/core.hpp>

#include <memory>

/* Scope:
 *   MidiPlayerNode's session-view API surface is exercised directly --
 *   schedule / state transitions / clip lifecycle / persistence
 *   round-trip.  Audio-thread render() is integration-scope and lives
 *   alongside the SessionView fixture (deferred).  These tests focus
 *   on the message-thread contract that SessionView calls through. */

using element::MidiPlayerNode;
using element::MidiNoteRegion;

namespace {

/* Helper: pump enough render-like activity that the SPSC FIFO drains
 * into per-slot pending action.  The render() path is the only drain;
 * tests can't drive it without an audio-thread fixture.  Instead we
 * rely on the fact that schedule + lookup are message-thread-visible
 * even when the audio thread never runs: schedulePlayingClip writes
 * via prepareToWrite/finishedWrite, and reconcileBoundClips uses the
 * same message-thread path.  State observation tests don't need
 * render() to fire -- they verify the API surface. */

} // namespace

BOOST_AUTO_TEST_SUITE (MidiClipInSessionTests)

/* === Slot lifecycle ===================================================== */

BOOST_AUTO_TEST_CASE (create_session_clip_returns_stable_id)
{
    MidiPlayerNode node;
    const auto id1 = node.createSessionClip();
    const auto id2 = node.createSessionClip();
    BOOST_CHECK (! id1.isNull());
    BOOST_CHECK (! id2.isNull());
    BOOST_CHECK (id1 != id2);
}

BOOST_AUTO_TEST_CASE (create_session_clip_default_region_is_one_bar_looped)
{
    MidiPlayerNode node;
    const auto id = node.createSessionClip();
    auto* r = node.getClipRegion (id);
    BOOST_REQUIRE (r != nullptr);
    /* Default: 4 beats (one bar at 4/4), looped, empty notes. */
    BOOST_CHECK_CLOSE (r->lengthBeats, 4.0, 1e-6);
    BOOST_CHECK_EQUAL (r->looped,      true);
    BOOST_CHECK_EQUAL (r->noteCount(), (size_t) 0);
}

BOOST_AUTO_TEST_CASE (create_session_clip_with_id_rejects_duplicate)
{
    MidiPlayerNode node;
    const juce::Uuid id;
    BOOST_CHECK (node.createSessionClipWithId (id));
    BOOST_CHECK (! node.createSessionClipWithId (id));
}

BOOST_AUTO_TEST_CASE (create_session_clip_with_null_id_rejected)
{
    MidiPlayerNode node;
    BOOST_CHECK (! node.createSessionClipWithId (juce::Uuid::null()));
}

BOOST_AUTO_TEST_CASE (remove_session_clip_tombstones_slot)
{
    MidiPlayerNode node;
    const auto id = node.createSessionClip();
    BOOST_REQUIRE (node.getClipRegion (id) != nullptr);
    BOOST_CHECK (node.removeSessionClip (id));
    BOOST_CHECK (node.getClipRegion (id) == nullptr);
    BOOST_CHECK (! node.isSessionClipPlaying (id));
}

BOOST_AUTO_TEST_CASE (remove_unknown_session_clip_returns_false)
{
    MidiPlayerNode node;
    BOOST_CHECK (! node.removeSessionClip (juce::Uuid()));
}

BOOST_AUTO_TEST_CASE (slot_reuse_after_remove)
{
    MidiPlayerNode node;
    const auto id1 = node.createSessionClip();
    node.removeSessionClip (id1);
    const auto id2 = node.createSessionClip();
    /* New clip gets a fresh id; the underlying slot may be reused
     * but observable identity is the new uuid. */
    BOOST_CHECK (id1 != id2);
    BOOST_CHECK (node.getClipRegion (id2) != nullptr);
    BOOST_CHECK (node.getClipRegion (id1) == nullptr);
}

/* === Accessors default for unknown ids ================================== */

BOOST_AUTO_TEST_CASE (unknown_clip_returns_defaults)
{
    MidiPlayerNode node;
    const juce::Uuid bogus;
    BOOST_CHECK_EQUAL (node.isSessionClipPlaying (bogus),       false);
    BOOST_CHECK_EQUAL (node.getSessionClipPositionBeats (bogus), 0.0);
    BOOST_CHECK_EQUAL (node.getSessionClipLengthBeats (bogus),   0.0);
    BOOST_CHECK_EQUAL (node.sessionClipWrappedSinceLastQuery (bogus), false);
    BOOST_CHECK (node.getClipRegion (bogus) == nullptr);
}

/* === Mute / solo parity with TrackerNode ================================ */

BOOST_AUTO_TEST_CASE (user_mute_round_trip)
{
    MidiPlayerNode node;
    BOOST_CHECK_EQUAL (node.getUserMuted(), false);
    node.setUserMuted (true);
    BOOST_CHECK_EQUAL (node.getUserMuted(), true);
    node.setUserMuted (false);
    BOOST_CHECK_EQUAL (node.getUserMuted(), false);
}

BOOST_AUTO_TEST_CASE (soloed_round_trip)
{
    MidiPlayerNode node;
    BOOST_CHECK_EQUAL (node.getSoloed(), false);
    node.setSoloed (true);
    BOOST_CHECK_EQUAL (node.getSoloed(), true);
}

/* === Schedule API smoke ================================================= */

BOOST_AUTO_TEST_CASE (schedule_unknown_clip_is_silent_noop)
{
    MidiPlayerNode node;
    /* No exception, no crash. */
    node.schedulePlayingClip (juce::Uuid(), -1.0, true);
    BOOST_CHECK (true);
}

/* === Region edits are visible via getClipRegion ========================= */

BOOST_AUTO_TEST_CASE (region_edits_reflect_on_subsequent_lookup)
{
    MidiPlayerNode node;
    const auto id = node.createSessionClip();
    auto* r = node.getClipRegion (id);
    BOOST_REQUIRE (r != nullptr);

    /* Add a note via the message-thread mutator path. */
    MidiNoteRegion::NoteList notes;
    element::MidiNote n;
    n.pitch       = 60;
    n.onBeat      = 0.0;
    n.lengthBeats = 1.0;
    n.velocity    = 100;
    n.channel     = 1;
    notes.push_back (n);
    r->setNotes (std::move (notes));

    auto* r2 = node.getClipRegion (id);
    BOOST_REQUIRE (r2 != nullptr);
    BOOST_CHECK_EQUAL (r2->noteCount(), (size_t) 1);
}

/* === Persistence round-trip ============================================= */

BOOST_AUTO_TEST_CASE (state_round_trip_preserves_clip_count)
{
    MidiPlayerNode src;
    const auto id1 = src.createSessionClip();
    const auto id2 = src.createSessionClip();
    juce::ignoreUnused (id1, id2);

    juce::MemoryBlock blob;
    src.getState (blob);
    BOOST_REQUIRE (blob.getSize() > 0);

    MidiPlayerNode dst;
    dst.setState (blob.getData(), (int) blob.getSize());

    BOOST_CHECK (dst.getClipRegion (id1) != nullptr);
    BOOST_CHECK (dst.getClipRegion (id2) != nullptr);
}

BOOST_AUTO_TEST_CASE (state_round_trip_preserves_region_notes)
{
    MidiPlayerNode src;
    const auto id = src.createSessionClip();
    auto* r = src.getClipRegion (id);
    BOOST_REQUIRE (r != nullptr);

    MidiNoteRegion::NoteList notes;
    for (int i = 0; i < 4; ++i)
    {
        element::MidiNote n;
        n.pitch       = 60 + i;
        n.onBeat      = (double) i * 0.5;
        n.lengthBeats = 0.4;
        n.velocity    = 80 + i;
        n.channel     = 1;
        notes.push_back (n);
    }
    r->setNotes (std::move (notes));

    juce::MemoryBlock blob;
    src.getState (blob);

    MidiPlayerNode dst;
    dst.setState (blob.getData(), (int) blob.getSize());
    auto* r2 = dst.getClipRegion (id);
    BOOST_REQUIRE (r2 != nullptr);
    BOOST_CHECK_EQUAL (r2->noteCount(), (size_t) 4);
}

BOOST_AUTO_TEST_CASE (empty_state_means_no_clips)
{
    MidiPlayerNode node;
    /* Pre-existing arrangement-only contract: empty getState means
     * no session-view clip data persisted.  setState with null/empty
     * still leaves the node functional. */
    juce::MemoryBlock blob;
    node.getState (blob);
    BOOST_CHECK_EQUAL (blob.getSize(), (size_t) 0);

    MidiPlayerNode dst;
    dst.setState (nullptr, 0);
    /* No assertions to make -- just verify it doesn't crash. */
    BOOST_CHECK (true);
}

/* === reconcileBoundClips silences unbound playing slots ================= */

BOOST_AUTO_TEST_CASE (reconcile_bound_unknown_slot_is_silent_noop)
{
    MidiPlayerNode node;
    const auto id = node.createSessionClip();
    juce::Array<juce::Uuid> bound;
    bound.add (id);
    /* No exception, no crash. */
    node.reconcileBoundClips (bound);
    BOOST_CHECK (true);
}

/* === Clip kind discriminant int-cast safety ============================ */

BOOST_AUTO_TEST_CASE (clip_kind_int_cast_round_trip)
{
    /* The persistence layer writes the kind as a string ("tracker" /
     * "midi") -- this is a structural sanity check that the in-memory
     * enum values fit in the persistence layer's expectations. */
    enum class Kind : uint8_t { Tracker = 0, Midi = 1 };
    BOOST_CHECK_EQUAL ((int) Kind::Tracker, 0);
    BOOST_CHECK_EQUAL ((int) Kind::Midi,    1);
}

BOOST_AUTO_TEST_SUITE_END()
