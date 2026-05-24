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

    /** Border + icon-halo tint.  Same colour drives the outer stroke
     *  AND the 1-px outline around the icon glyph.  Pass a
     *  transparent colour to drop back to the neutral grey border
     *  + no halo. */
    void setTint (juce::Colour c) { tint_ = c; repaint(); }

    void paintButton (juce::Graphics& g, bool isOver, bool isDown) override
    {
        const float cornerSize = 3.0f;
        const bool active = getToggleState();
        const bool tinted = ! tint_.isTransparent();
        const bool hasActiveTint = ! activeTint_.isTransparent();

        const auto bodyOff = juce::Colour (0xff'2c'2c'2c);
        const auto bodyOnDefault = juce::Colour (0xff'3a'3a'3a);
        const auto neutralBorder = juce::Colour (0xff'5a'5a'5a);

        const juce::Colour bodyCol = active
            ? (hasActiveTint ? activeTint_ : bodyOnDefault)
            : bodyOff;
        const juce::Colour borderCol = tinted
            ? (active ? tint_ : tint_.withMultipliedBrightness (0.75f))
            : (active && hasActiveTint
                  ? activeTint_.brighter (0.20f)
                  : neutralBorder);

        auto box = getLocalBounds().reduced (1);
        const auto frect = box.toFloat();

        /* Subtle vertical gradient on the body -- top edge a hair
         * brighter than the bottom so the button reads as having
         * depth.  Matches the tracker EDIT / FOLLOW button family. */
        juce::ColourGradient bodyGrad (bodyCol.brighter (0.12f),
                                         frect.getX(), frect.getY(),
                                         bodyCol.darker (0.18f),
                                         frect.getX(), frect.getBottom(),
                                         false);
        g.setGradientFill (bodyGrad);
        g.fillRoundedRectangle (frect, cornerSize);

        /* Top 1-px highlight line for an even subtler "lit from
         * above" depth cue. */
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawLine (frect.getX() + 1.5f, frect.getY() + 1.0f,
                    frect.getRight() - 1.5f, frect.getY() + 1.0f, 1.0f);

        /* Outer border + inner dark ring (graph-block aesthetic). */
        g.setColour (borderCol);
        g.drawRoundedRectangle (frect, cornerSize, 1.2f);
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.drawRoundedRectangle (frect.reduced (1.2f, 1.2f),
                                juce::jmax (0.5f, cornerSize - 1.2f),
                                1.0f);

        const auto isLightTint = active && hasActiveTint
                                && activeTint_.getPerceivedBrightness() > 0.55f;
        const juce::Colour fg = active
            ? (isLightTint ? juce::Colour (0xff'10'10'10) : juce::Colours::white)
            : juce::Colour (0xff'd0'd0'd0);

        auto inner = box.reduced (4, 0);
        const bool hasLabel = label_.isNotEmpty();

        /* Icons get a 1-px coloured halo when the button carries a
         * tint -- stamp the drawer in the tint colour at the 4
         * cardinal offsets and overpaint in fg.  The 4 stamps render
         * a 1-px outline that exactly traces whatever silhouette the
         * icon draws (works for both filled paths + stroked paths). */
        auto drawIconWithHalo = [&] (juce::Rectangle<float> iconBox)
        {
            if (tinted)
            {
                const float dx[] = { -1.0f, 1.0f,  0.0f, 0.0f };
                const float dy[] = {  0.0f, 0.0f, -1.0f, 1.0f };
                for (int i = 0; i < 4; ++i)
                {
                    juce::Graphics::ScopedSaveState ss (g);
                    g.addTransform (juce::AffineTransform::translation (dx[i], dy[i]));
                    iconDrawer_ (g, iconBox, tint_);
                }
            }
            iconDrawer_ (g, iconBox, fg);
        };

        if (iconDrawer_)
        {
            if (hasLabel)
            {
                /* Icon-left, label-right.  Icon claims a square chunk
                 * sized to the button height; label fills remainder. */
                const int iconSide = juce::jmin (inner.getHeight() - 4,
                                                  juce::jmax (10, inner.getHeight() - 6));
                const auto iconBox = juce::Rectangle<int> (
                    inner.getX(),
                    inner.getY() + (inner.getHeight() - iconSide) / 2,
                    iconSide, iconSide).toFloat();
                drawIconWithHalo (iconBox);

                const auto textBox = inner.withTrimmedLeft (iconSide + 3);
                g.setColour (fg);
                const float fontPx = juce::jmin (11.0f, textBox.getHeight() * 0.55f);
                g.setFont (juce::FontOptions (juce::jmax (9.0f, fontPx), juce::Font::bold));
                g.drawText (label_, textBox, juce::Justification::centredLeft, false);
            }
            else
            {
                const int side = juce::jmin (inner.getWidth(), inner.getHeight());
                const int pad  = juce::jmax (3, side / 5);
                const int iconSide = juce::jmax (8, side - pad * 2);
                const auto iconBox = juce::Rectangle<int> (
                    inner.getX() + (inner.getWidth()  - iconSide) / 2,
                    inner.getY() + (inner.getHeight() - iconSide) / 2,
                    iconSide, iconSide).toFloat();
                drawIconWithHalo (iconBox);
            }
        }
        else
        {
            g.setColour (fg);
            const float fontPx = juce::jmin (12.0f, inner.getHeight() * 0.55f);
            g.setFont (juce::FontOptions (juce::jmax (9.0f, fontPx), juce::Font::bold));
            g.drawText (label_, inner, juce::Justification::centred);
        }

        /* Hover / pressed overlay on top of body + icon. */
        if (isDown)
        {
            g.setColour (juce::Colours::black.withAlpha (0.18f));
            g.fillRoundedRectangle (frect, cornerSize);
        }
        else if (isOver)
        {
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.fillRoundedRectangle (frect, cornerSize);
        }
    }

private:
    juce::String label_;
    juce::Colour tint_;
    juce::Colour activeTint_;
    IconDrawer   iconDrawer_;
};

} // namespace element
