// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/sessionview.hpp"

#include <element/ui/style.hpp>

namespace element {

SessionView::SessionView()
{
    setName (EL_VIEW_SESSION_VIEW);
    setOpaque (true);
}

SessionView::~SessionView() = default;

void SessionView::paint (juce::Graphics& g)
{
    g.fillAll (Colors::contentBackgroundColor);
    g.setColour (Colors::textColor.withAlpha (0.55f));
    g.setFont (juce::FontOptions (14.0f));
    g.drawText ("Session view — coming soon",
                getLocalBounds(),
                juce::Justification::centred);
}

} // namespace element
