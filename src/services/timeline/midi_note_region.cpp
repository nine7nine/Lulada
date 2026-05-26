// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/timeline/midi_note_region.hpp"

#include <algorithm>
#include <cmath>

namespace element {

namespace {

/* ValueTree property identifiers for sparse-write serialisation.
 * Short attribute names keep round-tripped XML compact even with
 * hundreds of notes per region. */
const juce::Identifier kIdAttr        { "id" };
const juce::Identifier kSourceAttr    { "src" };
const juce::Identifier kPositionAttr  { "pos" };
const juce::Identifier kLengthAttr    { "len" };
const juce::Identifier kLoopedAttr    { "loop" };
const juce::Identifier kColourAttr    { "col" };
const juce::Identifier kNameAttr      { "name" };
const juce::Identifier kNoteTag       { "n" };
const juce::Identifier kNotePitchAttr { "p" };
const juce::Identifier kNoteVelAttr   { "v" };
const juce::Identifier kNoteChanAttr  { "ch" };
const juce::Identifier kNoteOnAttr    { "t" };
const juce::Identifier kNoteLenAttr   { "l" };
const juce::Identifier kMidiNoteRegionTag { "midiNoteRegion" };

/* Threshold for "matching" floating-point coordinates in
 * removeNotesMatching.  Notes are user-placed so the natural epsilon
 * is well above double precision; 1e-6 beats covers any UI-grid
 * quantisation we will see in practice. */
constexpr double kNoteMatchEps = 1e-6;

/* Sort comparator -- (onBeat, pitch) ascending.  Stable ordering used
 * by both setNotes and the audio-thread paint loop. */
inline bool noteLess (const MidiNote& a, const MidiNote& b) noexcept
{
    if (a.onBeat != b.onBeat) return a.onBeat < b.onBeat;
    return a.pitch < b.pitch;
}

} // namespace

//==============================================================================

MidiNoteRegion::MidiNoteRegion()
{
    /* Publish an empty snapshot up front so the audio thread never
     * observes a null active-notes pointer.  Ownership: the live
     * snapshot is owned by the region until either replaced (old
     * goes to trash_) or the region is destroyed. */
    activeNotes_.store (new NoteList(), std::memory_order_release);
}

MidiNoteRegion::~MidiNoteRegion()
{
    /* Reclaim the live snapshot.  trash_ unique_ptrs reclaim themselves
     * at deque destruction. */
    if (auto* live = activeNotes_.exchange (nullptr, std::memory_order_acq_rel))
        delete live;
}

std::unique_ptr<MidiNoteRegion> MidiNoteRegion::clone() const
{
    auto out = std::make_unique<MidiNoteRegion>();
    out->id            = id;
    out->sourceId      = sourceId;
    out->positionBeats = positionBeats;
    out->lengthBeats   = lengthBeats;
    out->looped        = looped;
    out->colour        = colour;
    out->name          = name;

    if (const auto* snap = loadSnapshot())
        out->setNotes (*snap);    /* setNotes makes its own copy + sorts */
    return out;
}

//==============================================================================

void MidiNoteRegion::setNotes (NoteList newNotes)
{
    std::sort (newNotes.begin(), newNotes.end(), noteLess);
    auto snap = std::make_unique<NoteList> (std::move (newNotes));
    publishSnapshot (std::move (snap));
}

void MidiNoteRegion::addNote (MidiNote n)
{
    mutateAndPublish ([&] (NoteList& copy)
    {
        copy.push_back (n);
        std::sort (copy.begin(), copy.end(), noteLess);
    });
}

void MidiNoteRegion::removeNotesMatching (const MidiNote& example) noexcept
{
    mutateAndPublish ([&] (NoteList& copy)
    {
        copy.erase (std::remove_if (copy.begin(), copy.end(),
                       [&] (const MidiNote& nx) noexcept
                       {
                           return nx.pitch   == example.pitch
                               && nx.channel == example.channel
                               && std::abs (nx.onBeat - example.onBeat) < kNoteMatchEps;
                       }),
                    copy.end());
    });
}

void MidiNoteRegion::sweepTrash() noexcept
{
    /* Message-thread reclaim with epoch-guarded grace period.  An entry
     * is safe to free only once audioEpoch_ has STRICTLY advanced past
     * the stamp at publish time -- that means the audio thread loaded
     * at least one new snapshot since the publish, so the old pointer
     * cannot still be in flight on the audio path. */
    const auto safeEpoch = audioEpoch_.load (std::memory_order_acquire);
    while (! trash_.empty() && trash_.front().stampEpoch < safeEpoch)
        trash_.pop_front();
}

//==============================================================================

void MidiNoteRegion::publishSnapshot (std::unique_ptr<NoteList> newSnap)
{
    const NoteList* raw   = newSnap.release();
    const auto      stamp = audioEpoch_.load (std::memory_order_acquire);
    const NoteList* old   = activeNotes_.exchange (raw, std::memory_order_acq_rel);
    if (old != nullptr)
        trash_.push_back (TrashEntry { std::unique_ptr<const NoteList> (old), stamp });
}

template <typename Mutator>
void MidiNoteRegion::mutateAndPublish (Mutator&& mutate)
{
    const auto* live = activeNotes_.load (std::memory_order_acquire);
    auto next = (live != nullptr)
                  ? std::make_unique<NoteList> (*live)
                  : std::make_unique<NoteList>();
    mutate (*next);
    publishSnapshot (std::move (next));
}

//==============================================================================

juce::ValueTree MidiNoteRegion::toValueTree() const
{
    juce::ValueTree v (kMidiNoteRegionTag);
    v.setProperty (kIdAttr, id.toString(), nullptr);
    if (! sourceId.isNull())
        v.setProperty (kSourceAttr, sourceId.toString(), nullptr);
    if (positionBeats != 0.0) v.setProperty (kPositionAttr, positionBeats, nullptr);
    if (lengthBeats   != 0.0) v.setProperty (kLengthAttr,   lengthBeats,   nullptr);
    if (looped)               v.setProperty (kLoopedAttr,   true,          nullptr);
    /* Default colour matches the ctor; only persist when overridden. */
    if (colour.getARGB() != juce::Colour (0xff'5a'8a'5a).getARGB())
        v.setProperty (kColourAttr, (juce::int64) colour.getARGB(), nullptr);
    if (name.isNotEmpty()) v.setProperty (kNameAttr, name, nullptr);

    if (const auto* snap = loadSnapshot())
    {
        for (const auto& n : *snap)
        {
            juce::ValueTree nv (kNoteTag);
            nv.setProperty (kNotePitchAttr, n.pitch,       nullptr);
            nv.setProperty (kNoteVelAttr,   n.velocity,    nullptr);
            /* Channel 1 is the default; sparse-write skips it. */
            if (n.channel != 1)
                nv.setProperty (kNoteChanAttr, n.channel, nullptr);
            nv.setProperty (kNoteOnAttr,    n.onBeat,      nullptr);
            nv.setProperty (kNoteLenAttr,   n.lengthBeats, nullptr);
            v.appendChild (nv, nullptr);
        }
    }
    return v;
}

std::unique_ptr<MidiNoteRegion> MidiNoteRegion::fromValueTree (const juce::ValueTree& v)
{
    if (! v.hasType (kMidiNoteRegionTag))
        return nullptr;

    auto r = std::make_unique<MidiNoteRegion>();
    r->id = juce::Uuid (v.getProperty (kIdAttr).toString());
    if (v.hasProperty (kSourceAttr))
        r->sourceId = juce::Uuid (v.getProperty (kSourceAttr).toString());
    r->positionBeats = (double) v.getProperty (kPositionAttr, 0.0);
    r->lengthBeats   = (double) v.getProperty (kLengthAttr,   0.0);
    r->looped        = (bool)   v.getProperty (kLoopedAttr,   false);
    if (v.hasProperty (kColourAttr))
        r->colour = juce::Colour ((juce::uint32) (juce::int64) v.getProperty (kColourAttr));
    r->name          = v.getProperty (kNameAttr, juce::String()).toString();

    NoteList notes;
    notes.reserve ((size_t) v.getNumChildren());
    for (int i = 0; i < v.getNumChildren(); ++i)
    {
        auto nv = v.getChild (i);
        if (! nv.hasType (kNoteTag))
            continue;
        MidiNote n;
        n.pitch       = (int)    nv.getProperty (kNotePitchAttr, 60);
        n.velocity    = (int)    nv.getProperty (kNoteVelAttr,   100);
        n.channel     = (int)    nv.getProperty (kNoteChanAttr,  1);
        n.onBeat      = (double) nv.getProperty (kNoteOnAttr,    0.0);
        n.lengthBeats = (double) nv.getProperty (kNoteLenAttr,   0.25);
        notes.push_back (n);
    }
    r->setNotes (std::move (notes));
    return r;
}

} // namespace element
