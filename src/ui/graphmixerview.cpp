// Copyright 2019-2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <element/services.hpp>
#include <element/tags.hpp>
#include <element/ui/style.hpp>

#include "ui/nodechannelstrip.hpp"
#include "ui/graphmixerview.hpp"
#include "ui/horizontallistbox.hpp"
#include "ui/viewhelpers.hpp"

#include "common.hpp"

namespace element {

class GraphMixerChannelStrip : public NodeChannelStripComponent,
                               public DragAndDropTarget,
                               public ComponentListener
{
public:
    std::function<void()> onReordered;

    GraphMixerChannelStrip (GuiService& gui) : NodeChannelStripComponent (gui, false)
    {
        onNodeChanged = [this]() {
            setNodeNameEditable (! (getNode().isIONode()));
            repaint();
        };
        // Channel-name label pops in white so it stays legible against
        // the type-tinted strip background regardless of node format.
        setNodeNameColour (Colours::white);
        listener.reset (new ChildListener (*this));
        addMouseListener (listener.get(), true);
    }

    ~GraphMixerChannelStrip()
    {
        removeMouseListener (listener.get());
        listener.reset();
    }

    void selectInGuiController()
    {
        if (auto* const cc = ViewHelpers::findContentComponent (this))
            if (auto* const gui = cc->services().find<GuiService>())
                if (getNode() != gui->getSelectedNode())
                    gui->selectNode (getNode());
    }

    void mouseDown (const MouseEvent& ev) override
    {
        if (! down)
        {
            down = true;
            dragging = false;
            selectInGuiController();
        }
    }

    void mouseDrag (const MouseEvent& ev) override
    {
        if (down && ! dragging)
        {
            dragging = true;
            auto* dnd = findParentComponentOfClass<DragAndDropContainer>();
            Image image (Image::ARGB, 1, 1, true);
            ScaledImage si (image);
            dnd->startDragging (var ("graphMixerStrip"), this, si);
        }
    }

    void mouseMove (const MouseEvent& ev) override
    {
        // noop
    }

    void mouseUp (const MouseEvent& ev) override
    {
        dragging = down = hover = false;
    }

    bool shouldDrawDragImageWhenOver() override
    {
        return false;
    }

    void itemDragEnter (const SourceDetails&) override
    {
        hover = true;
        repaint();
    }

    void itemDragExit (const SourceDetails&) override
    {
        hover = false;
        repaint();
    }

    void paint (Graphics& g) override
    {
        // Background stays subdued in every state — the type colour is
        // mixed in at low strength so the strip carries the cue without
        // shouting.  Selection is signalled by the saturated border in
        // paintOverChildren, not by flooding the bg.
        const auto accent = nodeTypeColour (getNode());
        const auto base   = Colors::widgetBackgroundColor.darker (0.45f);
        g.setColour (base.interpolatedWith (accent, 0.07f));
        g.fillAll();
        // Preserve the 1px right-edge separator from the parent paint.
        g.setColour (Colors::contentBackgroundColor);
        g.drawLine (getWidth() - 1.f, 0.0, getWidth() - 1.f, getHeight());
    }

    void paintOverChildren (Graphics& g) override
    {
        if (selected || (hover && ! dragging && ! down))
        {
            const auto accent = nodeTypeColour (getNode());
            g.setColour (selected ? accent : accent.withAlpha (0.6f));
            g.drawRect (0.f, 0.f, (float) getWidth(), (float) getHeight(), selected ? 1.4f : 1.0f);
        }
    }

    bool isInterestedInDragSource (const SourceDetails& details) override
    {
        return details.description == "graphMixerStrip";
    }

    void itemDropped (const SourceDetails& details) override
    {
        if (details.description == "graphMixerStrip")
        {
            auto* strip = dynamic_cast<GraphMixerChannelStrip*> (details.sourceComponent.get());
            auto myNode = getNode().data();
            auto dNode = strip->getNode().data();
            ValueTree parent = dNode.getParent();

            int myIndex = parent.indexOf (myNode);
            int dIndex = parent.indexOf (dNode);
            if (myIndex >= 0 && dIndex >= 0)
            {
                parent.moveChild (dIndex, myIndex, nullptr);
                if (onReordered)
                    onReordered();
            }
        }

        hover = false;
        repaint();
    }

    void setSelected (bool isNowSelected)
    {
        if (selected == isNowSelected)
            return;
        selected = isNowSelected;
        repaint();
    }

private:
    bool selected = false;
    bool dragging = false;
    bool down = false;
    bool hover = false;

    struct ChildListener : public MouseListener
    {
        ChildListener (GraphMixerChannelStrip& o) : owner (o) {}
        void mouseDown (const MouseEvent& ev) override
        {
            owner.selectInGuiController();
        }

        GraphMixerChannelStrip& owner;
    };

    std::unique_ptr<ChildListener> listener;

#if 0
    virtual void itemDragEnter (const SourceDetails& dragSourceDetails);
    virtual void itemDragMove (const SourceDetails& dragSourceDetails);
    virtual void itemDragExit (const SourceDetails& dragSourceDetails);
#endif
};

class GraphMixerListBoxModel : public ListBoxModel
{
public:
    GraphMixerListBoxModel (GuiService& g, HorizontalListBox& b) : gui (g), box (b) { refreshNodes(); }
    ~GraphMixerListBoxModel() {}

    int getNumRows() override
    {
        return nodes.size();
    }

    void paintListBoxItem (int rowNumber, Graphics& g, int width, int height, bool rowIsSelected) override
    {
        ignoreUnused (rowNumber, g, width, height, rowIsSelected);
    }

    Node getNode (int r)
    {
        return nodes[r];
    }

    Component* refreshComponentForRow (int rowNumber, bool isRowSelected, Component* existing) override
    {
        GraphMixerChannelStrip* const strip = existing == nullptr
                                                  ? new GraphMixerChannelStrip (gui)
                                                  : dynamic_cast<GraphMixerChannelStrip*> (existing);
        strip->onReordered = std::bind (&GraphMixerListBoxModel::onReordered, this);
        auto node = getNode (rowNumber);
        strip->setNode (node);
        strip->setSelected (node == gui.getSelectedNode());
        return strip;
    }

    void onReordered()
    {
        refreshNodes();
        box.updateContent();
    }

    void setNode (const Node& node)
    {
        if (node == _node)
            return;
        _node = node.isGraph() ? node : node.getParentGraph();
        refreshNodes();
    }

    void refreshNodes()
    {
        nodes.clearQuick();
        const auto graph = _node.isGraph() ? _node
                                           : gui.context().session()->getActiveGraph();
        for (int i = 0; i < graph.getNumNodes(); ++i)
        {
            const auto n = graph.getNode (i);
            // clang-format off
            if (n.isMidiIONode() || 
                n.getIdentifier() == EL_NODE_ID_MIDI_INPUT_DEVICE || 
                n.getIdentifier() == EL_NODE_ID_MIDI_OUTPUT_DEVICE)
            {
                continue;
            }
            // clang-format on

            nodes.add (n);
        }
    }
#if 0
    virtual void listBoxItemClicked (int row, const MouseEvent&);
    virtual void listBoxItemDoubleClicked (int row, const MouseEvent&);
    virtual void backgroundClicked (const MouseEvent&);
    virtual void selectedRowsChanged (int lastRowSelected);
    virtual void deleteKeyPressed (int lastRowSelected);
    virtual void returnKeyPressed (int lastRowSelected);
    virtual void listWasScrolled();
    virtual var getDragSourceDescription (const SparseSet<int>& rowsToDescribe);
    virtual String getTooltipForRow (int row);
    virtual MouseCursor getMouseCursorForRow (int row);
#endif
private:
    GuiService& gui;
    HorizontalListBox& box;
    Node _node;
    NodeArray nodes;
    [[maybe_unused]] bool dragging = false;
};

class GraphMixerView::Content : public Component,
                                public DragAndDropContainer,
                                private ValueTree::Listener
{
public:
    Content (GraphMixerView& v, GuiService& gui, Session* sess)
        : ui (gui), session (sess), view (v)
    {
        setOpaque (true);
        addAndMakeVisible (box);
        box.setRowHeight (80);
        model.reset (new GraphMixerListBoxModel (gui, box));
        box.setModel (model.get());
        box.updateContent();

        _conns.push_back (ui.nodeSelected.connect (std::bind (&Content::onNodeSelected, this)));
        auto& ssrv = *gui.sibling<SessionService>();
        _conns.push_back (ssrv.sigSessionLoaded.connect ([this]() {
            attachToActiveGraph();
            model->refreshNodes();
            model->setNode (session->getActiveGraph());
        }));

        attachToActiveGraph();
    }

    ~Content()
    {
        detachFromActiveGraph();
        for (auto& c : _conns)
            c.disconnect();
        _conns.clear();

        box.setModel (nullptr);
        model.reset();
    }

    /* Auto-refresh on graph-node add/remove so newly-added nodes show
     * up without requiring a click. */
    void attachToActiveGraph()
    {
        if (session == nullptr) return;
        auto g = session->getActiveGraph();
        if (! g.isValid()) return;
        auto t = g.data();
        if (t == attachedGraph_) return;
        detachFromActiveGraph();
        attachedGraph_ = t;
        if (attachedGraph_.isValid())
            attachedGraph_.addListener (this);
    }

    void detachFromActiveGraph()
    {
        if (attachedGraph_.isValid())
            attachedGraph_.removeListener (this);
        attachedGraph_ = ValueTree();
    }

    void valueTreeChildAdded (ValueTree&, ValueTree& child) override
    {
        if (child.hasType (types::Node) || child.hasType (tags::nodes))
            stabilize();
    }

    void valueTreeChildRemoved (ValueTree&, ValueTree& child, int) override
    {
        if (child.hasType (types::Node) || child.hasType (tags::nodes))
            stabilize();
    }

    void valueTreePropertyChanged (ValueTree&, const Identifier&) override {}
    void valueTreeChildOrderChanged (ValueTree&, int, int) override {}
    void valueTreeParentChanged (ValueTree&) override {}

    void onNodeSelected()
    {
        model->setNode (ui.getSelectedNode());
        // setNode() bails when the parent graph hasn't changed, so a
        // node added to the current graph (which fires nodeSelected on
        // the newly-added node) wouldn't be picked up otherwise — re-
        // walk the graph here so newly-added nodes appear immediately
        // instead of waiting for the user to click another strip.
        model->refreshNodes();
        box.updateContent();
    }

    void resized() override
    {
        box.setBounds (getLocalBounds());
    }

    void paint (Graphics& g) override
    {
        g.setColour (Colors::widgetBackgroundColor.darker());
        g.fillAll();

        if (model->getNumRows() <= 0)
        {
            g.setColour (Colors::textColor);
            g.setFont (Font (FontOptions (15.f)));
            g.drawText (TRANS ("No channels to display"),
                        getLocalBounds().toFloat(),
                        Justification::centred);
        }
    }

    void stabilize()
    {
        model->refreshNodes();
        box.updateContent();
        repaint();
    }

private:
    UI& ui;
    SessionPtr session;
    [[maybe_unused]] GraphMixerView& view;
    std::unique_ptr<GraphMixerListBoxModel> model;
    ChannelStripComponent channelStrip;
    HorizontalListBox box;
    std::vector<SignalConnection> _conns;
    ValueTree attachedGraph_;
};

GraphMixerView::GraphMixerView()
{
    setName (EL_VIEW_GRAPH_MIXER);
}

GraphMixerView::~GraphMixerView()
{
    content = nullptr;
}

void GraphMixerView::resized()
{
    if (content)
        content->setBounds (getLocalBounds());
}

void GraphMixerView::stabilizeContent()
{
    if (content)
        content->stabilize();
}

void GraphMixerView::initializeView (Services& app)
{
    content.reset (new Content (*this, *app.find<GuiService>(), app.context().session()));
    addAndMakeVisible (content.get());
    content->stabilize();
}

} // namespace element
