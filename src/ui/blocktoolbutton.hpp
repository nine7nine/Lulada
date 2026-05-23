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
    using IconDrawer = std::function<void (juce::Graphics&,
                                            juce::Rectangle<float>,
                                            juce::Colour /*fg*/)>;

    BlockToolButton (const juce::String& label,
                     juce::Colour tint = juce::Colour())
        : juce::Button (label), label_ (label), tint_ (tint) {}

    void setLabel (const juce::String& l)
    {
        if (label_ == l) return;
        label_ = l;
        repaint();
    }

    /** Optional vector icon drawer.  When set, the button paints the
     *  icon on the left and the label to its right; otherwise the
     *  label centres in the body.  Pass a lambda that strokes/fills
     *  inside the given rect using the foreground colour the host
     *  has chosen for this button state. */
    void setIcon (IconDrawer drawer) { iconDrawer_ = std::move (drawer); repaint(); }

    /** Optional override for the active-state body fill.  When set,
     *  the on-toggle state paints a saturated tint (matches tracker's
     *  FOLLOW button "go green" pattern) instead of the subtle dark-
     *  grey-brightening default. */
    void setActiveTint (juce::Colour c) { activeTint_ = c; repaint(); }

    void paintButton (juce::Graphics& g, bool isOver, bool isDown) override
    {
        const float cornerSize = 2.0f;
        const bool active = getToggleState();
        const bool tinted = ! tint_.isTransparent();
        const bool hasActiveTint = ! activeTint_.isTransparent();

        const auto bodyOff = juce::Colour (0xff2c2c2c);
        const auto bodyOnDefault = juce::Colour (0xff3a3a3a);
        const auto neutralBorder = juce::Colour (0xff8a8a8a);

        const juce::Colour bodyCol = active
            ? (hasActiveTint ? activeTint_ : bodyOnDefault)
            : bodyOff;
        const juce::Colour borderCol = tinted
            ? (active ? tint_ : tint_.withMultipliedBrightness (0.85f))
            : (active && hasActiveTint
                  ? activeTint_.brighter (0.25f)
                  : neutralBorder);

        auto box = getLocalBounds().reduced (1);

        g.setColour (bodyCol);
        g.fillRoundedRectangle (box.toFloat(), cornerSize);

        g.setColour (borderCol);
        g.drawRoundedRectangle (box.toFloat(), cornerSize, 1.5f);

        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (box.toFloat().reduced (1.5f, 1.5f),
                                juce::jmax (0.5f, cornerSize - 1.5f),
                                1.0f);

        /* Foreground colour: tracker's white-on / 0xffd0d0d0 off
         * pattern, with an exception for active-with-tint where we
         * use near-black so the colour pops. */
        const auto isLightTint = active && hasActiveTint
                                && activeTint_.getPerceivedBrightness() > 0.5f;
        const juce::Colour fg = active
            ? (isLightTint ? juce::Colour (0xff'10'10'10) : juce::Colours::white)
            : juce::Colour (0xffd0d0d0);

        auto inner = box.reduced (4, 0);

        if (iconDrawer_)
        {
            /* Icon-left, label-right.  Icon claims a square chunk
             * sized to the button height; label fills the remainder. */
            const int iconSide = juce::jmin (inner.getHeight() - 2,
                                              juce::jmax (10, inner.getHeight() - 4));
            const auto iconBox = juce::Rectangle<int> (inner.getX(),
                                                        inner.getY() + (inner.getHeight() - iconSide) / 2,
                                                        iconSide, iconSide).toFloat();
            iconDrawer_ (g, iconBox, fg);

            const auto textBox = inner.withTrimmedLeft (iconSide + 3);
            g.setColour (fg);
            const float fontPx = juce::jmin (11.0f, textBox.getHeight() * 0.58f);
            g.setFont (juce::FontOptions (juce::jmax (9.0f, fontPx), juce::Font::bold));
            g.drawText (label_, textBox, juce::Justification::centredLeft, false);
        }
        else
        {
            g.setColour (fg);
            const float fontPx = juce::jmin (12.0f, inner.getHeight() * 0.62f);
            g.setFont (juce::FontOptions (juce::jmax (9.0f, fontPx), juce::Font::bold));
            g.drawText (label_, inner, juce::Justification::centred);
        }

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
    juce::Colour activeTint_;
    IconDrawer   iconDrawer_;
};

} // namespace element
