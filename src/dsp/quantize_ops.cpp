// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dsp/quantize_ops.hpp"

#include "services/timeline/midi_note.hpp"
#include "services/timeline/midi_note_region.hpp"
#include "ui/pianoroll/midi_note_diff_command.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace element::dsp::quantize {

namespace {

constexpr double kBeatEpsilon = 1.0e-9;

/* Stable seed derivation for amount = 0 randomness paths.  Walks the
 * selection ids in sorted order so the same set produces the same
 * seed regardless of insertion order in the hash set. */
std::uint64_t deriveSeed (const std::unordered_set<std::uint64_t>& selection)
{
    std::vector<std::uint64_t> sorted (selection.begin(), selection.end());
    std::sort (sorted.begin(), sorted.end());
    /* FNV-1a 64-bit on the sorted byte sequence -- deterministic and
     * non-zero for any non-empty selection. */
    constexpr std::uint64_t kFnvOffset = 0xCBF29CE484222325ULL;
    constexpr std::uint64_t kFnvPrime  = 0x00000100000001B3ULL;
    std::uint64_t h = kFnvOffset;
    for (std::uint64_t id : sorted)
    {
        for (int b = 0; b < 8; ++b)
        {
            h ^= static_cast<std::uint8_t> ((id >> (b * 8)) & 0xFFu);
            h *= kFnvPrime;
        }
    }
    if (h == 0) h = kFnvOffset;
    return h;
}

bool nearlyEqual (double a, double b) noexcept
{
    return std::abs (a - b) <= kBeatEpsilon;
}

} // namespace

std::size_t quantizeNotes (const MidiNoteRegion& region,
                            const std::unordered_set<std::uint64_t>& selection,
                            const QuantizeOptions& options,
                            MidiNoteDiffCommand& out)
{
    if (selection.empty())            return 0;
    const auto* snap = region.loadSnapshot();
    if (snap == nullptr || snap->empty()) return 0;

    const double lengthBeats = region.lengthBeats > 0.0 ? region.lengthBeats : 0.0;
    if (lengthBeats <= 0.0) return 0;

    const auto points = buildQuantizePoints (options, lengthBeats);
    if (points.empty()) return 0;

    const double amount = std::clamp (options.amount, 0.0, 1.0);

    /* Deterministic RNG for randomBeats.  Seeded from options.seed or
     * derived from the selection set so undo/redo replay produces the
     * same jitter pattern that the user originally accepted. */
    const std::uint64_t seed = options.seed != 0 ? options.seed
                                                  : deriveSeed (selection);
    std::mt19937_64 rng (seed);
    const double randRange = std::max (0.0, options.randomBeats);
    std::uniform_real_distribution<double> jitter (-randRange, randRange);

    std::size_t touched = 0;
    for (const auto& note : *snap)
    {
        if (selection.find (note.id) == selection.end())
            continue;

        MidiNote after = note;
        const double origEnd = note.onBeat + note.lengthBeats;

        if (options.adjustStart)
        {
            double newOn = snapBeatMixed (note.onBeat, points, amount);
            if (randRange > 0.0) newOn += jitter (rng);
            if (newOn < 0.0) newOn = 0.0;
            after.onBeat = newOn;
        }

        if (options.adjustEnd)
        {
            double newEnd = snapBeatMixed (origEnd, points, amount);
            if (randRange > 0.0) newEnd += jitter (rng);
            const double minLen = 1.0 / 64.0;   /* never collapse a note */
            if (newEnd < after.onBeat + minLen)
                newEnd = after.onBeat + minLen;
            after.lengthBeats = newEnd - after.onBeat;
        }
        else if (options.adjustStart)
        {
            /* Preserve original duration -- the note's tail moves with
             * the head.  Matches Ardour + the audit's "adjustStart-only"
             * convention.  Without this, snapping the head alone
             * lengthens / shortens the note, which is rarely wanted. */
            after.lengthBeats = note.lengthBeats;
        }

        /* Clamp tail to region length so a quantize near the right
         * edge doesn't push notes past the region's lengthBeats. */
        if (after.onBeat + after.lengthBeats > lengthBeats)
        {
            const double maxOn = std::max (0.0, lengthBeats - after.lengthBeats);
            after.onBeat = std::min (after.onBeat, maxOn);
        }

        if (nearlyEqual (after.onBeat, note.onBeat)
            && nearlyEqual (after.lengthBeats, note.lengthBeats))
            continue;

        out.recordUpdate (note.id, note, after);
        ++touched;
    }
    return touched;
}

std::size_t humanizeVelocity (const MidiNoteRegion& region,
                               const std::unordered_set<std::uint64_t>& selection,
                               const HumanizeOptions& options,
                               MidiNoteDiffCommand& out)
{
    if (selection.empty())            return 0;
    const auto* snap = region.loadSnapshot();
    if (snap == nullptr || snap->empty()) return 0;

    const int range = std::max (0, options.velocityRange);
    const int bias  = options.velocityBias;
    const std::uint64_t seed = options.seed != 0 ? options.seed
                                                  : deriveSeed (selection);
    std::mt19937_64 rng (seed);
    std::uniform_int_distribution<int> jitter (-range, range);

    std::size_t touched = 0;
    for (const auto& note : *snap)
    {
        if (selection.find (note.id) == selection.end())
            continue;

        int v = note.velocity + bias;
        if (range > 0) v += jitter (rng);
        v = std::clamp (v, 1, 127);
        if (v == note.velocity) continue;

        MidiNote after = note;
        after.velocity = v;
        out.recordUpdate (note.id, note, after);
        ++touched;
    }
    return touched;
}

std::size_t scaleQuantize (const MidiNoteRegion& region,
                            const std::unordered_set<std::uint64_t>& selection,
                            Scale scale,
                            int rootSemitone,
                            MidiNoteDiffCommand& out)
{
    if (selection.empty())            return 0;
    const auto* snap = region.loadSnapshot();
    if (snap == nullptr || snap->empty()) return 0;

    std::size_t touched = 0;
    for (const auto& note : *snap)
    {
        if (selection.find (note.id) == selection.end())
            continue;

        const int snapped = snapPitchToScale (note.pitch, scale, rootSemitone);
        if (snapped == note.pitch) continue;

        MidiNote after = note;
        after.pitch = snapped;
        out.recordUpdate (note.id, note, after);
        ++touched;
    }
    return touched;
}

} // namespace element::dsp::quantize
