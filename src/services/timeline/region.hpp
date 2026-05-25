// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/core.hpp>
#include <element/juce/data_structures.hpp>
#include <element/juce/graphics.hpp>

#include <vector>

namespace element {

/** Per-segment interpolation shape for envelope breakpoints.  Curve
 *  applies from THIS point to the NEXT point.  The last point's
 *  curve is unused (no segment past it).
 *
 *  Linear      -- straight line between gainDbs.
 *  Exponential -- gainDb^2 weighting; slow start / fast finish.
 *  Smooth      -- cosine-based S-curve; symmetric ease in/out.
 *  Hold        -- step (no interpolation); next point's gain steps
 *                 in instantly when its beat is reached. */
enum class EnvelopeCurve : int { Linear = 0, Exponential = 1, Smooth = 2, Hold = 3 };

/** Single breakpoint on a clip envelope.
 *
 *  beatOffset is measured from the region's local start (0 ==
 *  positionBeats).  gainDb in dB (typical range -60..+12; the audio
 *  thread clamps to a safe range before applying).  curve applies
 *  to the segment between this point and the next-by-beatOffset. */
struct EnvelopePoint
{
    juce::Uuid    id;
    double        beatOffset { 0.0 };
    float         gainDb     { 0.0f };
    EnvelopeCurve curve      { EnvelopeCurve::Linear };
    /** 2D control-point Bezier handle for the segment between THIS
     *  point and the next-by-beatOffset.
     *
     *  curveOffsetT  -- normalised X within the segment in [0.25, 0.75]
     *                   (clamped at the UI layer; outside that range
     *                   the quadratic Bezier's x(u) is non-monotonic
     *                   and the curve loops back on itself).
     *  curveOffsetDb -- dB shift from the chord midpoint.  0 = handle
     *                   sits on the straight chord, no bend.  Positive
     *                   raises the handle above the chord, negative
     *                   lowers it.
     *
     *  Defaults (0.5, 0) = handle at geometric midpoint on the chord
     *  = linear.  Non-default = curve bent into a quadratic Bezier
     *  passing through (curveOffsetT, chordMidDb + curveOffsetDb) at
     *  Bezier-parameter u=0.5.  Hold ignores both (step regardless).
     *
     *  Sparse-write: stored as `cot` / `cod` XML attrs; only emitted
     *  when non-default. */
    float         curveOffsetT  { 0.5f };
    float         curveOffsetDb { 0.0f };
};

/** Pure-data Region.  A thin metadata wrapper pointing at a Source by
 *  uuid with beat-domain placement and per-region cosmetics.
 *
 *  Beat-domain coordinates throughout: positionBeats / startBeats /
 *  lengthBeats / fadeInBeats / fadeOutBeats are all in beats relative
 *  to the session transport's tempo map.  Sample-accurate scheduling
 *  happens at the audio thread (TimelineScheduler -> adapter ->
 *  per-node FIFO).
 *
 *  startBeats is the offset INTO the Source where playback begins.
 *  For VhtSequenceSource regions, v1 truncates startBeats to 0
 *  (sequence playback always starts at row 0).  For AudioFileSource
 *  regions, startBeats lets the user trim into the audio file.
 *
 *  Thread safety: Region is value-typed.  Lives owned by Playlist
 *  (message thread).  Audio thread only sees copies passed via FIFO
 *  entries; never reads the source Region struct directly.
 *
 *  See timeline-audio-design.md Section 1.3.
 */
struct Region
{
    juce::Uuid    id;
    juce::Uuid    sourceId;          // -> SourceRegistry lookup
    /** For MIDI (vht) regions: index of the sequence WITHIN the owning
     *  TrackerNode (sourceId = the TrackerNode's uuid).  For audio
     *  regions: unused (-1).  Lets us express "this region plays
     *  TrackerX's pattern N" without a Source-per-pattern entry in
     *  the registry -- the data already lives inside the
     *  TrackerNode's vht module.  Sparse-write: only persisted when
     *  != -1. */
    int           sequenceIdx   { -1 };
    double        positionBeats { 0.0 };
    double        startBeats    { 0.0 };
    double        lengthBeats   { 0.0 };
    double        gainDb        { 0.0 };
    double        fadeInBeats   { 0.0 };
    double        fadeOutBeats  { 0.0 };
    /** Fade curvature in [-1, +1].  0 = linear (current default).
     *  Positive  = concave-up  (slow start, fast finish; "exponential").
     *  Negative  = concave-down (fast start, slow finish; "logarithmic").
     *  Mapped to a power-curve exponent at evaluation time via
     *  p = exp2(curve * 2.0), so c==+1 -> p==4, c==-1 -> p==0.25.
     *  Sparse-write: only persisted when != 0.0. */
    float         fadeInCurve   { 0.0f };
    float         fadeOutCurve  { 0.0f };
    bool          looped        { false };
    juce::Colour  colour        { 0xff'4a'7a'b5 };
    juce::String  name;

    /** Volume envelope -- ordered by beatOffset ascending after
     *  every mutation.  Empty == use static gainDb as constant gain
     *  (existing behaviour preserved).  Two or more points enable
     *  per-sample interpolated gain.  Points outside [0, lengthBeats]
     *  are clamped on paint + evaluation. */
    std::vector<EnvelopePoint> volumeEnvelope;

    /** Sample the envelope's gain (dB) at a beat-offset inside the
     *  region.  Falls back to `gainDb` when the envelope is empty.
     *  Out-of-range offsets extrapolate as the nearest endpoint
     *  (no wrap).  Safe to call from the audio thread; pure on the
     *  Region copy held by the audio FIFO entry. */
    float gainAtBeatOffset (double localBeat) const noexcept;

    /** Sort envelope points by beatOffset.  Call after any mutation
     *  that could break the invariant (insert / drag past neighbour). */
    void sortEnvelope() noexcept;

    /** End position on the timeline (positionBeats + lengthBeats).
     *  Pure -- safe from any thread. */
    double endBeats() const noexcept { return positionBeats + lengthBeats; }

    /** Returns true if the given timeline beat lies inside the region's
     *  span (start inclusive, end exclusive).  Pure. */
    bool containsBeat (double beat) const noexcept
    {
        return beat >= positionBeats && beat < endBeats();
    }

    /** Sparse-write ValueTree serialiser.  Only writes properties that
     *  differ from defaults to keep saved XML compact and forward-
     *  compatible: future Region fields read as their default when
     *  loaded from older saves. */
    juce::ValueTree toValueTree() const;

    /** Inverse of toValueTree.  Missing properties land as defaults
     *  (sparse-read).  Returns a default Region on invalid input. */
    static Region fromValueTree (const juce::ValueTree&);
};

} // namespace element
