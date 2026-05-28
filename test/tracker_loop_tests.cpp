// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <boost/test/unit_test.hpp>

#include "nodes/tracker.hpp"

#include <element/juce/core.hpp>

#include <memory>

using element::TrackerNode;

BOOST_AUTO_TEST_SUITE (TrackerLoopTests)

/* getSequenceLengthBeats is the F2.1 enabler -- ArrangementView uses
 * it to compute the cutoff position for non-looped tracker regions
 * (cutoff = min(region.lengthBeats, sequence_length_beats)) and to
 * gate the dashed loop-cue paint for looped tracker regions whose
 * drawn lengthBeats spans more than one sequence iteration. */

BOOST_AUTO_TEST_CASE (length_beats_zero_before_prepare)
{
    /* TrackerNode constructor does not create mod_; that happens in
     * prepareToRender or setState.  Until then every accessor returns
     * the empty-state sentinel. */
    TrackerNode node;
    BOOST_CHECK_EQUAL (node.getSequenceLengthBeats (0), 0.0);
    BOOST_CHECK_EQUAL (node.getSequenceLengthBeats (-1), 0.0);
    BOOST_CHECK_EQUAL (node.getSequenceLengthBeats (1000), 0.0);
}

BOOST_AUTO_TEST_CASE (length_beats_default_pattern_after_prepare)
{
    /* installDefaultPattern creates one sequence with length 16 rows
     * and the vht default rpb of 4 -- so 16 / 4 = 4 beats. */
    TrackerNode node;
    node.prepareToRender (48000.0, 1024);

    BOOST_CHECK_CLOSE (node.getSequenceLengthBeats (0), 4.0, 1e-9);
}

BOOST_AUTO_TEST_CASE (length_beats_out_of_range_returns_zero)
{
    TrackerNode node;
    node.prepareToRender (48000.0, 1024);

    /* Negative + past-end both return the sentinel. */
    BOOST_CHECK_EQUAL (node.getSequenceLengthBeats (-1), 0.0);
    BOOST_CHECK_EQUAL (node.getSequenceLengthBeats (1), 0.0);
    BOOST_CHECK_EQUAL (node.getSequenceLengthBeats (999), 0.0);
}

BOOST_AUTO_TEST_CASE (length_beats_created_sequence_reports_rows_over_rpb)
{
    /* createSequence appends a new sequence with the requested row
     * count.  rpb defaults to 4 (vht's TRACK_DEF_RPB).  Verify the
     * accessor reflects the actual values, not just installDefault's. */
    TrackerNode node;
    node.prepareToRender (48000.0, 1024);

    const int newIdx = node.createSequence (32);   // 32 rows / 4 rpb = 8 beats
    BOOST_REQUIRE_EQUAL (newIdx, 1);
    BOOST_CHECK_CLOSE (node.getSequenceLengthBeats (1), 8.0, 1e-9);

    const int shortIdx = node.createSequence (8);  // 8 rows / 4 rpb = 2 beats
    BOOST_REQUIRE_EQUAL (shortIdx, 2);
    BOOST_CHECK_CLOSE (node.getSequenceLengthBeats (2), 2.0, 1e-9);
}

/* C.1 -- no rogue emit: every sequence starts with playing=0 so the
 * user must explicitly launch via session view or arrangement.
 * Closes the bug where vht's installDefaultPattern set the curr_seq
 * to playing=1, causing MIDI to flow without any user action. */

BOOST_AUTO_TEST_CASE (no_rogue_emit_after_prepare)
{
    TrackerNode node;
    node.prepareToRender (48000.0, 1024);

    /* installDefaultPattern adds one sequence -- it must NOT be
     * playing.  This is the core C.1 invariant. */
    BOOST_REQUIRE_EQUAL (node.numPatterns(), 1);
    BOOST_CHECK (! node.isSequencePlaying (0));
}

BOOST_AUTO_TEST_CASE (no_rogue_emit_after_create_sequence)
{
    TrackerNode node;
    node.prepareToRender (48000.0, 1024);

    const int idx = node.createSequence (16);
    BOOST_REQUIRE_EQUAL (idx, 1);
    BOOST_CHECK (! node.isSequencePlaying (0));
    BOOST_CHECK (! node.isSequencePlaying (1));
}

BOOST_AUTO_TEST_CASE (no_rogue_emit_after_clone_sequence)
{
    TrackerNode node;
    node.prepareToRender (48000.0, 1024);

    const int idx = node.cloneSequence (0);
    BOOST_REQUIRE_EQUAL (idx, 1);
    BOOST_CHECK (! node.isSequencePlaying (0));
    BOOST_CHECK (! node.isSequencePlaying (1));
}

BOOST_AUTO_TEST_CASE (no_rogue_emit_after_setstate_round_trip)
{
    /* Round-trip via getState / setState (used by session save/load).
     * After setState, no sequences should be playing -- previously
     * setState explicitly set curr_seq's playing=1 after zeroing all. */
    TrackerNode src;
    src.prepareToRender (48000.0, 1024);
    src.createSequence (8);
    src.createSequence (32);
    BOOST_REQUIRE_EQUAL (src.numPatterns(), 3);

    juce::MemoryBlock blob;
    src.getState (blob);

    TrackerNode restored;
    restored.prepareToRender (48000.0, 1024);
    restored.setState (blob.getData(), (int) blob.getSize());
    BOOST_REQUIRE_EQUAL (restored.numPatterns(), 3);

    BOOST_CHECK (! restored.isSequencePlaying (0));
    BOOST_CHECK (! restored.isSequencePlaying (1));
    BOOST_CHECK (! restored.isSequencePlaying (2));
}

BOOST_AUTO_TEST_CASE (setSequencePlaying_drives_engine_explicitly)
{
    /* User-initiated launch flow: SessionView calls schedulePlaying;
     * the audio thread applies it via applyPendingForBlock which
     * calls sequence_set_playing.  Verify the basic primitive
     * setSequencePlaying still toggles the flag (the session-view
     * launch path depends on this). */
    TrackerNode node;
    node.prepareToRender (48000.0, 1024);

    BOOST_CHECK (! node.isSequencePlaying (0));
    node.setSequencePlaying (0, true);
    BOOST_CHECK (node.isSequencePlaying (0));
    node.setSequencePlaying (0, false);
    BOOST_CHECK (! node.isSequencePlaying (0));
}

BOOST_AUTO_TEST_SUITE_END()
