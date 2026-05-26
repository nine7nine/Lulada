// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace element {

/** Single MIDI note inside a MidiNoteRegion.
 *
 *  pitch          -- MIDI note number 0..127 (C-1..G9).  Centre C = 60.
 *  velocity       -- 1..127.  Velocity 0 is a note-off marker in the wire
 *                    protocol and never stored as a live on-note; the
 *                    region's loaded snapshot guarantees velocity >= 1.
 *  channel        -- 1..16 (1-based to match user-facing surfaces; the
 *                    audio adapter maps to JUCE's 0..15 status-byte
 *                    nibble at emit time).
 *  onBeat         -- local-beat offset measured from the owning region's
 *                    positionBeats.  Same convention as
 *                    EnvelopePoint.beatOffset (services/timeline/region.hpp).
 *  lengthBeats    -- duration in beats.  Notes that extend past
 *                    region.lengthBeats are clamped on emit; persistence
 *                    keeps the raw value so an extended region recovers
 *                    the original tail.
 *
 *  POD with trivial default ctor + trivial copy + trivial destructor so
 *  vectors of MidiNote can be constructed/copied/destroyed without
 *  per-element allocation.  The owning region's COW snapshot relies on
 *  this -- vector copy is just memcpy of the underlying buffer. */
struct MidiNote
{
    int    pitch       { 60 };
    int    velocity    { 100 };
    int    channel     { 1 };
    double onBeat      { 0.0 };
    double lengthBeats { 0.25 };
};

} // namespace element
