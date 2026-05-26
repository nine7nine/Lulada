// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "services/timeline/midi_note.hpp"

#include <element/juce/gui_basics.hpp>

#include <memory>
#include <vector>

namespace element {

class MidiNoteRegion;
class PianoRollGrid;

/** Base class for piano-roll mouse-drag gestures.  Mirrors Ardour's
 *  editor_drag.h taxonomy -- each gesture is a discrete class that
 *  owns its own state for the duration of a single mouseDown ->
 *  mouseUp cycle.
 *
 *  Lifecycle:
 *   - PianoRollGrid::mouseDown creates the appropriate NoteDrag
 *     subclass via the static make* factory and stores it as
 *     activeDrag_.
 *   - PianoRollGrid::mouseDrag forwards to NoteDrag::mouseDrag.
 *   - PianoRollGrid::mouseUp forwards to NoteDrag::mouseUp (which
 *     commits any region mutations through a MidiNoteDiffCommand
 *     pushed to GuiService::getUndoManager) and then destroys the
 *     drag instance.
 *   - paintOverlay is called during PianoRollGrid::paint so the
 *     gesture can draw its in-flight preview on top of the existing
 *     note rectangles.
 *
 *  Subclasses:
 *   - NoteDragMove    -- move selection by snap-aligned delta
 *   - NoteDragCreate  -- create new note with Pencil tool
 *   - NoteDragResize  -- right-edge resize handle
 *   - NoteDragMarquee -- rubber-band select (or erase) */
class NoteDrag
{
public:
    virtual ~NoteDrag() = default;

    virtual void mouseDrag    (const juce::MouseEvent&, PianoRollGrid&) = 0;
    virtual void mouseUp      (const juce::MouseEvent&, PianoRollGrid&) = 0;
    virtual void paintOverlay (juce::Graphics&,        PianoRollGrid&) {}

    static std::unique_ptr<NoteDrag> makeMove    (PianoRollGrid&,
                                                    MidiNoteRegion&,
                                                    std::uint64_t hitId,
                                                    const juce::MouseEvent& e);
    static std::unique_ptr<NoteDrag> makeCreate  (PianoRollGrid&,
                                                    MidiNoteRegion&,
                                                    const juce::MouseEvent& e);
    static std::unique_ptr<NoteDrag> makeResize  (PianoRollGrid&,
                                                    MidiNoteRegion&,
                                                    std::uint64_t hitId,
                                                    const juce::MouseEvent& e);
    static std::unique_ptr<NoteDrag> makeMarquee (PianoRollGrid&,
                                                    const juce::MouseEvent& e,
                                                    bool                    eraseMode);
};

} // namespace element
