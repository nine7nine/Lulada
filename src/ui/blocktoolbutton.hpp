// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>
#include <element/ui/style.hpp>

namespace element {

/** Toolbar button styled like a miniature graph block.
 *
 *  - Bold coloured outer border (the "type tint") + dark inner inset,
 *    matching `BlockComponent::paint` (see `src/ui/block.cpp`).
 *  - Body fills with widgetBackgroundColor (slightly brightened when
 *    toggled on), glyph text centred.
 *  - No top "color strip" — bold outer border carries the colour cue.
 *  - When `tint` is transparent, the border falls back to a neutral
 *    dark grey so the button blends with the toolbar without claiming
 *    a colour identity (use for generic Play / Stop / Rescan / etc).
 *
 *  Density target: same as the tracker editor toolbar — compact, no
 *  per-button padding, glyph font scales with button height. */
class BlockToolButton : public juce::Button
{
public:
    BlockToolButton (const juce::String& label,
                     juce::Colour tint = juce::Colour())
        : juce::Button (label), label_ (label), tint_ (tint) {}

    void setLabel (const juce::String& l)
    {
        if (label_ == l) return;
        label_ = l;
        repaint();
    }

    void paintButton (juce::Graphics& g, bool isOver, bool isDown) override
    {
        /* Graph-block consistency: matches BlockComponent::paint
         * (block.cpp:919-925) — full-saturation type-tint outer
         * stroke at 1.5px + inner 0.6α-black ring at 1.0px for
         * definition.  Untinted buttons get a bright neutral border
         * so the entire main toolbar reads as a row of mini-blocks,
         * not a row of soft pillboxes. */
        const float cornerSize = 2.0f;
        const bool active = getToggleState();
        const bool tinted = ! tint_.isTransparent();

        const auto bodyOff = juce::Colour (0xff2c2c2c);
        const auto bodyOn  = juce::Colour (0xff3a3a3a);
        const auto neutralBorder = juce::Colour (0xff8a8a8a);

        const juce::Colour bodyCol = active ? bodyOn : bodyOff;
        const juce::Colour borderCol = tinted
            ? (active ? tint_ : tint_.withMultipliedBrightness (0.85f))
            : neutralBorder;

        auto box = getLocalBounds().reduced (1);

        g.setColour (bodyCol);
        g.fillRoundedRectangle (box.toFloat(), cornerSize);

        /* Outer bold stroke — full type-tint, 1.5px (matches block.cpp:920). */
        g.setColour (borderCol);
        g.drawRoundedRectangle (box.toFloat(), cornerSize, 1.5f);

        /* Inner dark ring — same trick block.cpp:923-925 uses to make
         * the bold outer stroke pop without bleeding into the body. */
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (box.toFloat().reduced (1.5f, 1.5f),
                                juce::jmax (0.5f, cornerSize - 1.5f),
                                1.0f);

        /* Glyph — tracker uses 0xffd0d0d0 off / white on; same here. */
        g.setColour (active ? juce::Colours::white : juce::Colour (0xffd0d0d0));
        const float fontPx = juce::jmin (12.0f, box.getHeight() * 0.62f);
        g.setFont (juce::FontOptions (juce::jmax (9.0f, fontPx), juce::Font::bold));
        g.drawText (label_, box, juce::Justification::centred);

        if (isDown)
        {
            g.setColour (juce::Colours::white.withAlpha (0.08f));
            g.fillRoundedRectangle (box.toFloat(), cornerSize);
        }
        else if (isOver)
        {
            g.setColour (juce::Colours::white.withAlpha (0.04f));
            g.fillRoundedRectangle (box.toFloat(), cornerSize);
        }
    }

private:
    juce::String label_;
    juce::Colour tint_;
};

} // namespace element
