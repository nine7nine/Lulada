// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>
#include <element/juce/data_structures.hpp>
#include <element/node.hpp>
#include <element/services.hpp>
#include <element/ui/content.hpp>

#define EL_VIEW_TRACKER_HOST "TrackerHostView"

namespace element {

class TrackerNode;

/** Main-window host that aggregates every TrackerNode in the active
 *  graph into a tabbed view, each tab containing the existing
 *  TrackerEditor for that tracker.  Auto-rescans when nodes are added
 *  or removed via a ValueTree::Listener on the active graph. */
class TrackerHostView : public ContentView,
                        private juce::ValueTree::Listener
{
public:
    TrackerHostView();
    ~TrackerHostView() override;

    void initializeView (Services&) override;
    void didBecomeActive() override;
    void willBeRemoved() override;
    void stabilizeContent() override;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void valueTreeChildAdded   (juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override {}
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override {}
    void valueTreeParentChanged (juce::ValueTree&) override {}

    void rescan();
    void attachToActiveGraph();
    void detachFromActiveGraph();

    Services* services_ = nullptr;
    juce::TabbedComponent tabs_ { juce::TabbedButtonBar::TabsAtTop };
    juce::ValueTree attachedGraphTree_;
};

} // namespace element
