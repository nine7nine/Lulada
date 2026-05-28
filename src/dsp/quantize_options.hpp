// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <vector>

namespace element::dsp::quantize {

/** Grid division for quantize / humanize / scale operations.  Mirrors
 *  the snap-division enum the toolbar uses (PianoRollGrid::snapDivision_
 *  is a plain `double` today but logically picks one of these values).
 *  The numeric mapping is (NoteLength)::Quarter = 1 beat. */
enum class NoteLength
{
    Whole,         /* 4 beats     */
    Half,          /* 2 beats     */
    Quarter,       /* 1 beat      */
    Eighth,        /* 0.5 beats   */
    Sixteenth,     /* 0.25 beats  */
    ThirtySecond,  /* 0.125 beats */
    SixtyFourth    /* 0.0625      */
};

/** Modifier on a NoteLength.  Triplet = 2/3 of straight; dotted = 3/2.
 *  Matches Zrythm + Ardour + Bitwig.  (External-project name not used
 *  in commit text per project rules; mentioned here only to document
 *  industry convention.) */
enum class NoteType
{
    Normal,
    Dotted,
    Triplet
};

/** Convert (NoteLength, NoteType) to a duration in beats.  Quarter ==
 *  1.0; Eighth == 0.5; dotted multiplies by 1.5; triplet multiplies by
 *  2/3.  Used by both the snap-point builder below and the swing-shift
 *  math. */
double divisionBeats (NoteLength, NoteType) noexcept;

/** Parameters for bulk quantize on a selection of notes.
 *
 *  `amount` is a 0..1 mix between original onset (0) and grid-locked
 *  onset (1).  At 0.5 the note moves halfway toward the nearest grid
 *  point -- musicians call this "loose quantize".
 *
 *  `adjustStart` and `adjustEnd` independently gate which edge of the
 *  note is snapped.  The common configuration is start-only (default):
 *  realign the attack but leave the release where the player put it.
 *
 *  `swing` shifts every OTHER quantize point later by
 *  `swing * (divisionBeats / 2)`.  0 = straight, 1 = full triplet feel.
 *  Industry-standard formulation.
 *
 *  `randomBeats` is a +/- jitter applied to the final onset after the
 *  amount-mix.  0 = pure quantize.  Used by the dialog's Humanize tab
 *  to add micro-timing variation.  Combined with `seed` for
 *  deterministic undo -- two identical calls with the same seed
 *  produce identical results, so Ctrl+Z + Ctrl+Y round-trips cleanly.
 *  `seed == 0` means "derive a stable seed from the selection set",
 *  which gives undo-stability without forcing a UI control on the
 *  seed itself. */
struct QuantizeOptions
{
    NoteLength    noteLength { NoteLength::Sixteenth };
    NoteType      noteType   { NoteType::Normal };
    double        amount     { 1.0 };
    bool          adjustStart { true };
    bool          adjustEnd   { false };
    double        swing       { 0.0 };
    double        randomBeats { 0.0 };
    std::uint64_t seed        { 0 };
};

/** Pre-compute the snap-point list for `options` across [0, lengthBeats].
 *  Points are sorted ascending.  Includes 0 and the final point at or
 *  just past lengthBeats so a note near the region tail can still snap
 *  to a valid point.  When `swing > 0`, odd-index points are shifted
 *  later by `swing * (divisionBeats / 2)` per the documented formula.
 *
 *  Result is meant to be cached across all notes in a single quantize
 *  invocation -- per-note re-build would be O(N*P), pre-build is O(P). */
std::vector<double> buildQuantizePoints (const QuantizeOptions& options,
                                          double lengthBeats);

/** Find the snap point in `points` closest to `value`.  `points` must
 *  be sorted ascending (buildQuantizePoints guarantees that).  Returns
 *  the value unchanged when `points` is empty. */
double nearestPoint (double value, const std::vector<double>& points) noexcept;

/** Snap a beat value toward `points`, mixed with `amount`.  Equivalent
 *  to `value + (nearestPoint(value, points) - value) * amount`. */
double snapBeatMixed (double value,
                      const std::vector<double>& points,
                      double amount) noexcept;

//==========================================================================
// Velocity humanize -- separate parameter object so the ops engine
// doesn't conflate timing-jitter with velocity-jitter.

/** Per-call parameters for humanizeVelocity.  Deterministic with
 *  fixed seed; seed = 0 derives from the selection set so undo is
 *  stable without forcing a UI control. */
struct HumanizeOptions
{
    int           velocityRange { 10 };   /* +/- amount, 0..127 */
    int           velocityBias  { 0 };    /* shift the centre by this much */
    std::uint64_t seed          { 0 };
};

//==========================================================================
// Scale quantize -- pitch-side analogue of timing quantize.  Snaps each
// note's pitch to the nearest in-scale tone for the given root.

enum class Scale
{
    Major,
    NaturalMinor,
    HarmonicMinor,
    Dorian,
    Phrygian,
    Lydian,
    Mixolydian,
    Locrian,
    MajorPentatonic,
    MinorPentatonic,
    Chromatic,
    WholeTone
};

/** Returns the seven (or fewer) interval offsets (0..11) that make up
 *  `scale`.  Used by scaleQuantize but exposed for tests + UI listing. */
const std::vector<int>& scaleIntervals (Scale) noexcept;

/** Snap a single pitch (0..127) to the nearest in-scale pitch given
 *  the scale + root note (0..11 with 0 = C).  Octave is preserved; if
 *  two in-scale pitches are equidistant the LOWER is preferred so the
 *  result is deterministic. */
int snapPitchToScale (int pitch, Scale scale, int rootSemitone) noexcept;

} // namespace element::dsp::quantize
