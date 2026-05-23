// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>

namespace element {
namespace ui {

/* Shared lane / track / clip colour palette.
 *
 *  Consumers:
 *   - Tracker editor track headers + per-channel highlights
 *   - Arrangement view lane headers + region body tints
 *   - Session view clip cells (planned)
 *
 *  16 hues stepped around the wheel, lightness picked so adjacent
 *  indices read as clearly different even on Element's dark
 *  background.  Indexed via modulo; cycles cleanly past 16 lanes. */
inline const juce::Colour kLanePalette[] = {
    juce::Colour { 0xff'd4'5a'5a },   // 0  red
    juce::Colour { 0xff'd4'85'4a },   // 1  orange
    juce::Colour { 0xff'd4'a5'4a },   // 2  amber
    juce::Colour { 0xff'c5'c5'4a },   // 3  yellow
    juce::Colour { 0xff'a5'c5'4a },   // 4  chartreuse
    juce::Colour { 0xff'6a'c5'5a },   // 5  green
    juce::Colour { 0xff'4a'c5'7a },   // 6  emerald
    juce::Colour { 0xff'4a'c5'a5 },   // 7  teal
    juce::Colour { 0xff'4a'b5'c5 },   // 8  cyan
    juce::Colour { 0xff'5a'95'c5 },   // 9  sky
    juce::Colour { 0xff'5a'7a'd4 },   // 10 blue
    juce::Colour { 0xff'7a'5a'd4 },   // 11 indigo
    juce::Colour { 0xff'9a'5a'd4 },   // 12 violet
    juce::Colour { 0xff'c5'5a'd4 },   // 13 magenta
    juce::Colour { 0xff'd4'5a'a5 },   // 14 pink
    juce::Colour { 0xff'd4'5a'75 },   // 15 rose
};

inline constexpr int kLanePaletteSize =
    (int) (sizeof (kLanePalette) / sizeof (kLanePalette[0]));

inline juce::Colour laneTint (int idx) noexcept
{
    return kLanePalette[((unsigned) idx) % (unsigned) kLanePaletteSize];
}

} // namespace ui
} // namespace element
