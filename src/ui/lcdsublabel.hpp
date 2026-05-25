// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>
#include "ui/fontcache.hpp"

namespace element {

/** Tiny LCD-styled sub-label that sits under a toolbar icon button.
 *  Dark recess + blue LCD text matching MainDisplayPanel's BPM /
 *  position readouts so the whole toolbar reads as one instrument-
 *  cluster style.
 *
 *  Currently decorative (paint-only).  Wired as the visual surface
 *  for spill-over menu triggers in a follow-up -- when the legacy
 *  top menubar is removed, clicking a sublabel will open a menu
 *  containing the per-button extended actions. */
class LcdSublabel : public juce::Component
{
public:
    explicit LcdSublabel (const juce::String& text) : text_ (text) {}

    void setText (const juce::String& t)
    {
        if (text_ == t) return;
        text_ = t;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour (juce::Colour (0xff'0e'12'16));
        g.fillRoundedRectangle (bounds, 2.5f);
        g.setColour (juce::Colour (0xff'2a'30'36));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 2.5f, 0.75f);
        g.setColour (juce::Colour (0xff'5a'be'e5));
        g.setFont (monoFont (8.5f, juce::Font::bold));
        g.drawText (text_, getLocalBounds(), juce::Justification::centred);
    }

private:
    juce::String text_;
};

} // namespace element
