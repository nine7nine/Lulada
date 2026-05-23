// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/core.hpp>
#include <element/juce/data_structures.hpp>
#include <element/juce/graphics.hpp>

namespace element {

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
    double        positionBeats { 0.0 };
    double        startBeats    { 0.0 };
    double        lengthBeats   { 0.0 };
    double        gainDb        { 0.0 };
    double        fadeInBeats   { 0.0 };
    double        fadeOutBeats  { 0.0 };
    bool          looped        { false };
    juce::Colour  colour        { 0xff'4a'7a'b5 };
    juce::String  name;

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
