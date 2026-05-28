// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/timeline/marker_track.hpp"

#include <algorithm>

namespace element {

namespace {

const juce::Identifier kMarkerTag     { "marker" };
const juce::Identifier kIdAttr        { "id" };
const juce::Identifier kNameAttr      { "name" };
const juce::Identifier kPositionAttr  { "pos" };
const juce::Identifier kColourAttr    { "col" };
const juce::Identifier kMarkersTag    { "markers" };

constexpr juce::uint32 kDefaultColourArgb = 0xff'e0'c0'70u;

} // namespace

//==============================================================================

juce::ValueTree Marker::toValueTree() const
{
    juce::ValueTree v (kMarkerTag);
    v.setProperty (kIdAttr,       id.toString(),                          nullptr);
    if (name.isNotEmpty())
        v.setProperty (kNameAttr, name,                                   nullptr);
    if (positionBeats != 0.0)
        v.setProperty (kPositionAttr, positionBeats,                      nullptr);
    if (colour.getARGB() != kDefaultColourArgb)
        v.setProperty (kColourAttr, (juce::int64) colour.getARGB(),       nullptr);
    return v;
}

Marker Marker::fromValueTree (const juce::ValueTree& v)
{
    Marker m;
    m.id            = juce::Uuid (v.getProperty (kIdAttr).toString());
    m.name          = v.getProperty (kNameAttr, juce::String()).toString();
    m.positionBeats = (double) v.getProperty (kPositionAttr, 0.0);
    if (v.hasProperty (kColourAttr))
        m.colour = juce::Colour ((juce::uint32) (juce::int64) v.getProperty (kColourAttr));
    return m;
}

//==============================================================================

juce::Uuid MarkerTrack::addMarker (double positionBeats, juce::String name)
{
    Marker m;
    m.id            = juce::Uuid();
    m.positionBeats = juce::jmax (0.0, positionBeats);
    m.name          = std::move (name);
    const auto id = m.id;
    markers_.push_back (std::move (m));
    sortByPosition();
    return id;
}

bool MarkerTrack::removeMarker (const juce::Uuid& markerId) noexcept
{
    const auto before = markers_.size();
    markers_.erase (std::remove_if (markers_.begin(), markers_.end(),
                                     [&] (const Marker& m) noexcept
                                     { return m.id == markerId; }),
                    markers_.end());
    return markers_.size() != before;
}

bool MarkerTrack::renameMarker (const juce::Uuid& markerId,
                                const juce::String& newName) noexcept
{
    for (auto& m : markers_)
        if (m.id == markerId)
        {
            m.name = newName;
            return true;
        }
    return false;
}

bool MarkerTrack::setMarkerColour (const juce::Uuid& markerId, juce::Colour c) noexcept
{
    for (auto& m : markers_)
        if (m.id == markerId)
        {
            m.colour = c;
            return true;
        }
    return false;
}

bool MarkerTrack::setMarkerPosition (const juce::Uuid& markerId,
                                      double positionBeats) noexcept
{
    for (auto& m : markers_)
        if (m.id == markerId)
        {
            m.positionBeats = juce::jmax (0.0, positionBeats);
            sortByPosition();
            return true;
        }
    return false;
}

const Marker* MarkerTrack::markerAt (std::size_t index) const noexcept
{
    if (index >= markers_.size()) return nullptr;
    return &markers_[index];
}

const Marker* MarkerTrack::findMarker (const juce::Uuid& markerId) const noexcept
{
    for (const auto& m : markers_)
        if (m.id == markerId)
            return &m;
    return nullptr;
}

void MarkerTrack::setMarkers (std::vector<Marker> markers)
{
    markers_ = std::move (markers);
    sortByPosition();
}

void MarkerTrack::sortByPosition() noexcept
{
    std::sort (markers_.begin(), markers_.end(),
               [] (const Marker& a, const Marker& b) noexcept
               { return a.positionBeats < b.positionBeats; });
}

juce::ValueTree MarkerTrack::toValueTree() const
{
    juce::ValueTree v (kMarkersTag);
    for (const auto& m : markers_)
        v.appendChild (m.toValueTree(), nullptr);
    return v;
}

void MarkerTrack::loadFromValueTree (const juce::ValueTree& v)
{
    markers_.clear();
    if (! v.hasType (kMarkersTag)) return;
    markers_.reserve ((size_t) v.getNumChildren());
    for (int i = 0; i < v.getNumChildren(); ++i)
    {
        const auto child = v.getChild (i);
        if (! child.hasType (kMarkerTag)) continue;
        markers_.push_back (Marker::fromValueTree (child));
    }
    sortByPosition();
}

} // namespace element
