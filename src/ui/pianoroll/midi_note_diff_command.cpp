// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/pianoroll/midi_note_diff_command.hpp"
#include "services/timeline/midi_note_region.hpp"

namespace element {

MidiNoteDiffCommand::MidiNoteDiffCommand (juce::Uuid regionId,
                                            RegionResolver resolver)
    : regionId_ (regionId), resolver_ (std::move (resolver))
{
}

void MidiNoteDiffCommand::recordAdd (MidiNoteRegion& region, MidiNote note)
{
    if (note.id == 0)
        note.id = region.nextNoteId();
    adds_.push_back ({ note });
}

void MidiNoteDiffCommand::recordRemove (MidiNoteRegion& region, std::uint64_t noteId)
{
    if (noteId == 0) return;
    const auto* snap = region.loadSnapshot();
    if (snap == nullptr) return;
    for (const auto& n : *snap)
    {
        if (n.id == noteId)
        {
            removes_.push_back ({ n });
            return;
        }
    }
}

void MidiNoteDiffCommand::recordUpdate (std::uint64_t noteId,
                                          const MidiNote& before,
                                          const MidiNote& after)
{
    if (noteId == 0) return;
    if (before.pitch       == after.pitch
        && before.velocity == after.velocity
        && before.channel  == after.channel
        && before.onBeat   == after.onBeat
        && before.lengthBeats == after.lengthBeats)
        return;   /* no-op */
    updates_.push_back ({ noteId, before, after });
}

bool MidiNoteDiffCommand::perform()
{
    if (! resolver_) return true;
    auto* region = resolver_ (regionId_);
    if (region == nullptr) return true;

    /* Apply in stable order: removes first, then adds, then updates.
     * This matches Ardour's NoteDiffCommand semantics -- removes get
     * out of the way before adds re-introduce ids, and updates apply
     * to the post-add state. */
    for (const auto& r : removes_)
        region->removeNoteById (r.note.id);

    for (const auto& a : adds_)
        region->addNote (a.note);

    for (const auto& u : updates_)
    {
        MidiNote target = u.after;
        target.id = u.id;
        region->updateNoteById (u.id, target);
    }

    if (onApplied) onApplied();
    return true;
}

bool MidiNoteDiffCommand::undo()
{
    if (! resolver_) return true;
    auto* region = resolver_ (regionId_);
    if (region == nullptr) return true;

    /* Reverse order: undo updates, undo adds (remove them), undo
     * removes (re-add them).  Each is the inverse of perform()'s
     * forward pass. */
    for (auto it = updates_.rbegin(); it != updates_.rend(); ++it)
    {
        MidiNote target = it->before;
        target.id = it->id;
        region->updateNoteById (it->id, target);
    }

    for (auto it = adds_.rbegin(); it != adds_.rend(); ++it)
        region->removeNoteById (it->note.id);

    for (auto it = removes_.rbegin(); it != removes_.rend(); ++it)
        region->addNote (it->note);   /* note carries its original id */

    if (onApplied) onApplied();
    return true;
}

} // namespace element
