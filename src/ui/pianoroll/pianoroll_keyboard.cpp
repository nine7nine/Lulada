// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/pianoroll/pianoroll_keyboard.hpp"

namespace element {

PianoRollKeyboard::PianoRollKeyboard()
    : detail::PianoRollKeyboardStateHolder(),
      juce::MidiKeyboardComponent (internalState,
                                     juce::MidiKeyboardComponent::verticalKeyboardFacingRight)
{
    /* Base init order (left-to-right): PianoRollKeyboardStateHolder
     * runs first, so `internalState` is fully constructed by the
     * time MidiKeyboardComponent::ctor calls state.addListener(this).
     * See the class comment for the rationale. */
    /* Fixed visible range, Bitwig default.  Session 2 will likely
     * make this user-zoomable; for now the static range keeps the
     * paint loop cheap (~60 keys regardless of dock height). */
    setAvailableRange (kDefaultLowestVisibleNote, kDefaultHighestVisibleNote);
    setLowestVisibleKey (kDefaultLowestVisibleNote);

    /* JUCE's keyboard exposes a setter for octave label visibility on
     * vertical orientations; we want the C octave labels visible so
     * users can navigate by ear.  Default style + colour come from
     * LookAndFeel_E1 (style_v1.cpp -- MidiKeyboardComponent::
     * ColourIds set globally). */
    setOctaveForMiddleC (4);
    setKeyWidth (12.0f);  /* "width" here is the per-key extent along
                            * the keyboard's primary axis; in vertical
                            * orientation that's the per-key HEIGHT. */

    /* No mouse interaction in Session 1.  We don't disable the
     * component entirely -- that would dim it via the LookAndFeel
     * disabled colour.  Instead absorb clicks via setEnabled (true)
     * but ignore them at the MidiKeyboardComponent layer; Session 2
     * may add hover-to-preview audition. */
    setMouseClickGrabsKeyboardFocus (false);
}

int PianoRollKeyboard::getLowestVisibleNoteNumber() const noexcept
{
    return (int) getLowestVisibleKey();
}

int PianoRollKeyboard::getHighestVisibleNoteNumber() const noexcept
{
    /* JUCE's MidiKeyboardComponent doesn't expose a direct getter for
     * the highest visible note; derive from the fixed available range
     * since we never call setAvailableRange after construction. */
    return kDefaultHighestVisibleNote;
}

int PianoRollKeyboard::getKeyRowHeight() const noexcept
{
    /* In vertical orientation, "key width" is per-key Y extent. */
    return (int) getKeyWidth();
}

int PianoRollKeyboard::yForPitch (int pitch) const noexcept
{
    const int lo = getLowestVisibleNoteNumber();
    const int hi = getHighestVisibleNoteNumber();
    if (pitch < lo) return getHeight();
    if (pitch > hi) return 0;

    /* verticalKeyboardFacingRight -- high pitches at the TOP, low at
     * the BOTTOM.  Each pitch occupies kKeyRowHeight() pixels.
     * Returns the Y coordinate of the TOP edge of the key for pitch. */
    const int rowH = getKeyRowHeight();
    return (hi - pitch) * rowH;
}

} // namespace element
