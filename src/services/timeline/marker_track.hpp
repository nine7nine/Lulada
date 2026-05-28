// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/core.hpp>
#include <element/juce/data_structures.hpp>
#include <element/juce/graphics.hpp>

#include <vector>

namespace element {

/** Named cue point on the arrangement timeline.  Markers are the
 *  arrangement's named jump targets -- distinct from the Range tool's
 *  loop overlay (which sets transport bounds rather than naming a
 *  position).  A user can place markers anywhere on the timeline and
 *  jump back to them via Numpad 1-9 (in marker-list order).
 *
 *  Persistence: round-tripped via toValueTree / fromValueTree under
 *  the session's tags::arrangement subtree, sibling to <lanes>. */
struct Marker
{
    juce::Uuid   id;
    juce::String name;
    double       positionBeats { 0.0 };
    juce::Colour colour        { 0xff'e0'c0'70 };

    juce::ValueTree toValueTree() const;
    static Marker   fromValueTree (const juce::ValueTree&);
};

/** Session-global ordered marker list.  Lives on ArrangementView and
 *  is serialised under <arrangement>/<markers>.  Pure data; no audio-
 *  thread consumer (markers drive UI seek only).  Operations mutate
 *  in-place + re-sort on insert. */
class MarkerTrack
{
public:
    MarkerTrack() = default;

    const std::vector<Marker>& markers() const noexcept { return markers_; }

    /** Insert a marker; auto-sorts by positionBeats so render order
     *  matches numpad jump order.  Returns the new marker's id. */
    juce::Uuid addMarker (double positionBeats, juce::String name = {});

    /** Remove by id; returns true if a marker was removed. */
    bool removeMarker (const juce::Uuid& markerId) noexcept;

    /** Rename + re-paint trigger.  Returns true if a marker was found. */
    bool renameMarker (const juce::Uuid& markerId, const juce::String& newName) noexcept;

    /** Recolour; returns true if a marker was found. */
    bool setMarkerColour (const juce::Uuid& markerId, juce::Colour c) noexcept;

    /** Move marker's position; re-sorts.  Returns true if found. */
    bool setMarkerPosition (const juce::Uuid& markerId, double positionBeats) noexcept;

    /** Look up by sorted index -- Numpad N jumps to markers_[N-1].
     *  Returns nullptr if index out of range. */
    const Marker* markerAt (std::size_t index) const noexcept;

    /** Find by id; nullptr if absent. */
    const Marker* findMarker (const juce::Uuid& markerId) const noexcept;

    /** Replace all markers from external source (used by undo / session
     *  reload).  Re-sorts by positionBeats. */
    void setMarkers (std::vector<Marker> markers);

    void clear() noexcept { markers_.clear(); }

    juce::ValueTree toValueTree() const;
    void            loadFromValueTree (const juce::ValueTree&);

private:
    void sortByPosition() noexcept;

    std::vector<Marker> markers_;
};

} // namespace element
