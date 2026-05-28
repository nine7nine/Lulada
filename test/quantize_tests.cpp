// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <boost/test/unit_test.hpp>

#include "dsp/quantize_options.hpp"
#include "dsp/quantize_ops.hpp"
#include "services/timeline/midi_note.hpp"
#include "services/timeline/midi_note_region.hpp"
#include "ui/pianoroll/midi_note_diff_command.hpp"

#include <element/juce/core.hpp>

#include <cmath>
#include <unordered_set>
#include <vector>

using element::MidiNote;
using element::MidiNoteRegion;
using element::MidiNoteDiffCommand;

namespace q = element::dsp::quantize;

namespace {

inline bool nearly (double a, double b, double tol = 1e-9)
{
    return std::abs (a - b) <= tol;
}

MidiNote makeNote (int pitch, double onBeat,
                    double lengthBeats = 0.25,
                    int velocity = 100, int channel = 1)
{
    MidiNote n;
    n.pitch       = pitch;
    n.onBeat      = onBeat;
    n.lengthBeats = lengthBeats;
    n.velocity    = velocity;
    n.channel     = channel;
    return n;
}

/* Stable resolver that just returns the region pointer for any uuid.
 * Tests don't need uuid-keyed multi-region resolution. */
MidiNoteDiffCommand::RegionResolver resolverFor (MidiNoteRegion& r)
{
    return [&r] (const juce::Uuid&) { return &r; };
}

std::unordered_set<std::uint64_t> selectionOfAll (const MidiNoteRegion& r)
{
    std::unordered_set<std::uint64_t> ids;
    if (const auto* snap = r.loadSnapshot())
        for (const auto& n : *snap) ids.insert (n.id);
    return ids;
}

} // namespace

BOOST_AUTO_TEST_SUITE (QuantizeTests)

/* ---------- divisionBeats ---------- */

BOOST_AUTO_TEST_CASE (division_beats_matches_standard_table)
{
    BOOST_CHECK (nearly (q::divisionBeats (q::NoteLength::Quarter,      q::NoteType::Normal),  1.0));
    BOOST_CHECK (nearly (q::divisionBeats (q::NoteLength::Eighth,       q::NoteType::Normal),  0.5));
    BOOST_CHECK (nearly (q::divisionBeats (q::NoteLength::Sixteenth,    q::NoteType::Normal),  0.25));
    BOOST_CHECK (nearly (q::divisionBeats (q::NoteLength::ThirtySecond, q::NoteType::Normal),  0.125));
    BOOST_CHECK (nearly (q::divisionBeats (q::NoteLength::Quarter,      q::NoteType::Dotted),  1.5));
    BOOST_CHECK (nearly (q::divisionBeats (q::NoteLength::Quarter,      q::NoteType::Triplet), 2.0 / 3.0));
    BOOST_CHECK (nearly (q::divisionBeats (q::NoteLength::Eighth,       q::NoteType::Triplet), 1.0 / 3.0));
}

/* ---------- buildQuantizePoints ---------- */

BOOST_AUTO_TEST_CASE (build_points_covers_region_at_quarter_division)
{
    q::QuantizeOptions opts;
    opts.noteLength = q::NoteLength::Quarter;
    opts.noteType   = q::NoteType::Normal;
    opts.swing      = 0.0;

    const auto pts = q::buildQuantizePoints (opts, 4.0);
    BOOST_REQUIRE_GE (pts.size(), (size_t) 5);   /* 0, 1, 2, 3, 4 */
    BOOST_CHECK (nearly (pts[0], 0.0));
    BOOST_CHECK (nearly (pts[1], 1.0));
    BOOST_CHECK (nearly (pts[2], 2.0));
    BOOST_CHECK (nearly (pts[3], 3.0));
    BOOST_CHECK (nearly (pts[4], 4.0));
}

BOOST_AUTO_TEST_CASE (swing_shifts_odd_indexed_points)
{
    q::QuantizeOptions opts;
    opts.noteLength = q::NoteLength::Eighth;     /* D = 0.5 */
    opts.swing      = 1.0;                        /* full triplet feel */
    /* swingShift = 1.0 * (0.5 / 2.0) = 0.25 */

    const auto pts = q::buildQuantizePoints (opts, 2.0);
    /* Even indices stay on the grid; odd indices shift +0.25. */
    BOOST_REQUIRE_GE (pts.size(), (size_t) 5);
    BOOST_CHECK (nearly (pts[0], 0.0));
    BOOST_CHECK (nearly (pts[1], 0.75));
    BOOST_CHECK (nearly (pts[2], 1.0));
    BOOST_CHECK (nearly (pts[3], 1.75));
    BOOST_CHECK (nearly (pts[4], 2.0));
}

BOOST_AUTO_TEST_CASE (build_points_empty_for_zero_length)
{
    q::QuantizeOptions opts;
    const auto pts = q::buildQuantizePoints (opts, 0.0);
    BOOST_CHECK (pts.empty());
}

/* ---------- nearestPoint + snapBeatMixed ---------- */

BOOST_AUTO_TEST_CASE (nearest_point_picks_closest)
{
    std::vector<double> pts { 0.0, 1.0, 2.0, 3.0 };
    BOOST_CHECK (nearly (q::nearestPoint (0.4,  pts), 0.0));
    BOOST_CHECK (nearly (q::nearestPoint (0.6,  pts), 1.0));
    BOOST_CHECK (nearly (q::nearestPoint (2.49, pts), 2.0));
    BOOST_CHECK (nearly (q::nearestPoint (2.51, pts), 3.0));
}

BOOST_AUTO_TEST_CASE (snap_beat_mixed_interpolates_with_amount)
{
    std::vector<double> pts { 0.0, 1.0, 2.0 };
    /* Note at 0.6, nearest = 1.0, amount = 0.5 -> midpoint = 0.8. */
    BOOST_CHECK (nearly (q::snapBeatMixed (0.6, pts, 0.5), 0.8));
    BOOST_CHECK (nearly (q::snapBeatMixed (0.6, pts, 0.0), 0.6));
    BOOST_CHECK (nearly (q::snapBeatMixed (0.6, pts, 1.0), 1.0));
}

/* ---------- quantizeNotes ---------- */

BOOST_AUTO_TEST_CASE (quantize_snaps_off_grid_notes_to_grid_at_full_amount)
{
    MidiNoteRegion r;
    r.lengthBeats = 4.0;
    r.setNotesAssigningIds ({
        makeNote (60, 0.10),   /* near 0.0 */
        makeNote (62, 0.55),   /* near 0.5 (1/8 grid) */
        makeNote (64, 1.04),   /* near 1.0 */
        makeNote (66, 2.97)    /* near 3.0 */
    });

    q::QuantizeOptions opts;
    opts.noteLength  = q::NoteLength::Eighth;   /* 0.5 beat grid */
    opts.amount      = 1.0;
    opts.adjustStart = true;
    opts.adjustEnd   = false;

    MidiNoteDiffCommand cmd (juce::Uuid(), resolverFor (r));
    const auto touched = q::quantizeNotes (r, selectionOfAll (r), opts, cmd);
    BOOST_CHECK_EQUAL (touched, (size_t) 4);
    BOOST_REQUIRE (cmd.perform());

    const auto* snap = r.loadSnapshot();
    BOOST_REQUIRE (snap != nullptr);
    BOOST_CHECK (nearly ((*snap)[0].onBeat, 0.0));
    BOOST_CHECK (nearly ((*snap)[1].onBeat, 0.5));
    BOOST_CHECK (nearly ((*snap)[2].onBeat, 1.0));
    BOOST_CHECK (nearly ((*snap)[3].onBeat, 3.0));
}

BOOST_AUTO_TEST_CASE (quantize_preserves_length_when_adjust_end_false)
{
    MidiNoteRegion r;
    r.lengthBeats = 4.0;
    r.setNotesAssigningIds ({ makeNote (60, 0.13, 1.0) });   /* length = 1 beat */

    q::QuantizeOptions opts;
    opts.noteLength  = q::NoteLength::Quarter;
    opts.adjustStart = true;
    opts.adjustEnd   = false;
    opts.amount      = 1.0;

    MidiNoteDiffCommand cmd (juce::Uuid(), resolverFor (r));
    q::quantizeNotes (r, selectionOfAll (r), opts, cmd);
    cmd.perform();

    const auto* snap = r.loadSnapshot();
    BOOST_REQUIRE_EQUAL (snap->size(), (size_t) 1);
    BOOST_CHECK (nearly ((*snap)[0].onBeat,      0.0));
    BOOST_CHECK (nearly ((*snap)[0].lengthBeats, 1.0));
}

BOOST_AUTO_TEST_CASE (quantize_amount_zero_is_identity)
{
    MidiNoteRegion r;
    r.lengthBeats = 2.0;
    r.setNotesAssigningIds ({ makeNote (60, 0.37) });

    q::QuantizeOptions opts;
    opts.noteLength = q::NoteLength::Quarter;
    opts.amount     = 0.0;
    opts.randomBeats = 0.0;

    MidiNoteDiffCommand cmd (juce::Uuid(), resolverFor (r));
    const auto touched = q::quantizeNotes (r, selectionOfAll (r), opts, cmd);
    /* amount=0 with no jitter -> no change -> nothing recorded. */
    BOOST_CHECK_EQUAL (touched, (size_t) 0);
}

BOOST_AUTO_TEST_CASE (quantize_amount_half_moves_halfway_to_grid)
{
    MidiNoteRegion r;
    r.lengthBeats = 2.0;
    r.setNotesAssigningIds ({ makeNote (60, 0.40) });

    q::QuantizeOptions opts;
    opts.noteLength = q::NoteLength::Quarter;   /* nearest grid = 0.0 */
    opts.amount     = 0.5;

    MidiNoteDiffCommand cmd (juce::Uuid(), resolverFor (r));
    q::quantizeNotes (r, selectionOfAll (r), opts, cmd);
    cmd.perform();

    const auto* snap = r.loadSnapshot();
    /* 0.4 -> midpoint to 0.0 -> 0.2. */
    BOOST_CHECK (nearly ((*snap)[0].onBeat, 0.2));
}

BOOST_AUTO_TEST_CASE (quantize_clamps_to_region_tail)
{
    MidiNoteRegion r;
    r.lengthBeats = 4.0;
    /* Note near right edge with significant length -- snapping its
     * onset to 4.0 (the nearest quarter grid point) would push the
     * tail past lengthBeats; quantize must clamp. */
    r.setNotesAssigningIds ({ makeNote (60, 3.90, 1.0) });

    q::QuantizeOptions opts;
    opts.noteLength = q::NoteLength::Quarter;
    opts.amount     = 1.0;

    MidiNoteDiffCommand cmd (juce::Uuid(), resolverFor (r));
    q::quantizeNotes (r, selectionOfAll (r), opts, cmd);
    cmd.perform();

    const auto* snap = r.loadSnapshot();
    BOOST_REQUIRE_EQUAL (snap->size(), (size_t) 1);
    BOOST_CHECK ((*snap)[0].onBeat + (*snap)[0].lengthBeats <= 4.0 + 1e-9);
}

BOOST_AUTO_TEST_CASE (quantize_skips_notes_outside_selection)
{
    MidiNoteRegion r;
    r.lengthBeats = 4.0;
    const auto id1 = r.addNote (makeNote (60, 0.10));
    const auto id2 = r.addNote (makeNote (62, 1.10));
    (void) id2;

    q::QuantizeOptions opts;
    opts.noteLength = q::NoteLength::Quarter;
    opts.amount     = 1.0;

    std::unordered_set<std::uint64_t> sel { id1 };   /* only first note */

    MidiNoteDiffCommand cmd (juce::Uuid(), resolverFor (r));
    const auto touched = q::quantizeNotes (r, sel, opts, cmd);
    BOOST_CHECK_EQUAL (touched, (size_t) 1);
    cmd.perform();

    const auto* snap = r.loadSnapshot();
    BOOST_REQUIRE_EQUAL (snap->size(), (size_t) 2);
    /* id1 snapped to 0.0; id2 untouched at 1.10. */
    for (const auto& n : *snap)
    {
        if (n.id == id1)
            BOOST_CHECK (nearly (n.onBeat, 0.0));
        else
            BOOST_CHECK (nearly (n.onBeat, 1.10));
    }
}

BOOST_AUTO_TEST_CASE (quantize_undo_round_trips)
{
    MidiNoteRegion r;
    r.lengthBeats = 4.0;
    r.setNotesAssigningIds ({
        makeNote (60, 0.13),
        makeNote (62, 1.27),
        makeNote (64, 2.91)
    });
    std::vector<double> originalOnsets;
    for (const auto& n : *r.loadSnapshot())
        originalOnsets.push_back (n.onBeat);

    q::QuantizeOptions opts;
    opts.noteLength = q::NoteLength::Quarter;
    opts.amount     = 1.0;

    MidiNoteDiffCommand cmd (juce::Uuid(), resolverFor (r));
    q::quantizeNotes (r, selectionOfAll (r), opts, cmd);
    cmd.perform();
    cmd.undo();

    const auto* snap = r.loadSnapshot();
    BOOST_REQUIRE_EQUAL (snap->size(), originalOnsets.size());
    for (std::size_t i = 0; i < snap->size(); ++i)
        BOOST_CHECK (nearly ((*snap)[i].onBeat, originalOnsets[i]));
}

/* ---------- humanizeVelocity ---------- */

BOOST_AUTO_TEST_CASE (humanize_velocity_is_deterministic_for_fixed_seed)
{
    MidiNoteRegion r;
    r.lengthBeats = 4.0;
    r.setNotesAssigningIds ({
        makeNote (60, 0.0, 0.25, 80),
        makeNote (62, 1.0, 0.25, 80),
        makeNote (64, 2.0, 0.25, 80)
    });

    q::HumanizeOptions opts;
    opts.velocityRange = 20;
    opts.seed          = 12345;

    MidiNoteDiffCommand cmd1 (juce::Uuid(), resolverFor (r));
    q::humanizeVelocity (r, selectionOfAll (r), opts, cmd1);
    cmd1.perform();

    std::vector<int> after1;
    for (const auto& n : *r.loadSnapshot()) after1.push_back (n.velocity);

    /* Undo, run again with same seed, expect identical result. */
    cmd1.undo();
    MidiNoteDiffCommand cmd2 (juce::Uuid(), resolverFor (r));
    q::humanizeVelocity (r, selectionOfAll (r), opts, cmd2);
    cmd2.perform();

    std::vector<int> after2;
    for (const auto& n : *r.loadSnapshot()) after2.push_back (n.velocity);

    BOOST_REQUIRE_EQUAL (after1.size(), after2.size());
    for (std::size_t i = 0; i < after1.size(); ++i)
        BOOST_CHECK_EQUAL (after1[i], after2[i]);
}

BOOST_AUTO_TEST_CASE (humanize_velocity_clamps_to_1_127)
{
    MidiNoteRegion r;
    r.lengthBeats = 4.0;
    r.setNotesAssigningIds ({
        makeNote (60, 0.0, 0.25,   2),   /* near floor */
        makeNote (62, 1.0, 0.25, 126)    /* near ceiling */
    });

    q::HumanizeOptions opts;
    opts.velocityRange = 100;            /* large enough to blow past either edge */
    opts.seed          = 999;

    MidiNoteDiffCommand cmd (juce::Uuid(), resolverFor (r));
    q::humanizeVelocity (r, selectionOfAll (r), opts, cmd);
    cmd.perform();

    for (const auto& n : *r.loadSnapshot())
    {
        BOOST_CHECK_GE (n.velocity, 1);
        BOOST_CHECK_LE (n.velocity, 127);
    }
}

/* ---------- scaleQuantize ---------- */

BOOST_AUTO_TEST_CASE (scale_quantize_c_major_passes_white_keys_through)
{
    MidiNoteRegion r;
    r.lengthBeats = 4.0;
    /* All white keys in the C-major octave starting at C4 (60). */
    r.setNotesAssigningIds ({
        makeNote (60, 0.0),  /* C */
        makeNote (62, 0.25), /* D */
        makeNote (64, 0.5),  /* E */
        makeNote (65, 0.75), /* F */
        makeNote (67, 1.0),  /* G */
        makeNote (69, 1.25), /* A */
        makeNote (71, 1.5)   /* B */
    });

    MidiNoteDiffCommand cmd (juce::Uuid(), resolverFor (r));
    const auto touched = q::scaleQuantize (r, selectionOfAll (r),
                                            q::Scale::Major, 0 /* root C */, cmd);
    BOOST_CHECK_EQUAL (touched, (size_t) 0);   /* all already in scale */
}

BOOST_AUTO_TEST_CASE (scale_quantize_c_major_snaps_black_keys_to_neighbours)
{
    MidiNoteRegion r;
    r.lengthBeats = 4.0;
    /* C#4 (61) should snap to either C (60) or D (62) -- our tie-break
     * favours the LOWER pitch, so the result is C (60). */
    r.setNotesAssigningIds ({ makeNote (61, 0.0) });

    MidiNoteDiffCommand cmd (juce::Uuid(), resolverFor (r));
    q::scaleQuantize (r, selectionOfAll (r), q::Scale::Major, 0, cmd);
    cmd.perform();

    const auto* snap = r.loadSnapshot();
    BOOST_REQUIRE_EQUAL (snap->size(), (size_t) 1);
    BOOST_CHECK_EQUAL ((*snap)[0].pitch, 60);
}

BOOST_AUTO_TEST_CASE (scale_quantize_chromatic_is_identity)
{
    MidiNoteRegion r;
    r.lengthBeats = 4.0;
    /* Every pitch is in the chromatic scale; the op should record no
     * updates. */
    for (int p = 48; p <= 84; ++p)
        r.addNote (makeNote (p, 0.0));

    MidiNoteDiffCommand cmd (juce::Uuid(), resolverFor (r));
    const auto touched = q::scaleQuantize (r, selectionOfAll (r),
                                            q::Scale::Chromatic, 0, cmd);
    BOOST_CHECK_EQUAL (touched, (size_t) 0);
}

BOOST_AUTO_TEST_CASE (scale_quantize_octave_root_shift_works)
{
    /* G major scale: G A B C D E F# (intervals 0,2,4,5,7,9,11 from G).
     * F natural (65) is NOT in G-major (F# is); snap should move it to
     * either E (64) or F# (66).  Tie-break prefers lower, so 64. */
    MidiNoteRegion r;
    r.lengthBeats = 4.0;
    r.setNotesAssigningIds ({ makeNote (65, 0.0) });

    MidiNoteDiffCommand cmd (juce::Uuid(), resolverFor (r));
    q::scaleQuantize (r, selectionOfAll (r), q::Scale::Major, 7 /* root G */, cmd);
    cmd.perform();

    const auto* snap = r.loadSnapshot();
    BOOST_REQUIRE_EQUAL (snap->size(), (size_t) 1);
    /* The snapped pitch must be 64 (E) or 66 (F#); tie-break picks lower. */
    BOOST_CHECK_EQUAL ((*snap)[0].pitch, 64);
}

/* ---------- Empty selection / region edge cases ---------- */

BOOST_AUTO_TEST_CASE (quantize_with_empty_selection_is_noop)
{
    MidiNoteRegion r;
    r.lengthBeats = 4.0;
    r.setNotesAssigningIds ({ makeNote (60, 0.13) });

    q::QuantizeOptions opts;
    MidiNoteDiffCommand cmd (juce::Uuid(), resolverFor (r));
    const auto touched
        = q::quantizeNotes (r, std::unordered_set<std::uint64_t>{}, opts, cmd);
    BOOST_CHECK_EQUAL (touched, (size_t) 0);
}

BOOST_AUTO_TEST_CASE (quantize_with_zero_length_region_is_noop)
{
    MidiNoteRegion r;
    r.lengthBeats = 0.0;   /* degenerate -- no grid */
    q::QuantizeOptions opts;
    MidiNoteDiffCommand cmd (juce::Uuid(), resolverFor (r));
    const auto touched
        = q::quantizeNotes (r, std::unordered_set<std::uint64_t>{ 1, 2 }, opts, cmd);
    BOOST_CHECK_EQUAL (touched, (size_t) 0);
}

BOOST_AUTO_TEST_SUITE_END()
