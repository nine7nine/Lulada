// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/buttons.hpp"

namespace element {

IconButton::IconButton (const String& buttonName)
    : Button (buttonName) {}
IconButton::~IconButton() {}

void IconButton::setIcon (Icon newIcon, float reduceSize)
{
    icon = newIcon;
    repaint();
}

void IconButton::paintButton (Graphics& g, bool isMouseOverButton, bool isButtonDown)
{
    getLookAndFeel().drawButtonBackground (g, *this, findColour (getToggleState() ? TextButton::buttonOnColourId : TextButton::buttonColourId), isMouseOverButton, isButtonDown);
    Rectangle<float> bounds (0.f, 0.f, (float) jmin (getWidth(), getHeight()), (float) jmin (getWidth(), getHeight()));
    icon.colour = isEnabled() ? Colors::textColor : Colors::textColor.darker();
    icon.draw (g, bounds.reduced (iconReduceSize), false);
}

void SettingButton::paintButton (Graphics& g, bool isMouseOverButton, bool isButtonDown)
{
    /* Modernised flat-panel paint -- matches BlockToolButton family.
     * Subtle vertical gradient (top brighter, bottom darker) + 1-px
     * highlight line for "lit from above" depth cue.  Toggle-on
     * tints the body via backgroundOnColourId so Play / Record can
     * still flash green / red as a body wash. */
    const bool isOn = getToggleState();
    constexpr float cornerSize = 3.0f;
    const auto borderCol = Colour (0xff'5a'5a'5a);

    Colour fill;
    if (isOn)
    {
        const auto onTint = findColour (backgroundOnColourId);
        fill = onTint.isTransparent()
                   ? Colour (0xff'3a'3a'3a)
                   : onTint;
    }
    else
    {
        fill = Colour (0xff'2c'2c'2c);
    }

    auto box = getLocalBounds().reduced (1);
    const auto frect = box.toFloat();

    juce::ColourGradient bodyGrad (fill.brighter (0.12f),
                                     frect.getX(), frect.getY(),
                                     fill.darker (0.18f),
                                     frect.getX(), frect.getBottom(),
                                     false);
    g.setGradientFill (bodyGrad);
    g.fillRoundedRectangle (frect, cornerSize);

    /* Top highlight line. */
    g.setColour (Colours::white.withAlpha (0.06f));
    g.drawLine (frect.getX() + 1.5f, frect.getY() + 1.0f,
                frect.getRight() - 1.5f, frect.getY() + 1.0f, 1.0f);

    g.setColour (borderCol);
    g.drawRoundedRectangle (frect, cornerSize, 1.2f);
    g.setColour (Colours::black.withAlpha (0.55f));
    g.drawRoundedRectangle (frect.reduced (1.2f, 1.2f),
                            juce::jmax (0.5f, cornerSize - 1.2f),
                            1.0f);

    if (! path.isEmpty())
    {
        const Colour iconCol = iconColour.isTransparent()
            ? getTextColour().brighter (0.15f)
            : (isEnabled() ? iconColour : iconColour.withMultipliedBrightness (0.55f));
        Icon i (path, iconCol);
        Rectangle<float> r { 0.0, 0.0, (float) getWidth(), (float) getHeight() };
        i.draw (g, r.reduced (pathReduction), false);
    }
    else if (icon.isNull() || ! icon.isValid())
    {
        String text = getButtonText();

        if (text.isEmpty() && getClickingTogglesState())
            text = (getToggleState()) ? yes : no;
        g.setFont (12.f);
        g.setColour (getTextColour());
        g.drawText (text, getLocalBounds(), Justification::centred);
    }
    else
    {
        const Rectangle<int> area (0, 0, getWidth(), getHeight());
        g.drawImage (icon, area.reduced (2).toFloat(), RectanglePlacement::onlyReduceInSize);
    }

    /* Hover / pressed overlay applied last so it sits on top of icon
     * + text without recolouring them via fill alpha. */
    if (isButtonDown)
    {
        g.setColour (Colours::black.withAlpha (0.18f));
        g.fillRoundedRectangle (frect, cornerSize);
    }
    else if (isMouseOverButton)
    {
        g.setColour (Colours::white.withAlpha (0.06f));
        g.fillRoundedRectangle (frect, cornerSize);
    }
}

} // namespace element
