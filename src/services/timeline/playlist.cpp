// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/timeline/playlist.hpp"

#include <algorithm>

namespace element {

namespace {
const juce::Identifier kIdAttr ("id");
} // namespace

Playlist::Playlist()
    : id_ (juce::Uuid())
{
}

bool Playlist::addRegion (Region r)
{
    if (r.lengthBeats < 0.0)  return false;
    if (overlapsExisting (r.positionBeats, r.lengthBeats, r.id))
        return false;

    regions_.push_back (std::move (r));
    rebuildOrder();
    return true;
}

bool Playlist::removeRegion (juce::Uuid regionId)
{
    const auto before = regions_.size();
    regions_.erase (
        std::remove_if (regions_.begin(), regions_.end(),
                        [regionId] (const Region& r) { return r.id == regionId; }),
        regions_.end());
    return regions_.size() != before;
}

bool Playlist::moveRegion (juce::Uuid regionId, double newPositionBeats)
{
    auto* r = findRegion (regionId);
    if (r == nullptr) return false;

    const double oldPos = r->positionBeats;
    r->positionBeats = newPositionBeats;
    if (overlapsExisting (r->positionBeats, r->lengthBeats, regionId))
    {
        r->positionBeats = oldPos;     // restore
        return false;
    }
    rebuildOrder();
    return true;
}

bool Playlist::resizeRegion (juce::Uuid regionId, double newLengthBeats)
{
    if (newLengthBeats < 0.0) return false;

    auto* r = findRegion (regionId);
    if (r == nullptr) return false;

    const double oldLen = r->lengthBeats;
    r->lengthBeats = newLengthBeats;
    if (overlapsExisting (r->positionBeats, r->lengthBeats, regionId))
    {
        r->lengthBeats = oldLen;       // restore
        return false;
    }
    return true;
}

const Region* Playlist::regionAt (double beat) const noexcept
{
    /* Sorted by positionBeats; first region whose span contains
     * `beat` is the answer (v1 disallows overlap so it's unique). */
    for (const auto& r : regions_)
    {
        if (r.positionBeats > beat) break;
        if (r.containsBeat (beat)) return &r;
    }
    return nullptr;
}

const Region* Playlist::findRegion (juce::Uuid regionId) const noexcept
{
    for (const auto& r : regions_)
        if (r.id == regionId) return &r;
    return nullptr;
}

Region* Playlist::findRegion (juce::Uuid regionId) noexcept
{
    for (auto& r : regions_)
        if (r.id == regionId) return &r;
    return nullptr;
}

bool Playlist::overlapsExisting (double position, double length,
                                  juce::Uuid excludeId) const noexcept
{
    const double aStart = position;
    const double aEnd   = position + length;
    for (const auto& r : regions_)
    {
        if (r.id == excludeId) continue;
        const double bStart = r.positionBeats;
        const double bEnd   = r.endBeats();
        if (aStart < bEnd && bStart < aEnd) return true;
    }
    return false;
}

void Playlist::rebuildOrder() noexcept
{
    std::sort (regions_.begin(), regions_.end(),
               [] (const Region& a, const Region& b) {
                   return a.positionBeats < b.positionBeats;
               });
}

juce::ValueTree Playlist::toValueTree() const
{
    juce::ValueTree v ("playlist");
    v.setProperty (kIdAttr, id_.toString(), nullptr);
    for (const auto& r : regions_)
        v.appendChild (r.toValueTree(), nullptr);
    return v;
}

Playlist Playlist::fromValueTree (const juce::ValueTree& v)
{
    Playlist p;
    if (! v.isValid() || v.getType() != juce::Identifier ("playlist"))
        return p;

    const auto idStr = v.getProperty (kIdAttr).toString();
    if (idStr.isNotEmpty()) p.id_ = juce::Uuid (idStr);

    for (int i = 0; i < v.getNumChildren(); ++i)
    {
        const auto child = v.getChild (i);
        if (child.getType() != juce::Identifier ("region")) continue;
        p.regions_.push_back (Region::fromValueTree (child));
    }
    p.rebuildOrder();
    return p;
}

} // namespace element
