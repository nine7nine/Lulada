// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>
#include <element/ui/content.hpp>

#define EL_VIEW_SESSION_VIEW "SessionView"

namespace element {

/** Bitwig/Ableton-style clip-grid launcher view.
 *
 *  Phase 0 is a stub: empty pane wired into the View menu and command
 *  table.  Subsequent phases add the data model, the per-TrackerNode
 *  session-view API, the grid UI, quantised launching, scenes, follow
 *  actions, and the per-column mixer strip.
 *
 *  Design + cookbook: ~/wine-nspa-notes/session-view-design.md
 */
class SessionView : public ContentView
{
public:
    SessionView();
    ~SessionView() override;

    void paint (juce::Graphics&) override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SessionView)
};

} // namespace element
