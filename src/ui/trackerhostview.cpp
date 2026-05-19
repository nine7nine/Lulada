// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/trackerhostview.hpp"

#include <element/context.hpp>
#include <element/session.hpp>
#include <element/tags.hpp>
#include <element/ui/style.hpp>

#include "nodes/tracker.hpp"
#include "nodes/trackereditor.hpp"

namespace element {

using juce::Component;
using juce::Graphics;
using juce::String;

TrackerHostView::TrackerHostView()
{
    setName (EL_VIEW_TRACKER_HOST);
    addAndMakeVisible (tabs_);
    tabs_.setOutline (0);
    tabs_.setTabBarDepth (28);
}

TrackerHostView::~TrackerHostView()
{
    detachFromActiveGraph();
    tabs_.clearTabs();
}

void TrackerHostView::initializeView (Services& s)
{
    services_ = &s;
}

void TrackerHostView::didBecomeActive()
{
    attachToActiveGraph();
    rescan();
}

void TrackerHostView::willBeRemoved()
{
    detachFromActiveGraph();
}

void TrackerHostView::stabilizeContent()
{
    attachToActiveGraph();
    rescan();
}

void TrackerHostView::resized()
{
    tabs_.setBounds (getLocalBounds());
}

void TrackerHostView::paint (Graphics& g)
{
    g.fillAll (Colors::contentBackgroundColor);
}

/* ===================================================================== */

static void collectTrackerNodes (const Node& graph, juce::Array<Node>& out)
{
    const int n = graph.getNumNodes();
    for (int i = 0; i < n; ++i)
    {
        Node child = graph.getNode (i);
        if (! child.isValid()) continue;
        if (auto* proc = child.getObject())
        {
            if (dynamic_cast<TrackerNode*> (proc) != nullptr)
            {
                out.add (child);
                continue;
            }
        }
        if (child.isGraph())
            collectTrackerNodes (child, out);
    }
}

void TrackerHostView::rescan()
{
    /* Preserve current tab index so adding a new tracker doesn't yank
     * the user off the one they were editing. */
    const int prevIdx = tabs_.getCurrentTabIndex();

    tabs_.clearTabs();

    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    auto graph = sess->getActiveGraph();
    if (! graph.isValid()) return;

    juce::Array<Node> trackers;
    collectTrackerNodes (graph, trackers);

    for (int i = 0; i < trackers.size(); ++i)
    {
        const auto& n = trackers.getReference (i);
        auto* ed = new TrackerEditor (n);
        const String label = n.getName().isNotEmpty() ? n.getName() : String ("Tracker ") + String (i + 1);
        tabs_.addTab (label, Colors::widgetBackgroundColor.darker(), ed, true);
    }

    if (tabs_.getNumTabs() > 0)
        tabs_.setCurrentTabIndex (juce::jlimit (0, tabs_.getNumTabs() - 1, prevIdx));
}

void TrackerHostView::attachToActiveGraph()
{
    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    auto graph = sess->getActiveGraph();
    if (! graph.isValid()) return;
    auto tree = graph.data();
    if (tree == attachedGraphTree_) return;

    detachFromActiveGraph();
    attachedGraphTree_ = tree;
    if (attachedGraphTree_.isValid())
        attachedGraphTree_.addListener (this);
}

void TrackerHostView::detachFromActiveGraph()
{
    if (attachedGraphTree_.isValid())
        attachedGraphTree_.removeListener (this);
    attachedGraphTree_ = juce::ValueTree();
}

void TrackerHostView::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child)
{
    if (child.hasType (types::Node) || child.hasType (tags::nodes))
        rescan();
}

void TrackerHostView::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    if (child.hasType (types::Node) || child.hasType (tags::nodes))
        rescan();
}

} // namespace element
