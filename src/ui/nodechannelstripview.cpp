// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <element/services.hpp>
#include <element/ui.hpp>
#include <element/ui/style.hpp>
#include <element/session.hpp>
#include <element/context.hpp>
#include <element/signals.hpp>

#include "ui/nodechannelstripview.hpp"
#include "ui/nodechannelstrip.hpp"

namespace element {

class NodeChannelStripView::Content : public NodeChannelStripComponent
{
public:
    Content (GuiService& g)
        : NodeChannelStripComponent (g)
    {
        // Channel-name label pops in white so it stays legible against
        // the type-tinted strip background regardless of node format.
        setNodeNameColour (Colours::white);
        bindSignals();
        _conns.push_back (g.sibling<SessionService>()->sigSessionLoaded.connect ([this, &g]() {
            auto graph = g.session()->getActiveGraph();
            setNode (graph.getNode (0));
            g.selectNode (graph.getNode (0));
        }));
    }

    ~Content()
    {
        for (auto& c : _conns)
            c.disconnect();
        _conns.clear();
        unbindSignals();
    }

    void paint (Graphics& g) override
    {
        // Match the mixer strip — subdued bg with a low-alpha tint of
        // the node-type accent.  This view always shows the active
        // node, so no selection state is layered on top.
        const auto accent = colorForNode (getNode());
        const auto base   = Colors::widgetBackgroundColor.darker (0.45f);
        g.setColour (base.interpolatedWith (accent, 0.07f));
        g.fillAll();
        g.setColour (Colors::contentBackgroundColor);
        g.drawLine (0.0, 0.0, 0.0, getHeight());
    }

    std::vector<boost::signals2::connection> _conns;
};

NodeChannelStripView::NodeChannelStripView()
{
}

NodeChannelStripView::~NodeChannelStripView()
{
    content = nullptr;
}

void NodeChannelStripView::resized()
{
    if (content)
        content->setBounds (getLocalBounds());
}

void NodeChannelStripView::stabilizeContent()
{
    if (content)
        content->nodeSelected();
}

void NodeChannelStripView::initializeView (Services& app)
{
    auto& gui = *app.find<GuiService>();
    content.reset (new Content (gui));
    addAndMakeVisible (content.get());
    resized();
    repaint();
}

} // namespace element
