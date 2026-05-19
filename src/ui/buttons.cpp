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
    /* Tracker-toolbar consistency: flat dark grey body, visible neutral
     * border that reads as bold against the dark toolbar background.
     * Toggle-on tints the body via `backgroundOnColourId` so
     * play/record can still show green/red, but as a body wash —
     * never as a coloured outer stroke. */
    const bool isOn = getToggleState();
    constexpr float cornerSize = 2.0f;
    const auto borderCol = Colour (0xff5a5a5a);

    Colour fill;
    if (isOn)
    {
        const auto onTint = findColour (backgroundOnColourId);
        fill = onTint.isTransparent()
                   ? Colour (0xff3a3a3a)
                   : onTint.withMultipliedSaturation (0.65f).withMultipliedBrightness (0.7f);
    }
    else
    {
        fill = Colour (0xff2c2c2c);
    }

    auto box = getLocalBounds().reduced (1);
    g.setColour (fill);
    g.fillRoundedRectangle (box.toFloat(), cornerSize);

    g.setColour (borderCol);
    g.drawRoundedRectangle (box.toFloat(), cornerSize, 1.0f);

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
        g.setColour (Colours::white.withAlpha (0.10f));
        g.fillRoundedRectangle (box.toFloat(), cornerSize);
    }
    else if (isMouseOverButton)
    {
        g.setColour (Colours::white.withAlpha (0.05f));
        g.fillRoundedRectangle (box.toFloat(), cornerSize);
    }
}

} // namespace element
