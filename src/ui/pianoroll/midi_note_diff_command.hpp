// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "services/timeline/midi_note.hpp"

#include <element/juce/data_structures.hpp>

#include <functional>
#include <vector>

namespace element {

class MidiNoteRegion;

/** UndoableAction wrapping one piano-roll edit gesture.  Ardour
 *  borrowed pattern (libs/ardour/midi_model.cc NoteDiffCommand) but
 *  built on Element's UUID + id-stable MidiNote model rather than
 *  Evoral's sequence-tree index.
 *
 *  Three kinds of operations per command:
 *   - Add    : a fresh note created by the gesture.  We pre-assign
 *              the id via region->nextNoteId() at record time so
 *              perform() / undo() can address it deterministically
 *              even across redo cycles.
 *   - Remove : a note removed by the gesture (Erase tool, Delete
 *              key).  Records the FULL note state so undo can
 *              re-create it with the same id + pitch + velocity +
 *              channel + on/length.
 *   - Update : an existing note's mutable fields changed (Move,
 *              Resize, Velocity).  Records before + after.
 *
 *  Commands are constructed empty and populated via record* methods
 *  during the gesture's mouseUp commit; the GuiService UndoManager
 *  then perform()s the populated command, which applies the recorded
 *  state to the bound region.
 *
 *  Region resolution is done by uuid at perform / undo time, so a
 *  region that's been removed + readded across an undo boundary
 *  re-resolves correctly.  If the region can't be resolved, the
 *  perform / undo is a no-op (returns true so UndoManager doesn't
 *  bail). */
class MidiNoteDiffCommand : public juce::UndoableAction
{
public:
    using RegionResolver = std::function<MidiNoteRegion* (const juce::Uuid&)>;

    MidiNoteDiffCommand (juce::Uuid regionId, RegionResolver resolver);
    ~MidiNoteDiffCommand() override = default;

    /** Record a freshly-created note.  Pre-assigns an id via
     *  region->nextNoteId() so the perform()/undo() flow has a
     *  stable handle.  The caller passes the note WITHOUT its id
     *  set; this method fills it in. */
    void recordAdd    (MidiNoteRegion& region, MidiNote note);

    /** Record a note removal.  Captures the full current state of
     *  the note so undo() can re-instate it. */
    void recordRemove (MidiNoteRegion& region, std::uint64_t noteId);

    /** Record a field update.  Before and after must agree on id. */
    void recordUpdate (std::uint64_t noteId, const MidiNote& before, const MidiNote& after);

    /** Returns true if the command contains at least one op. */
    bool isEmpty() const noexcept
    {
        return adds_.empty() && removes_.empty() && updates_.empty();
    }

    bool perform() override;
    bool undo()    override;

    /** UndoManager merges consecutive commands that come back from
     *  createCoalescedAction.  For now we don't coalesce -- each
     *  gesture is its own command. */
    juce::UndoableAction* createCoalescedAction (juce::UndoableAction*) override { return nullptr; }

private:
    juce::Uuid     regionId_;
    RegionResolver resolver_;

    struct Add    { MidiNote note;                            };
    struct Remove { MidiNote note;                            };  /* full state */
    struct Update { std::uint64_t id; MidiNote before, after; };

    std::vector<Add>    adds_;
    std::vector<Remove> removes_;
    std::vector<Update> updates_;
};

} // namespace element
