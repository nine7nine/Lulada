// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dsp/quantize_options.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace element::dsp::quantize {

namespace {

constexpr double kEps = 1.0e-9;

double straightBeats (NoteLength len) noexcept
{
    switch (len)
    {
        case NoteLength::Whole:        return 4.0;
        case NoteLength::Half:         return 2.0;
        case NoteLength::Quarter:      return 1.0;
        case NoteLength::Eighth:       return 0.5;
        case NoteLength::Sixteenth:    return 0.25;
        case NoteLength::ThirtySecond: return 0.125;
        case NoteLength::SixtyFourth:  return 0.0625;
    }
    return 0.25;
}

} // namespace

double divisionBeats (NoteLength len, NoteType type) noexcept
{
    const double base = straightBeats (len);
    switch (type)
    {
        case NoteType::Normal:  return base;
        case NoteType::Dotted:  return base * 1.5;
        case NoteType::Triplet: return base * (2.0 / 3.0);
    }
    return base;
}

std::vector<double> buildQuantizePoints (const QuantizeOptions& options,
                                          double lengthBeats)
{
    std::vector<double> points;
    const double D = divisionBeats (options.noteLength, options.noteType);
    if (D <= 0.0 || lengthBeats <= 0.0)
        return points;

    /* Swing shifts the SECOND of each pair of straight grid points later
     * by up to half a division.  At swing = 1.0 the off-grid point lands
     * 2/3 of the way through the pair -- the triplet-feel limit. */
    const double swing      = std::clamp (options.swing, 0.0, 1.0);
    const double swingShift = swing * (D * 0.5);

    /* Reserve a little headroom past lengthBeats so a note whose onset
     * sits at the very tail of the region can still snap to the grid
     * point at or just past the end. */
    const std::size_t reserve = static_cast<std::size_t> ((lengthBeats / D) + 2.0);
    points.reserve (reserve);

    for (int i = 0; ; ++i)
    {
        double p = D * i;
        if ((i & 1) == 1)
            p += swingShift;
        if (p > lengthBeats + D)
            break;
        points.push_back (p);
    }
    return points;
}

double nearestPoint (double value, const std::vector<double>& points) noexcept
{
    if (points.empty())
        return value;
    /* Binary search for the first point >= value; the nearest is then
     * whichever of [lb-1, lb] minimises |value - point|. */
    auto it = std::lower_bound (points.begin(), points.end(), value);
    if (it == points.begin())
        return points.front();
    if (it == points.end())
        return points.back();
    const double hi = *it;
    const double lo = *(it - 1);
    return (std::abs (value - lo) <= std::abs (value - hi)) ? lo : hi;
}

double snapBeatMixed (double value,
                      const std::vector<double>& points,
                      double amount) noexcept
{
    const double mix    = std::clamp (amount, 0.0, 1.0);
    const double target = nearestPoint (value, points);
    return value + (target - value) * mix;
}

//==========================================================================

namespace {

/* Scale interval tables.  Indices are semitone offsets from the root,
 * inclusive of 0.  Built once at first call and returned by const-ref
 * so callers can iterate without copying. */
const std::vector<int>& majorIntervals()           { static const std::vector<int> v {0,2,4,5,7,9,11};  return v; }
const std::vector<int>& naturalMinorIntervals()    { static const std::vector<int> v {0,2,3,5,7,8,10};  return v; }
const std::vector<int>& harmonicMinorIntervals()   { static const std::vector<int> v {0,2,3,5,7,8,11};  return v; }
const std::vector<int>& dorianIntervals()          { static const std::vector<int> v {0,2,3,5,7,9,10};  return v; }
const std::vector<int>& phrygianIntervals()        { static const std::vector<int> v {0,1,3,5,7,8,10};  return v; }
const std::vector<int>& lydianIntervals()          { static const std::vector<int> v {0,2,4,6,7,9,11};  return v; }
const std::vector<int>& mixolydianIntervals()      { static const std::vector<int> v {0,2,4,5,7,9,10};  return v; }
const std::vector<int>& locrianIntervals()         { static const std::vector<int> v {0,1,3,5,6,8,10};  return v; }
const std::vector<int>& majorPentaIntervals()      { static const std::vector<int> v {0,2,4,7,9};       return v; }
const std::vector<int>& minorPentaIntervals()      { static const std::vector<int> v {0,3,5,7,10};      return v; }
const std::vector<int>& chromaticIntervals()       { static const std::vector<int> v {0,1,2,3,4,5,6,7,8,9,10,11}; return v; }
const std::vector<int>& wholeToneIntervals()       { static const std::vector<int> v {0,2,4,6,8,10};    return v; }

} // namespace

const std::vector<int>& scaleIntervals (Scale s) noexcept
{
    switch (s)
    {
        case Scale::Major:           return majorIntervals();
        case Scale::NaturalMinor:    return naturalMinorIntervals();
        case Scale::HarmonicMinor:   return harmonicMinorIntervals();
        case Scale::Dorian:          return dorianIntervals();
        case Scale::Phrygian:        return phrygianIntervals();
        case Scale::Lydian:          return lydianIntervals();
        case Scale::Mixolydian:      return mixolydianIntervals();
        case Scale::Locrian:         return locrianIntervals();
        case Scale::MajorPentatonic: return majorPentaIntervals();
        case Scale::MinorPentatonic: return minorPentaIntervals();
        case Scale::Chromatic:       return chromaticIntervals();
        case Scale::WholeTone:       return wholeToneIntervals();
    }
    return chromaticIntervals();
}

int snapPitchToScale (int pitch, Scale scale, int rootSemitone) noexcept
{
    if (pitch < 0)   pitch = 0;
    if (pitch > 127) pitch = 127;
    const int root = ((rootSemitone % 12) + 12) % 12;

    const auto& intervals = scaleIntervals (scale);
    if (intervals.empty())
        return pitch;

    /* Find the in-scale pitch with the smallest absolute distance.  We
     * scan the three octaves centred on `pitch` (previous / same / next)
     * to make sure we don't miss a closer in-scale tone across an octave
     * boundary -- e.g. pitch 71 (B) on a C-major scale should snap up to
     * 72 (C) rather than down to 69 (A).  Three octaves is enough cover
     * for any 12-tone scale: the worst case is 6 semitones in either
     * direction. */
    const int octaveOfPitch = pitch / 12;
    int bestPitch = pitch;
    int bestDist  = 999;
    for (int o = octaveOfPitch - 1; o <= octaveOfPitch + 1; ++o)
    {
        const int baseSemi = o * 12 + root;
        for (int iv : intervals)
        {
            const int candidate = baseSemi + iv;
            if (candidate < 0 || candidate > 127) continue;
            const int dist = std::abs (candidate - pitch);
            if (dist < bestDist
                || (dist == bestDist && candidate < bestPitch))
            {
                bestDist  = dist;
                bestPitch = candidate;
            }
        }
    }
    return bestPitch;
}

} // namespace element::dsp::quantize
