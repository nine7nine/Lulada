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
    /* Overlap-allowed: regions may share or contain beat spans.
     * regionAt returns the first hit in position order, which gives
     * "earliest-start wins" playback resolution.  v2 may introduce
     * an explicit z-order property for top-wins. */
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

    /* Overlap-allowed (see addRegion).  Move freely; rebuildOrder
     * keeps positionBeats-sort intact. */
    r->positionBeats = newPositionBeats;
    rebuildOrder();
    return true;
}

bool Playlist::resizeRegion (juce::Uuid regionId, double newLengthBeats)
{
    if (newLengthBeats < 0.0) return false;

    auto* r = findRegion (regionId);
    if (r == nullptr) return false;

    /* Overlap-allowed; downstream paint + playback resolve via
     * earliest-start-wins regionAt(). */
    r->lengthBeats = newLengthBeats;
    return true;
}

juce::Uuid Playlist::splitRegion (juce::Uuid regionId, double atBeat)
{
    static constexpr double kMinFragmentBeats = 0.0625;   /* 1/16 of a beat */

    auto* head = findRegion (regionId);
    if (head == nullptr) return juce::Uuid();

    const double splitOffset = atBeat - head->positionBeats;
    if (splitOffset < kMinFragmentBeats) return juce::Uuid();

    const double tailLength = head->lengthBeats - splitOffset;
    if (tailLength < kMinFragmentBeats) return juce::Uuid();

    /* Build the tail by copy + adjust.  Source-offset is preserved
     * across the cut so audio playback flows through the seam: the
     * tail's startBeats picks up where the head left off in the
     * source file. */
    Region tail = *head;
    tail.id            = juce::Uuid();
    tail.positionBeats = atBeat;
    tail.lengthBeats   = tailLength;
    tail.startBeats    = head->startBeats + splitOffset;

    /* Shrink the head in place.  head pointer stays valid since we
     * don't mutate the vector here. */
    head->lengthBeats = splitOffset;

    /* Append + re-sort.  push_back may invalidate `head` but we're
     * done with it by now. */
    const juce::Uuid newId = tail.id;
    regions_.push_back (std::move (tail));
    rebuildOrder();
    return newId;
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
