// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>
#include <deque>

namespace element {

/* Cached default-monospaced Font lookup keyed by (height, style).
 * Replaces the per-paint
 *   juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), N, S)
 * pattern that fires in arrangement / session / sampler / tracker
 * paint hot paths.  First call at each (h, s) caches the resulting
 * Font; subsequent calls are an O(N) scan of the cache (N ~10 in
 * practice across the whole UI).
 *
 * std::deque is deliberate: push_back preserves references to existing
 * elements, so callers may hold the returned `const Font&` across
 * additional monoFont() calls without lifetime concerns.  juce
 * ::Graphics::setFont copies the Font internally regardless.
 *
 * Single-threaded: paint / resized / timerCallback all run on the
 * MessageManager thread, so no lock is needed.  The first call at any
 * (height, style) happens after JUCE init since paint() can only fire
 * once a window is on the desktop, so the lazy-static juce::Font
 * construction is safe. */
inline const juce::Font& monoFont (float height, int style = juce::Font::plain)
{
    struct Entry { float h; int style; juce::Font font; };
    static std::deque<Entry> cache;
    for (auto& e : cache)
        if (e.h == height && e.style == style) return e.font;
    cache.push_back ({ height, style,
        juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                       height, style)) });
    return cache.back().font;
}

} // namespace element
