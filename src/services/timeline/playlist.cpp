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

Playlist::Playlist (const Playlist& other)
    : id_       (other.id_),
      regions_  (other.regions_)
{
    /* MidiNoteRegion is non-copyable; clone each one.  The cloned
     * vector keeps the same beat-sort order as the source. */
    midiRegions_.reserve (other.midiRegions_.size());
    for (const auto& m : other.midiRegions_)
        if (m != nullptr)
            midiRegions_.push_back (m->clone());
}

Playlist& Playlist::operator= (const Playlist& other)
{
    if (this == &other) return *this;
    id_      = other.id_;
    regions_ = other.regions_;
    midiRegions_.clear();
    midiRegions_.reserve (other.midiRegions_.size());
    for (const auto& m : other.midiRegions_)
        if (m != nullptr)
            midiRegions_.push_back (m->clone());
    return *this;
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

//==============================================================================
// MIDI region operations.  Parallel to the audio/tracker region API; the
// storage shape is std::vector<unique_ptr<MidiNoteRegion>> because
// MidiNoteRegion is non-copyable.

bool Playlist::addMidiRegion (std::unique_ptr<MidiNoteRegion> r)
{
    if (r == nullptr)                return false;
    if (r->lengthBeats < 0.0)        return false;

    /* Overlap rejection -- mirror the audio path's no-overlap policy.
     * Two MIDI regions on the same lane occupying the same beat range
     * would feed duplicate NoteOn/NoteOff streams into the bound
     * MidiPlayerNode; even when the synth handles it gracefully the
     * extra event traffic is wasted work.  Caller can split first +
     * add second to butt-join cleanly. */
    const double newStart = r->positionBeats;
    const double newEnd   = newStart + r->lengthBeats;
    for (const auto& other : midiRegions_)
    {
        if (other == nullptr) continue;
        const double otherEnd = other->positionBeats + other->lengthBeats;
        if (newStart < otherEnd && newEnd > other->positionBeats)
            return false;
    }

    midiRegions_.push_back (std::move (r));
    rebuildMidiOrder();
    return true;
}

bool Playlist::removeMidiRegion (juce::Uuid regionId)
{
    const auto before = midiRegions_.size();
    midiRegions_.erase (
        std::remove_if (midiRegions_.begin(), midiRegions_.end(),
                        [regionId] (const std::unique_ptr<MidiNoteRegion>& m)
                        {
                            return m != nullptr && m->id == regionId;
                        }),
        midiRegions_.end());
    return midiRegions_.size() != before;
}

MidiNoteRegion* Playlist::findMidiRegion (juce::Uuid regionId) noexcept
{
    for (auto& m : midiRegions_)
        if (m != nullptr && m->id == regionId)
            return m.get();
    return nullptr;
}

const MidiNoteRegion* Playlist::findMidiRegion (juce::Uuid regionId) const noexcept
{
    for (const auto& m : midiRegions_)
        if (m != nullptr && m->id == regionId)
            return m.get();
    return nullptr;
}

void Playlist::rebuildMidiOrder() noexcept
{
    std::sort (midiRegions_.begin(), midiRegions_.end(),
               [] (const std::unique_ptr<MidiNoteRegion>& a,
                   const std::unique_ptr<MidiNoteRegion>& b)
               {
                   if (a == nullptr) return false;
                   if (b == nullptr) return true;
                   return a->positionBeats < b->positionBeats;
               });
}

juce::Uuid Playlist::splitMidiRegion (juce::Uuid regionId, double atBeat)
{
    /* atBeat is in SESSION beats (same coord system as positionBeats);
     * split is rejected if it lands on or outside the region's actual
     * range (no zero-length halves). */
    auto* left = findMidiRegion (regionId);
    if (left == nullptr) return juce::Uuid::null();

    const double leftStart = left->positionBeats;
    const double leftEnd   = leftStart + left->lengthBeats;
    if (atBeat <= leftStart + 1e-9)  return juce::Uuid::null();
    if (atBeat >= leftEnd   - 1e-9)  return juce::Uuid::null();

    const double cutLocal = atBeat - leftStart;   /* offset into left region */

    /* Snapshot the left region's note list, partition into stays /
     * moves.  Notes whose onBeat falls before the cut stay; notes at
     * or after the cut migrate.  Notes that STRADDLE the cut (start
     * before, end after) are TRUNCATED at the cut on the left half
     * and not duplicated into the right -- matches Ableton + Bitwig. */
    auto leftNotes  = MidiNoteRegion::NoteList{};
    auto rightNotes = MidiNoteRegion::NoteList{};

    if (const auto* snap = left->loadSnapshot())
    {
        leftNotes .reserve (snap->size());
        rightNotes.reserve (snap->size());
        for (const auto& n : *snap)
        {
            if (n.onBeat >= cutLocal)
            {
                MidiNote r = n;
                r.onBeat -= cutLocal;
                rightNotes.push_back (r);
            }
            else
            {
                MidiNote l = n;
                /* Truncate at the cut if the note extends past it. */
                const double localEnd = l.onBeat + l.lengthBeats;
                if (localEnd > cutLocal)
                    l.lengthBeats = cutLocal - l.onBeat;
                leftNotes.push_back (l);
            }
        }
    }

    /* Mutate the left half in place: shrink lengthBeats + publish the
     * truncated note set.  ID + positionBeats + sourceId untouched. */
    left->lengthBeats = cutLocal;
    left->setNotes (std::move (leftNotes));

    /* Build the right half.  Fresh uuid + positionBeats placed at the
     * cut.  Inherit name / colour / looped / sourceId from the left
     * (sourceId is shared if the original was imported; both halves
     * trace back to the same SMF blob).  Note ids are stamped fresh
     * by setNotesAssigningIds so the right half's piano-roll
     * selection has clean identities. */
    auto right = std::make_unique<MidiNoteRegion>();
    right->id            = juce::Uuid();
    right->sourceId      = left->sourceId;
    right->positionBeats = atBeat;
    right->lengthBeats   = leftEnd - atBeat;
    right->looped        = left->looped;
    right->colour        = left->colour;
    right->name          = left->name;
    right->setNotesAssigningIds (std::move (rightNotes));

    const juce::Uuid newId = right->id;

    /* addMidiRegion's overlap check runs against the now-shortened
     * left so the right half lands cleanly.  rebuildMidiOrder runs
     * inside addMidiRegion. */
    if (! addMidiRegion (std::move (right)))
        return juce::Uuid::null();

    return newId;
}

juce::ValueTree Playlist::toValueTree() const
{
    juce::ValueTree v ("playlist");
    v.setProperty (kIdAttr, id_.toString(), nullptr);
    for (const auto& r : regions_)
        v.appendChild (r.toValueTree(), nullptr);
    for (const auto& m : midiRegions_)
        if (m != nullptr)
            v.appendChild (m->toValueTree(), nullptr);
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
        if (child.getType() == juce::Identifier ("region"))
        {
            p.regions_.push_back (Region::fromValueTree (child));
        }
        else if (child.getType() == juce::Identifier ("midiNoteRegion"))
        {
            auto m = MidiNoteRegion::fromValueTree (child);
            if (m != nullptr)
                p.midiRegions_.push_back (std::move (m));
        }
    }
    p.rebuildOrder();
    p.rebuildMidiOrder();
    return p;
}

} // namespace element
