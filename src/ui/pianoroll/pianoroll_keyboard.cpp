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

    /* Keyboard palette inherits the global LookAndFeel scheme
     * (style_v1.cpp -- MidiKeyboardComponent::ColourIds set by
     * LookAndFeel_E1).  Same colours as the transport's virtual
     * keyboard + the sampler keyboard so all three read as one
     * family.  Do NOT override locally -- previous attempts to
     * "boost contrast" by lightening the white keys produced a
     * near-white strip in vertical orientation that obliterated
     * the black-key shape. */

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
    /* Derived from the live JUCE low-bound + cached span.  Clamped to
     * kAbsoluteHighestNote so the result stays meaningful at the edge
     * (when the user has scrolled all the way up). */
    const int lo = (int) getLowestVisibleKey();
    return juce::jmin (kAbsoluteHighestNote, lo + cachedSpan_ - 1);
}

void PianoRollKeyboard::setVisibleNoteRange (int lo, int hi)
{
    lo = juce::jlimit (kAbsoluteLowestNote, kAbsoluteHighestNote, lo);
    hi = juce::jlimit (kAbsoluteLowestNote, kAbsoluteHighestNote, hi);
    if (hi - lo < kMinPitchSpan)
        hi = juce::jmin (kAbsoluteHighestNote, lo + kMinPitchSpan);
    if (hi - lo < kMinPitchSpan)
        lo = juce::jmax (kAbsoluteLowestNote,  hi - kMinPitchSpan);

    const int newSpan = hi - lo + 1;
    if (lo == (int) getLowestVisibleKey() && newSpan == cachedSpan_) return;

    cachedSpan_ = newSpan;
    /* Re-clamp JUCE's internal lo bound so its built-in scroll arrows
     * (and any direct setLowestVisibleKey call) can't push lo past
     * the point where lo + span - 1 would exceed kAbsoluteHighestNote.
     * Without this, scrolling to the top edge would shrink the visible
     * window (lo creeps past 127 - span + 1 and getHighest clamps
     * down), which reads as a zoom-in at the boundary. */
    const int maxLowestKey = juce::jmax (kAbsoluteLowestNote,
                                            kAbsoluteHighestNote - cachedSpan_ + 1);
    setAvailableRange (kAbsoluteLowestNote, maxLowestKey);
    setLowestVisibleKey (lo);

    /* Adjust per-key extent so the visible range fills the component
     * height.  In verticalKeyboardFacingRight orientation, setKeyWidth
     * controls the per-key Y extent.  If the component isn't laid
     * out yet (height == 0) the next resized() pass will recompute. */
    const int h = juce::jmax (1, getHeight());
    setKeyWidth (juce::jmax (3.0f, (float) h / (float) cachedSpan_));
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
     * new height.  Using cachedSpan_ directly (rather than deriving
     * span from getHighestVisibleNoteNumber()) keeps the per-key
     * extent stable while the user scrolls -- the span is invariant
     * across a scroll, only lo moves. */
    const int span = juce::jmax (1, cachedSpan_);
    const int h    = juce::jmax (1, getHeight());
    setKeyWidth (juce::jmax (3.0f, (float) h / (float) span));
    juce::MidiKeyboardComponent::resized();
}

void PianoRollKeyboard::drawBlackNote (int /*midiNoteNumber*/,
                                         juce::Graphics& g,
                                         juce::Rectangle<float> area,
                                         bool isDown, bool isOver,
                                         juce::Colour noteFillColour)
{
    /* Flat fill, no JUCE "lit-from-above" inner highlight cap.  Same
     * override as VirtualKeyboardComponent: under our dark theme the
     * default highlight reads as grey-on-grey and obliterates the
     * black-key silhouette.  Down/over overlays still composite via
     * the LookAndFeel ColourIds the host registered. */
    auto c = noteFillColour;
    if (isDown) c = c.overlaidWith (findColour (keyDownOverlayColourId));
    if (isOver) c = c.overlaidWith (findColour (mouseOverKeyOverlayColourId));
    g.setColour (c);
    g.fillRect (area);
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
