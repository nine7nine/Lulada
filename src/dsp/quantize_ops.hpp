// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dsp/quantize_options.hpp"

#include <cstdint>
#include <unordered_set>

namespace element {

class MidiNoteRegion;
class MidiNoteDiffCommand;

namespace dsp::quantize {

/** Walk `selection` over `region` and populate `out` with the updates
 *  needed to quantize each selected note's onset (and optionally end
 *  edge) per `options`.  `out` is NOT cleared first; callers may add
 *  unrelated updates to the same command if a single gesture combines
 *  multiple ops.
 *
 *  Implementation notes:
 *   - The snap-point list is built once from `region->lengthBeats` and
 *     reused across every selected note.
 *   - Notes whose snapped onset would push them past the region's tail
 *     are clamped to `lengthBeats - lengthBeats(note)`; this matches
 *     the nudge / drag-move code path in pianoroll_grid.cpp.
 *   - Selection ids that don't resolve in the snapshot are silently
 *     skipped (handles racey selection state after an external publish).
 *   - When randomBeats > 0, the random source is a Mersenne Twister
 *     seeded with options.seed; if seed == 0, a stable seed is derived
 *     from the sorted selection ids so the same call produces the same
 *     diff across repeat invocations (undo-safe).
 *
 *  Returns the number of notes touched (selection size minus skips /
 *  no-op snaps).  Useful for the dialog's live "X notes affected"
 *  preview label. */
std::size_t quantizeNotes (const MidiNoteRegion& region,
                            const std::unordered_set<std::uint64_t>& selection,
                            const QuantizeOptions& options,
                            MidiNoteDiffCommand& out);

/** Walk `selection` and emit velocity-only updates per `options`.  Each
 *  note's velocity is replaced with
 *  `clamp (current + bias + uniform(-range, +range), 1, 127)`.
 *  Deterministic with fixed seed -- see quantizeNotes for the
 *  seed-derivation rule. */
std::size_t humanizeVelocity (const MidiNoteRegion& region,
                               const std::unordered_set<std::uint64_t>& selection,
                               const HumanizeOptions& options,
                               MidiNoteDiffCommand& out);

/** Walk `selection` and emit pitch-only updates that snap each note to
 *  the nearest in-scale pitch.  Same skip semantics as quantizeNotes. */
std::size_t scaleQuantize (const MidiNoteRegion& region,
                            const std::unordered_set<std::uint64_t>& selection,
                            Scale scale,
                            int rootSemitone,
                            MidiNoteDiffCommand& out);

} // namespace dsp::quantize
} // namespace element
