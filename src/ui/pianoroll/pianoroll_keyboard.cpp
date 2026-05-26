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
    setAvailableRange (kAbsoluteLowestNote, kAbsoluteHighestNote);
    setVisibleNoteRange (kDefaultLowestVisibleNote, kDefaultHighestVisibleNote);

    /* JUCE's keyboard exposes a setter for octave label visibility on
     * vertical orientations; we want the C octave labels visible so
     * users can navigate by ear. */
    setOctaveForMiddleC (4);
    setKeyWidth (12.0f);  /* "width" here is the per-key extent along
                            * the keyboard's primary axis; in vertical
                            * orientation that's the per-key HEIGHT. */

    /* Override Element's LookAndFeel keyboard palette LOCALLY for the
     * piano-roll instance only -- the global LCD-grey scheme works on
     * the virtual keyboard up top but reads as low-contrast in the
     * piano-roll dock context (the dock background is darker, so the
     * dark-grey white keys disappear into it).  Bring contrast back
     * by giving white keys a proper light-grey body and black keys a
     * deep black with a brighter top edge.  Mirrors Ableton + Zrythm
     * keyboard column conventions. */
    setColour (juce::MidiKeyboardComponent::whiteNoteColourId,
                juce::Colour (0xff'd8'd8'd8));
    setColour (juce::MidiKeyboardComponent::blackNoteColourId,
                juce::Colour (0xff'08'08'08));
    setColour (juce::MidiKeyboardComponent::keySeparatorLineColourId,
                juce::Colour (0xff'18'18'18));
    setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId,
                juce::Colour (0x60'80'a0'c0));
    setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId,
                juce::Colour (0xa0'5a'be'e5));
    setColour (juce::MidiKeyboardComponent::textLabelColourId,
                juce::Colour (0xff'30'30'30));   /* dark text on light keys */
    setColour (juce::MidiKeyboardComponent::shadowColourId,
                juce::Colour (0x66'00'00'00));

    /* No mouse interaction in Session 1.  We don't disable the
     * component entirely -- that would dim it via the LookAndFeel
     * disabled colour.  Instead absorb clicks via setEnabled (true)
     * but ignore them at the MidiKeyboardComponent layer; Session 2+
     * may add hover-to-preview audition. */
    setMouseClickGrabsKeyboardFocus (false);
}

int PianoRollKeyboard::getLowestVisibleNoteNumber() const noexcept
{
    return (int) getLowestVisibleKey();
}

int PianoRollKeyboard::getHighestVisibleNoteNumber() const noexcept
{
    /* Computed at setVisibleNoteRange time so the read is O(1). */
    return cachedHighest_;
}

void PianoRollKeyboard::setVisibleNoteRange (int lo, int hi)
{
    lo = juce::jlimit (kAbsoluteLowestNote, kAbsoluteHighestNote, lo);
    hi = juce::jlimit (kAbsoluteLowestNote, kAbsoluteHighestNote, hi);
    if (hi - lo < kMinPitchSpan)
        hi = juce::jmin (kAbsoluteHighestNote, lo + kMinPitchSpan);
    if (hi - lo < kMinPitchSpan)
        lo = juce::jmax (kAbsoluteLowestNote,  hi - kMinPitchSpan);

    if (lo == (int) getLowestVisibleKey() && hi == cachedHighest_) return;

    cachedHighest_ = hi;
    setLowestVisibleKey (lo);

    /* Adjust per-key extent so the visible range fills the component
     * height.  In verticalKeyboardFacingRight orientation, setKeyWidth
     * controls the per-key Y extent.  If the component isn't laid
     * out yet (height == 0) the next resized() pass will recompute. */
    const int span = hi - lo + 1;
    const int h    = juce::jmax (1, getHeight());
    if (span > 0)
        setKeyWidth (juce::jmax (3.0f, (float) h / (float) span));
    repaint();
}

void PianoRollKeyboard::zoomVertically (double factor)
{
    if (factor <= 0.0) return;
    const int lo = getLowestVisibleNoteNumber();
    const int hi = getHighestVisibleNoteNumber();
    const double centre = ((double) lo + (double) hi) * 0.5;
    const double span   = juce::jmax (1.0, ((double) (hi - lo)) * factor);
    int newLo = (int) std::round (centre - span * 0.5);
    int newHi = (int) std::round (centre + span * 0.5);
    setVisibleNoteRange (newLo, newHi);
}

void PianoRollKeyboard::shiftVisibleRange (int deltaSemitones)
{
    if (deltaSemitones == 0) return;
    const int lo = getLowestVisibleNoteNumber();
    const int hi = getHighestVisibleNoteNumber();
    const int span = hi - lo;
    int newLo = juce::jlimit (kAbsoluteLowestNote,
                                kAbsoluteHighestNote - span,
                                lo + deltaSemitones);
    setVisibleNoteRange (newLo, newLo + span);
}

int PianoRollKeyboard::getKeyRowHeight() const noexcept
{
    /* In vertical orientation, "key width" is per-key Y extent. */
    return (int) getKeyWidth();
}

void PianoRollKeyboard::resized()
{
    /* Recompute per-key Y extent so the visible pitch range fills the
     * new height.  In verticalKeyboardFacingRight orientation,
     * setKeyWidth controls the per-key Y extent. */
    const int lo = getLowestVisibleNoteNumber();
    const int hi = getHighestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    const int h    = juce::jmax (1, getHeight());
    setKeyWidth (juce::jmax (3.0f, (float) h / (float) span));
    juce::MidiKeyboardComponent::resized();
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
