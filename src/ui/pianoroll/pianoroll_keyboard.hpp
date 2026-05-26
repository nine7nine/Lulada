// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/* MidiKeyboardComponent / MidiKeyboardState live in juce_audio_utils;
 * Element doesn't ship an element/juce/audio_utils.hpp wrapper (same
 * situation as the AudioThumbnail include in arrangementview.hpp).
 * Pull the JUCE module header directly. */
#include <element/juce/gui_basics.hpp>
#include <juce_audio_utils/juce_audio_utils.h>

namespace element {

namespace detail {
/* Private state holder used as a base class purely to control C++
 * initialization order: base classes are constructed left-to-right
 * BEFORE member fields.  If PianoRollKeyboard stored its
 * MidiKeyboardState as a MEMBER and passed it to the
 * MidiKeyboardComponent base ctor, the base would observe
 * uninitialized memory (the member's ctor hasn't run yet) and
 * juce::MidiKeyboardComponent::MidiKeyboardComponent calls
 * `state.addListener(this)` immediately -- guaranteed crash on
 * the uninitialized ListenerList.  Holding the state inside a
 * helper base instead, declared LEFT of MidiKeyboardComponent in
 * the inheritance list, makes the state fully constructed by the
 * time the keyboard's ctor runs. */
struct PianoRollKeyboardStateHolder
{
    /* Named `internalState` (not just `state`) to keep it
     * unambiguous: juce::MidiKeyboardComponent exposes its own
     * `state` reference member, and an unqualified `state` inside
     * PianoRollKeyboard would otherwise be ambiguous. */
    juce::MidiKeyboardState internalState;
};
} // namespace detail

/** Vertical piano-key column docked to the LEFT edge of the
 *  PianoRollGrid.  Subclasses juce::MidiKeyboardComponent so it picks
 *  up the LCD-grey palette installed by LookAndFeel_E1 (style_v1.cpp
 *  registers MidiKeyboardComponent::ColourIds globally).
 *
 *  Orientation: verticalKeyboardFacingRight -- the playing surface
 *  faces RIGHT (toward the grid).  High pitches at the TOP, low at
 *  the bottom -- matches the convention from Ableton, Bitwig, FL, and
 *  Ardour.
 *
 *  Session 1 scope (paint-only):
 *   - No mouse interaction (clicks are absorbed but produce no sound).
 *   - Fixed visible pitch range 36 (C2) to 96 (C7), Bitwig default.
 *   - Owns its own MidiKeyboardState; it stays empty -- no notes
 *     light up.  Session 2+ may light the row corresponding to the
 *     pointer's hover Y, or echo selected notes from the grid.
 *
 *  Y-scroll lockstep with the PianoRollGrid is wired by PianoRollView
 *  in commit C; this class exposes setBaseNoteY / getKeyHeight so the
 *  grid can match its row height to the keyboard's. */
class PianoRollKeyboard : private detail::PianoRollKeyboardStateHolder,
                          public juce::MidiKeyboardComponent
{
public:
    PianoRollKeyboard();
    ~PianoRollKeyboard() override = default;

    /** First (highest pitch) visible MIDI note number.  Bitwig
     *  default = 96 (C7).  The keyboard's verticalKeyboardFacingRight
     *  orientation places this at the TOP of the column. */
    static constexpr int kDefaultLowestVisibleNote  = 36;  // C2
    static constexpr int kDefaultHighestVisibleNote = 96;  // C7

    /** Visible pitch range.  PianoRollGrid (commit C) reads this so
     *  its bar/beat grid uses the same per-pitch row height. */
    int getLowestVisibleNoteNumber() const noexcept;
    int getHighestVisibleNoteNumber() const noexcept;

    /** Vertical pixel height of one key row.  Used by the grid to
     *  align its note paint to the keyboard's key boundaries. */
    int getKeyRowHeight() const noexcept;

    /** Convert a MIDI pitch in the visible range to a Y coordinate
     *  in this component's local space (top edge of the key for
     *  `pitch`).  Returns getHeight() if pitch is below visible
     *  range, 0 if above. */
    int yForPitch (int pitch) const noexcept;

    /* State is held by the private detail::PianoRollKeyboardStateHolder
     * base (above) -- accessed via the inherited `state` member.  No
     * notes ever light up in Session 1; Session 3's playback layer
     * will broadcast via a separate channel. */

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollKeyboard)
};

} // namespace element
