// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <element/services.hpp>
#include <element/ui.hpp>
#include <element/ui/content.hpp>
#include <element/node.hpp>
#include <element/context.hpp>

#include "engine/midiengine.hpp"

#include "ui/nodeportstable.hpp"
#include "ui/buttons.hpp"
#include "ui/contextmenus.hpp"
#include "ui/grapheditorcomponent.hpp"
#include "ui/ioconfigurationwindow.hpp"
#include "ui/nodeeditorfactory.hpp"
#include "ui/viewhelpers.hpp"
#include "ui/block.hpp"
#include "ui/blockutils.hpp"

#include "scopedflag.hpp"

namespace element {

namespace {

/** Map a plugin's category string to a default block colour.  Used
 *  when no user-set "color" UI property exists.
 *
 *  VST / VST3 / CLAP plugins set categories like "Instrument",
 *  "Fx", "Synth", "Effect|Reverb", "Instrument|u-he" etc.  Split on
 *  '|' and match the first token to a known bucket; unknown buckets
 *  fall through to a desaturated default so something always shows.
 *
 *  Element internal plugins populate `desc.category` directly (see
 *  fillInPluginDescription / getPluginDescription in src/nodes/*). */
inline juce::Colour defaultColorForCategory (const juce::String& category)
{
    if (category.isEmpty()) return juce::Colour (0x00000000);

    /* Lowercase + whitespace-normalised tokens for both halves of a
     * potential vendor-style "X|Y" tag.  Plugins are inconsistent
     * about whether the bucket is in the first or second slot
     * (e.g. CLAP uses "delay" / VST3 uses "Fx|Delay") so we match
     * the more specific token first across both. */
    const auto first  = category.upToFirstOccurrenceOf ("|", false, true).trim().toLowerCase();
    const auto second = category.fromFirstOccurrenceOf ("|", false, true).trim().toLowerCase();
    auto matchAny = [&] (std::initializer_list<const char*> patterns) {
        for (auto* p : patterns)
        {
            if (first == p || second == p
                || first.contains (p) || second.contains (p))
                return true;
        }
        return false;
    };

    /* === Most-specific effect buckets first (so an "Fx|Reverb"
         lands on "reverb" rather than the generic "effect" bucket). */
    if (matchAny ({ "reverb" }))             return juce::Colour { 0xff'4a'a8'8c };  /* teal-green */
    if (matchAny ({ "delay", "echo" }))      return juce::Colour { 0xff'5a'b0'b0 };  /* aqua */
    if (matchAny ({ "eq", "equalizer", "equaliser" }))
                                              return juce::Colour { 0xff'9a'c0'4a };  /* lime */
    if (matchAny ({ "dynamics", "compressor", "limiter", "gate",
                    "expander", "mastering" }))
                                              return juce::Colour { 0xff'c0'b0'40 };  /* mustard */
    if (matchAny ({ "distortion", "saturation", "amp", "amplifier" }))
                                              return juce::Colour { 0xff'd0'60'40 };  /* burnt orange */
    if (matchAny ({ "modulation", "chorus", "flanger", "phaser",
                    "tremolo", "vibrato" }))
                                              return juce::Colour { 0xff'a0'5a'c0 };  /* lavender */
    if (matchAny ({ "filter", "wah" }))      return juce::Colour { 0xff'5a'c0'a0 };  /* mint */
    if (matchAny ({ "pitch", "pitch-shift", "pitch-shifter",
                    "pitch-correction", "vocoder" }))
                                              return juce::Colour { 0xff'b0'9a'c0 };  /* dusty purple */
    if (matchAny ({ "spatial", "stereo", "surround", "panner" }))
                                              return juce::Colour { 0xff'5a'7a'c0 };  /* sky blue */
    if (matchAny ({ "analyzer", "analyser", "meter", "spectrum" }))
                                              return juce::Colour { 0xff'40'a0'c0 };  /* steel blue */
    if (matchAny ({ "drum", "drum-machine", "percussion" }))
                                              return juce::Colour { 0xff'e0'70'4a };  /* terracotta */

    /* === Broad family buckets. */
    if (matchAny ({ "instrument", "synth", "synthesizer", "synthesiser",
                    "sampler", "rom-sampler", "monosynth", "multivoice",
                    "virtual-instrument", "piano", "strings", "brass",
                    "winds", "ensemble", "external" }))
        return juce::Colour { 0xff'8a'5a'c0 };                                       /* warm purple */
    if (matchAny ({ "sequencer", "tracker", "arpeggiator",
                    "step-sequencer", "pattern" }))
        return juce::Colour { 0xff'40'a0'a0 };                                       /* teal */
    if (matchAny ({ "midi" }))
        return juce::Colour { 0xff'd0'80'40 };                                       /* orange */
    if (matchAny ({ "effect", "fx", "multi-effects", "tools",
                    "utility" }))
        return juce::Colour { 0xff'5a'a5'5a };                                       /* green */
    if (matchAny ({ "mixer", "mixing", "router", "routing", "channel" }))
        return juce::Colour { 0xff'5a'7a'a0 };                                       /* slate blue */
    if (matchAny ({ "audio", "player", "media", "playback" }))
        return juce::Colour { 0xff'4a'90'd0 };                                       /* blue */
    if (matchAny ({ "control", "osc", "script", "automation",
                    "control-surface" }))
        return juce::Colour { 0xff'c0'a0'40 };                                       /* gold */
    if (matchAny ({ "generator", "visualiser", "visualizer", "scope",
                    "noise" }))
        return juce::Colour { 0xff'b0'5a'a0 };                                       /* magenta */

    /* Unknown but non-empty — neutral desaturated.  Different from
     * "no colour" so the user can still tell the block was tagged. */
    return juce::Colour { 0xff'70'70'70 };
}

} // namespace

namespace detail {
inline static Context* context (juce::Component* comp)
{
    if (auto cc = ViewHelpers::findContentComponent (comp))
        return &cc->context();
    return nullptr;
}

static bool canResize (BlockComponent& block)
{
    return block.getDisplayMode() == BlockComponent::Embed && block.getNode().getFormat() == EL_NODE_FORMAT_NAME;
}
} // namespace detail

//=============================================================================
PortComponent::PortComponent (const Node& g, const Node& n, const uint32 nid, const uint32 i, const bool dir, const PortType t, const bool v)
    : graph (g), node (n), nodeID (nid), port (i), type (t), input (dir), vertical (v)
{
    if (const ProcessorPtr obj = node.getObject())
    {
        const Port p (node.getPort ((int) port));
        String tip = p.getName();

        if (tip.isEmpty())
        {
            if (node.isAudioInputNode())
            {
                tip = "Input " + String (port + 1);
            }
            else if (node.isAudioOutputNode())
            {
                tip = "Output " + String (port + 1);
            }
            else
            {
                tip = (input ? "Input " : "Output ") + String (port + 1);
            }
        }

        setTooltip (tip);
    }

    setSize (16, 16);
}

PortComponent::~PortComponent() {}

bool PortComponent::isInput() const noexcept { return input; }
uint32 PortComponent::getNodeId() const noexcept { return nodeID; }
uint32 PortComponent::getPortIndex() const noexcept { return port; }

Colour PortComponent::getColor() const noexcept
{
    switch (this->type)
    {
        case PortType::Audio:
            return Colours::lightgreen;
            break;
        case PortType::Control:
            return Colours::lightblue;
            break;
        case PortType::Midi:
            return Colours::orange;
            break;
        default:
            break;
    }

    return Colours::red;
}

void PortComponent::paint (Graphics& g)
{
    if (true)
    {
        const auto bounds = juce::Rectangle<float> (0.f, 0.f, (float) getWidth(), (float) getHeight());
        g.setColour (getColor());
        g.fillRoundedRectangle (bounds, 2.f);

        /* Element: dark 1px outline around the port dot so the colored
         * fill reads cleanly against any block-fill or canvas color. */
        g.setColour (Colours::black.withAlpha (0.75f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 1.5f, 1.0f);
    }
    else
    {
        Path path;

        float start = 0.0, end = 0.0;
        if (vertical)
        {
            start = input ? -90.f : 270.f;
            end = input ? 90.f : 90.f;
        }
        else
        {
            start = input ? 180.f : 0.f;
            end = input ? 360.f : 180.f;
        }

        path.addPieSegment (getLocalBounds().toFloat(),
                            degreesToRadians (start),
                            degreesToRadians (end),
                            0);
        g.setColour (getColor());
        g.fillPath (path);
    }
}

void PortComponent::mouseDown (const MouseEvent& e)
{
    if (! isEnabled())
        return;
    getGraphEditor()->beginConnectorDrag (
        input ? 0 : nodeID, port, input ? nodeID : 0, port, e);
}

void PortComponent::mouseDrag (const MouseEvent& e)
{
    if (! isEnabled())
        return;
    getGraphEditor()->dragConnector (e);
}

void PortComponent::mouseUp (const MouseEvent& e)
{
    if (! isEnabled())
        return;
    getGraphEditor()->endDraggingConnector (e);
}

GraphEditorComponent* PortComponent::getGraphEditor() const noexcept
{
    return findParentComponentOfClass<GraphEditorComponent>();
}

void BlockComponent::AsyncEmbedInit::timerCallback()
{
    if (block.getDisplayMode() != BlockComponent::Embed)
    {
        stopTimer();
        return;
    }

    if (ViewHelpers::findContentComponent (&block) == nullptr)
        return;
    initialized = true;
    block.setDisplayModeInternal (Embed, true);
    stopTimer();
}

//=============================================================================
BlockComponent::BlockComponent (const Node& graph_, const Node& node_, const bool vertical_)
    : filterID (node_.getNodeId()),
      graph (graph_),
      node (node_),
      font (FontOptions (11.0f)),
      embedInit (*this)
{
    nodeObject = node.getPropertyAsValue (tags::object, true);
    obj = node.getObject();
    jassert (obj != nullptr);
    nodeObject.addListener (this);

    /* Element: dropped setBufferedToImage(true) + the DropShadow
     * effect.  Both made sense when each block redraw was a costly
     * software-rasterized paint pushed through the X11 socket — the
     * offscreen image cached the result so subsequent paints were
     * cheap.  With the GL renderer attached to the main window, all
     * drawing goes through the GPU and the per-component offscreen
     * actually hurts: every resize / move invalidates the image and
     * forces a full regeneration, and the Gaussian-blurred shadow
     * effect runs on the CPU regardless of renderer.  Stripping
     * both removed the resize choppiness on dense graphs. */
    nodeEnabled = node.getPropertyAsValue (tags::enabled);
    nodeEnabled.addListener (this);
    nodeName = node.getPropertyAsValue (tags::name);
    nodeName.addListener (this);

    addAndMakeVisible (configButton);
    configButton.setPath (getIcons().fasCog);
    configButton.addListener (this);

    addAndMakeVisible (powerButton);
    powerButton.setColour (SettingButton::backgroundOnColourId,
                           findColour (SettingButton::backgroundColourId));
    powerButton.setColour (SettingButton::backgroundColourId, Colors::toggleBlue);
    powerButton.getToggleStateValue().referTo (node.getPropertyAsValue (tags::bypass));
    powerButton.setClickingTogglesState (true);
    powerButton.addListener (this);

    addAndMakeVisible (muteButton);
    muteButton.setYesNoText ("M", "M");
    muteButton.setColour (SettingButton::backgroundOnColourId, Colors::toggleRed);
    muteButton.getToggleStateValue().referTo (node.getPropertyAsValue (tags::mute));
    muteButton.setClickingTogglesState (true);
    muteButton.addListener (this);

    hiddenPorts = node.getBlockValueTree()
                      .getPropertyAsValue (tags::hiddenPorts, nullptr);
    hiddenPorts.addListener (this);

    auto blockData = node.getBlockValueTree();
    displayModeValue = blockData.getPropertyAsValue (tags::displayMode, nullptr);
    displayModeValue.addListener (this);
    const auto idm = getDisplayModeFromString (displayModeValue.getValue());
    setDisplayModeInternal (idm, false);
    if (idm == Embed)
        embedInit.startTimer (14);

    // setup a fallback alignment.
    String portAlignStr = "middle";

    /* Element: audio I/O nodes always default to middle port alignment.
     * The legacy display-mode-based fallback below puts ports flush to
     * the leading edge in Normal/Embed (and vertical Compact/Small),
     * which looks lopsided on wide multi-channel I/O blocks — a stereo
     * Audio In with 2 dots clustered on the left edge of an 8-channel-
     * wide block reads as misaligned.  MIDI blocks keep the existing
     * defaults (their single port + "before" alignment already reads
     * well). */
    const bool isAudioIONode = node.isAudioInputNode() || node.isAudioOutputNode();
    if (! isAudioIONode)
    {
        switch (getDisplayMode())
        {
            case Normal:
            case Embed:
                portAlignStr = "before";
                break;
            case Small:
            case Compact:
                portAlignStr = vertical_ ? "before" : "middle";
                break;
        }
    }

    _portAlign = portAlignmentFromKey (blockData.getProperty (tags::portAlignment, portAlignStr));
    if (! blockData.hasProperty (tags::portAlignment))
        blockData.setProperty (tags::portAlignment, portAlignmentKey (_portAlign), nullptr);

    customWidth = node.getBlockValueTree().getProperty (tags::width, customWidth);
    customHeight = node.getBlockValueTree().getProperty (tags::height, customHeight);
    setSize (customWidth > 0 ? customWidth : 170,
             customHeight > 0 ? customHeight : 60);

    if (obj != nullptr)
    {
        willRemoveConn = obj->willBeRemoved.connect ([this]() {
            valueChanged (nodeObject);
        });
    }
}

BlockComponent::~BlockComponent() noexcept
{
    nodeObject.removeListener (this);
    willRemoveConn.disconnect();
    clearEmbedded();
    obj = nullptr;

    nodeEnabled.removeListener (this);
    nodeName.removeListener (this);
    hiddenPorts.removeListener (this);
    displayModeValue.removeListener (this);
    deleteAllPins();
}

void BlockComponent::componentMovedOrResized (Component& component,
                                              bool wasMoved,
                                              bool wasResized)
{
    if (embedded == nullptr || embedded.get() != &component)
        return;

    struct AsyncUpdateSize : public juce::MessageManager::MessageBase
    {
        AsyncUpdateSize (BlockComponent& b) : block (&b) {}
        juce::Component::SafePointer<BlockComponent> block;
        void messageCallback() override
        {
            if (auto bp = block.getComponent())
                bp->updateSize();
        }
    };

    if (wasResized)
        (new AsyncUpdateSize (*this))->post();

    juce::ignoreUnused (wasMoved);
}

void BlockComponent::clearEmbedded()
{
    if (nullptr == embedded)
        return;

    embedded->removeComponentListener (this);

    if (auto jed = dynamic_cast<juce::AudioProcessorEditor*> (embedded.get()))
        jed->processor.editorBeingDeleted (jed);

    embedded.reset();
}

void BlockComponent::setDisplayModeInternal (DisplayMode mode, bool force)
{
    if (mode == displayMode && ! force)
        return;
    auto oldMode = displayMode;

    displayMode = mode;
    displayModeValue.removeListener (this);
    displayModeValue.setValue (getDisplayModeKey (displayMode));
    displayModeValue.addListener (this);

    if (oldMode == Embed)
        clearEmbedded();
    updateSize();

    detail::updateBlockButtonVisibility (*this, this->node);

    if (displayMode == Embed)
    {
        if (detail::supportsEmbed (this->node))
        {
            struct EmbedBockAsync : MessageManager::MessageBase
            {
                using PtrType = std::unique_ptr<juce::Component>;
                EmbedBockAsync (BlockComponent& b, const Node& n, UI& u, PtrType& p, DisplayMode om)
                    : block (b), node (n), ui (u), embedded (p), oldMode (om) {}

                void messageCallback() override
                {
                    ui.closePluginWindowsFor (node, false);

                    if (embedded == nullptr)
                    {
                        NodeEditorFactory factory (ui);
                        if (auto e = factory.instantiate (node, NodeEditorPlacement::PluginWindow))
                            embedded.reset (e.release());
                        else if (auto ape = NodeEditorFactory::createAudioProcessorEditor (node))
                            embedded.reset (ape.release());
                        else if (auto ed = NodeEditorFactory::createEditor (node))
                            embedded.reset (ed.release());
                    }

                    if (embedded != nullptr)
                    {
                        block.addAndMakeVisible (embedded.get());
                        block.updateSize();
                        block.resized();
                        embedded->addComponentListener (&block);
                    }
                    else
                    {
                        if (oldMode != Embed)
                            block.setDisplayModeInternal (oldMode, true);
                    }
                }

                BlockComponent& block;
                Node node;
                UI& ui;
                PtrType& embedded;
                DisplayMode oldMode;
            };

            if (auto* ui = ViewHelpers::getGuiController (this))
                (new EmbedBockAsync (*this, node, *ui, this->embedded, oldMode))->post();
        }
        else
        {
        }
    }
    else
    {
        clearEmbedded();
    }
}

void BlockComponent::setDisplayMode (DisplayMode mode)
{
    setDisplayModeInternal (mode, false);
}

void BlockComponent::setPortAlignment (PortAlignment newAlign)
{
    node.getBlockValueTree().setProperty (
        tags::portAlignment, portAlignmentKey (newAlign), nullptr);
    _portAlign = newAlign;
    resized();
}

void BlockComponent::moveBlockTo (double x, double y)
{
    setNodePosition (x, y);
    updatePosition();
}

bool BlockComponent::isSelected() const noexcept
{
    return selected;
}

void BlockComponent::setPowerButtonVisible (bool visible) { setButtonVisible (powerButton, visible); }
void BlockComponent::setConfigButtonVisible (bool visible) { setButtonVisible (configButton, visible); }
void BlockComponent::setMuteButtonVisible (bool visible) { setButtonVisible (muteButton, visible); }

void BlockComponent::valueChanged (Value& value)
{
    if (nodeEnabled.refersToSameSourceAs (value))
    {
        repaint();
    }
    else if (nodeName.refersToSameSourceAs (value))
    {
        setName (node.getName());
        update (false, false);
    }
    else if (hiddenPorts.refersToSameSourceAs (value))
    {
        if (auto* ge = getGraphPanel())
            ge->updateComponents (false);
    }
    else if (displayModeValue.refersToSameSourceAs (value))
    {
        update (false, false);
    }
    else if (nodeObject.refersToSameSourceAs (nodeObject))
    {
        willRemoveConn.disconnect();
        clearEmbedded();
        obj = nullptr;
    }
}

void BlockComponent::handleAsyncUpdate()
{
    repaint();
}

void BlockComponent::buttonClicked (Button* b)
{
    if (! isEnabled())
        return;

    ProcessorPtr obj = node.getObject();
    auto* proc = (obj) ? obj->getAudioProcessor() : 0;
    if (! obj)
        return;

    if (b == &configButton && configButton.getToggleState())
    {
        configButton.setToggleState (false, dontSendNotification);
        // ioBox.clear();
    }
    else if (proc != nullptr && b == &configButton && ! configButton.getToggleState())
    {
        if (auto context = detail::context (this))
        {
            CallOutBox::launchAsynchronously (
                std::make_unique<IOConfigurationWindow> (*context, getNode(), *proc),
                configButton.getScreenBounds(),
                nullptr);
        }
        else
        {
            jassertfalse;
        }
    }
    else if (b == &powerButton)
    {
        if (obj->isSuspended() != node.isBypassed())
            obj->suspendProcessing (node.isBypassed());
    }
    else if (b == &muteButton)
    {
        node.setMuted (muteButton.getToggleState());
    }
}

void BlockComponent::deleteAllPins()
{
    for (int i = getNumChildComponents(); --i >= 0;)
        if (auto* c = dynamic_cast<PortComponent*> (getChildComponent (i)))
            delete c;
}

void BlockComponent::changeListenerCallback (ChangeBroadcaster* broadcaster)
{
    if (broadcaster == &colorSelector)
    {
        color = colorSelector.getCurrentColour().withAlpha (1.0f);
        node.getUIValueTree().setProperty ("color", color.toString(), nullptr);

        forEachSibling ([this] (BlockComponent& sibling) {
            if (! sibling.isSelected() || sibling.color == color)
                return;

            sibling.color = color;
            sibling.node.getUIValueTree().setProperty ("color",
                                                       sibling.color.toString(),
                                                       nullptr);
            sibling.repaint();
        });

        repaint();
    }
}

void BlockComponent::mouseDown (const MouseEvent& e)
{
    if (! isEnabled())
        return;

    originalPos = localPointToGlobal (Point<int>());
    originalBounds = getBounds();
    toFront (true);
    dragging = false;
    auto* const panel = getGraphPanel();

    selectionMouseDownResult = panel->selectedNodes.addToSelectionOnMouseDown (node.getNodeId(), e.mods);
    if (auto* cc = ViewHelpers::findContentComponent (this))
    {
        ScopedFlag block (panel->ignoreNodeSelected, true);
        cc->services().find<GuiService>()->selectNode (node);
    }

    if (e.mods.isPopupMenu())
    {
        auto* const world = ViewHelpers::getGlobals (this);
        auto& plugins (world->plugins());
        NodePopupMenu menu (node);
        menu.addReplaceSubmenu (plugins);

        if (! node.isMidiIONode() && ! node.isMidiDevice())
        {
            menu.addSeparator();
            menu.addItem (10, "Ports...", true, false);
        }

        menu.addSeparator();
        menu.addColorSubmenu (colorSelector);
        addDisplaySubmenu (menu);

        menu.addOptionsSubmenu();

        if (world)
            menu.addPresetsMenu (world->presets());

        colorSelector.setCurrentColour (Colour::fromString (
            node.getUIValueTree().getProperty ("color", color.toString()).toString()));
        colorSelector.addChangeListener (this);
        const int result = menu.show();
        colorSelector.removeChangeListener (this);

        const auto types = plugins.getKnownPlugins().getTypes();

        if (auto* message = menu.createMessageForResultCode (result))
        {
            const bool beingRemoved = nullptr != dynamic_cast<RemoveNodeMessage*> (message);
            ViewHelpers::postMessageFor (this, message);
            if (beingRemoved)
                clearEmbedded();

            for (const auto& nodeId : getGraphPanel()->selectedNodes)
            {
                if (nodeId == node.getNodeId())
                    continue;
                const Node selectedNode = graph.getNodeById (nodeId);
                if (selectedNode.isValid())
                {
                    if (nullptr != dynamic_cast<RemoveNodeMessage*> (message))
                    {
                        if (auto panel = getGraphPanel())
                            if (auto sb = panel->findBlock (selectedNode))
                                sb->clearEmbedded();

                        ViewHelpers::postMessageFor (this, new RemoveNodeMessage (selectedNode));
                    }
                }
            }
        }
        else if (KnownPluginList::getIndexChosenByMenu (types, result) >= 0)
        {
            auto index = KnownPluginList::getIndexChosenByMenu (types, result);
            ViewHelpers::postMessageFor (this,
                                         new ReplaceNodeMessage (node, types.getUnchecked (index)));
        }
        else
        {
            switch (result)
            {
                case 10: {
                    auto* component = new NodePortsTable();
                    component->setNode (node);
                    CallOutBox::launchAsynchronously (
                        std::unique_ptr<Component> (component),
                        getScreenBounds(),
                        nullptr);
                    break;
                }
            }
        }
    }

    repaint();
    getGraphPanel()->updateSelection();
}

void BlockComponent::mouseMove (const MouseEvent& e)
{
    Component::mouseMove (e);
    const bool canResize = detail::canResize (*this);

    if (canResize && getCornerResizeBox().toFloat().contains (e.position))
    {
        if (! mouseInCornerResize)
        {
            mouseInCornerResize = true;
            repaint();
        }
    }
    else
    {
        if (mouseInCornerResize)
        {
            mouseInCornerResize = false;
            repaint();
        }
    }
}

void BlockComponent::mouseDrag (const MouseEvent& e)
{
    if (! isEnabled())
        return;

    if (e.mods.isPopupMenu() || blockDrag)
        return;

    auto* const panel = getGraphPanel();

    if (mouseInCornerResize)
    {
        setCustomSize (originalBounds.getWidth() + e.getDistanceFromDragStartX(),
                       originalBounds.getHeight() + e.getDistanceFromDragStartY());
        if (panel != nullptr)
            panel->updateConnectorComponents();
        return;
    }

    // if (std::abs (e.getDistanceFromDragStartX()) < 2 && abs (e.getDistanceFromDragStartY()) < 2)
    //     return;

    dragging = true;
    int deltaX = e.getDistanceFromDragStartX();
    int deltaY = e.getDistanceFromDragStartY();

    Point<int> pos (originalPos + Point<int> (deltaX, deltaY));
    if (getParentComponent() != nullptr)
        pos = getParentComponent()->getLocalPoint (nullptr, pos);

    bool hasMoved = false;

    double ox, oy, nx, ny;
    node.getPosition (ox, oy);
    nx = ox;
    ny = oy;
    moveBlockTo (pos.getX(), pos.getY());

    if (panel != nullptr)
    {
        if (panel->onBlockMoved)
            panel->onBlockMoved (*this);

        // onBlockMoved may have reverted position
        node.getPosition (nx, ny);
        hasMoved = nx != ox || ny != oy;

        int dx = deltaX - lastDragDeltaX;
        int dy = deltaY - lastDragDeltaY;

        if (hasMoved)
        {
            for (int i = 0; i < panel->getNumChildComponents(); ++i)
            {
                auto* block = dynamic_cast<BlockComponent*> (panel->getChildComponent (i));
                if (block == nullptr || block == this || ! block->isSelected())
                    continue;

                auto bp = block->getNodePosition();
                if (! vertical)
                    std::swap (bp.x, bp.y);

                block->moveBlockTo (roundToIntAccurate (bp.x + dx),
                                    roundToIntAccurate (bp.y + dy));
                panel->onBlockMoved (*block);
            }
        }

        panel->updateConnectorComponents();
    }

    lastDragDeltaX = deltaX;
    lastDragDeltaY = deltaY;
}

void BlockComponent::mouseUp (const MouseEvent& e)
{
    dragging = selectionMouseDownResult = blockDrag = false;
    lastDragDeltaX = lastDragDeltaY = 0;
    if (! isEnabled())
        return;
    auto* panel = getGraphPanel();

    if (panel)
        panel->selectedNodes.addToSelectionOnMouseUp (node.getNodeId(), e.mods, dragging, selectionMouseDownResult);

    if (e.mouseWasClicked() && e.getNumberOfClicks() == 2)
        makeEditorActive();
}

void BlockComponent::setSelectedInternal (bool status)
{
    if (selected == status)
        return;
    selected = status;
    repaint();
}

void BlockComponent::makeEditorActive()
{
    if (node.isGraph())
    {
        // TODO: this can cause a crash, do it async
        if (auto* cc = ViewHelpers::findContentComponent (this))
            cc->setCurrentNode (node);
    }
    else if (node.hasProperty (tags::missing))
    {
        String message = "This node is unavailable and running as a Placeholder.\n";
        message << node.getName() << " (" << node.getFormat().toString()
                << ") could not be found for loading.";
        AlertWindow::showMessageBoxAsync (AlertWindow::InfoIcon,
                                          node.getName(),
                                          message,
                                          "Ok");
    }
    else if (node.isValid())
    {
        if (displayMode == Embed)
        {
            setDisplayMode (Small);
        }
        ViewHelpers::presentPluginWindow (this, node);
    }
}

bool BlockComponent::hitTest (int x, int y)
{
    for (int i = getNumChildComponents(); --i >= 0;)
        if (getChildComponent (i)->getBounds().contains (x, y))
            return true;
    return getBoxRectangle().contains (x, y);
}

Rectangle<int> BlockComponent::getBoxRectangle() const
{
    return getLocalBounds().reduced (pinSize / 2);
}

Rectangle<int> BlockComponent::getCornerResizeBox() const
{
    auto r = getBoxRectangle();
    return { r.getRight() - 14, r.getBottom() - 14, 12, 12 };
}

void BlockComponent::paintOverChildren (Graphics& g)
{
    ignoreUnused (g);
}

void BlockComponent::paint (Graphics& g)
{
    const float cornerSize = 2.4f;
    const auto box (getBoxRectangle());
    const int colorBarHeight = vertical ? 20 : 18;
    bool colorize = color != Colour (0x00000000);
    Colour bgc = isEnabled() && node.isEnabled() && ! node.isMissing()
                     ? Colors::widgetBackgroundColor.brighter (0.8f)
                     : Colors::widgetBackgroundColor.brighter (0.2f);

    auto barColor = isEnabled() && node.isEnabled() ? color : color.darker (.1f);

    if (isSelected())
    {
        bgc = bgc.brighter (0.55f);
    }

    if (colorize)
    {
        switch (displayMode)
        {
            case Compact:
            case Small: {
                g.setColour (selected ? barColor.brighter (0.275f) : barColor);
                g.fillRoundedRectangle (box.toFloat(), cornerSize);
                break;
            }
            case Embed:
            case Normal: {
                auto b1 = box;
                auto b2 = b1.removeFromTop (colorBarHeight);
                g.setColour (barColor);
                Path path;
                path.addRoundedRectangle (b2.getX(), b2.getY(), b2.getWidth(), b2.getHeight(), cornerSize, cornerSize, true, true, false, false);
                g.fillPath (path);

                path.clear();
                g.setColour (bgc);
                path.addRoundedRectangle (b1.getX(), b1.getY(), b1.getWidth(), b1.getHeight(), cornerSize, cornerSize, false, false, true, true);
                g.fillPath (path);
                break;
            }
        }
    }
    else
    {
        g.setColour (bgc);
        g.fillRoundedRectangle (box.toFloat(), cornerSize);
    }

    if (node.isMissing())
    {
        g.setColour (bgc.darker (0.6f));
        g.drawRoundedRectangle (box.toFloat(), cornerSize, 1.3f);
    }
    else
    {
        /* Element: per-block-type colored border + dark inset separator.
         * Visual cue so users can tell at a glance whether a block is
         * Audio I/O, JACK MIDI, a VST2 / VST3 / CLAP / LV2 plugin, or
         * an Element internal utility — independent of the user-set
         * "color" property (which still drives the fill / title-bar as
         * before).  Selection is already conveyed by the brightened
         * background, so the border stays at a single bright weight
         * regardless of selection state.
         *
         * The dark 1px stroke sits just inside the colored stroke to
         * keep the color from bleeding into the block contents when
         * the colorize fill (or a brightened-selection bg) is the same
         * family of hue as the type. */
        const auto typeColor = [this]() -> Colour {
            /* Border colour priority:
             *   1. Audio / MIDI IO pseudo-nodes — match their wire colour.
             *   2. Plugin category — Instrument / Sequencer / MIDI / Effect
             *      / etc. via defaultColorForCategory.  Works for Element
             *      internals (whose desc.category we populate) and for
             *      hosted plugins whose vendor sets one.
             *   3. Plugin format fallback (VST / VST3 / CLAP / LV2 / AU)
             *      when there's no category at all. */
            if (node.isAudioInputNode() || node.isAudioOutputNode())
                return Colour (0xff00e676);      // green A400 — matches audio wire
            if (node.isMidiInputNode() || node.isMidiOutputNode())
                return Colour (0xffffa726);      // orange 400 — matches MIDI wire

            const auto category = node.getProperty (tags::category).toString();
            if (category.isNotEmpty())
            {
                const auto c = defaultColorForCategory (category);
                if (c != Colour (0x00000000))
                    return c;
            }

            const auto format = node.getFormat().toString();
            if (format == "VST")       return Colour (0xff3d5afe);  // indigo A400 — VST2
            if (format == "VST3")      return Colour (0xffd500f9);  // purple A400 — VST3
            if (format == "CLAP")      return Colour (0xff00e5ff);  // cyan   A400 — CLAP
            if (format == "LV2")       return Colour (0xffff1744);  // red    A400 — LV2
            if (format == "AudioUnit") return Colour (0xffff9100);  // orange A400 — AudioUnit
            if (format == EL_NODE_FORMAT_NAME)
                return Colour (0xffff4081);                          // pink A400 — internal w/o category
            return Colour (0xffbdbdbd);                              // gray 400 fallback
        }();

        g.setColour (typeColor);
        g.drawRoundedRectangle (box.toFloat(), cornerSize, 1.5f);

        g.setColour (Colours::black.withAlpha (0.6f));
        g.drawRoundedRectangle (box.toFloat().reduced (1.5f, 1.5f),
                                juce::jmax (0.5f, cornerSize - 1.5f),
                                1.0f);
    }

    auto displayName = node.getDisplayName();
    auto subName = node.hasModifiedName() ? node.getPluginName() : String();

    if (node.getParentGraph().isRootGraph())
    {
        if (node.isAudioIONode())
        {
            subName = String();
        }
        else if (node.isMidiInputNode())
        {
            auto mode = ViewHelpers::getGuiController (this)->getRunMode();
            auto& midi = ViewHelpers::getGlobals (this)->midi();
            if (mode != RunMode::Plugin && midi.getNumActiveMidiInputs() <= 0)
                subName = "(no device)";
        }
    }

    auto normalTextColor = node.isMissing() ? Colors::toggleRed : Colours::black;
    if (colorize && ! node.isMissing())
        normalTextColor = Colours::white.overlaidWith (color).contrasting();

    g.setColour (normalTextColor);
    g.setFont (Font (FontOptions (12.f).withStyle (node.isMissing() ? "Bold" : "Regular")));

    if (vertical)
    {
        switch (displayMode)
        {
            case Small:
            case Normal:
            case Embed: {
                int y = box.getY() + 2;
                g.drawFittedText (displayName, box.getX(), y, box.getWidth(), 18, Justification::centred, 2);

                if (subName.isNotEmpty())
                {
                    g.setColour (Colours::black);
                    g.setFont (Font (FontOptions (9.f)));
                    y += colorBarHeight;
                    g.drawFittedText (subName, box.getX(), y, box.getWidth(), 9, Justification::centred, 2);
                }
                break;
            }
            case Compact: {
                g.drawFittedText (displayName, box.getX(), box.getY(), box.getWidth(), box.getHeight(), Justification::centred, 2);
                break;
            }
        }
    }
    else
    {
        switch (displayMode)
        {
            case Normal:
            case Embed: {
                int y = box.getY();
                g.drawFittedText (displayName, box.getX(), y, box.getWidth(), 18, Justification::centred, 2);

                if (subName.isNotEmpty())
                {
                    g.setColour (Colours::black);
                    g.setFont (Font (FontOptions (9.f)));
                    y += colorBarHeight;
                    g.drawFittedText (subName, box.getX(), y, box.getWidth(), 13, Justification::centred, 2);
                }
                break;
            }
            case Small: {
                g.drawFittedText (displayName, box, Justification::centred, 2);
                break;
            }
            case Compact: {
                Style::drawVerticalText (g, displayName, getLocalBounds(), Justification::centred);
                break;
            }
        }
    }

    if (mouseInCornerResize)
    {
        auto cbox = getCornerResizeBox();
        g.setOrigin (cbox.getPosition());
        getLookAndFeel().drawCornerResizer (g, 12, 12, true, false);
    }
}

void BlockComponent::resized()
{
    const auto box (getBoxRectangle());

    auto r = box.reduced (4, 2).removeFromBottom (18);
    r.removeFromBottom (4);
    const int halfPinSize = pinSize / 2;

    {
        Component* buttons[] = { &configButton, &muteButton, &powerButton };
        for (int i = 0; i < 3; ++i)
            if (buttons[i]->isVisible())
                buttons[i]->setBounds (r.removeFromLeft (16));
    }

    if (displayMode == Embed && embedded)
    {
        auto er = box;
        er.removeFromTop (vertical ? 20 : 18);
        er.removeFromBottom (22);
        if (node.getFormat() != EL_NODE_FORMAT_NAME)
        {
            customWidth = customHeight = 0;
            embedded->setBounds (er.getX(),
                                 er.getY(),
                                 embedded->getWidth(),
                                 embedded->getHeight());
        }
        else
        {
            customWidth = getWidth();
            customHeight = getHeight();
            embedded->setBounds (er);
        }
    }

    const auto spaceNeeded = (std::max (numIns, numOuts) * (pinSpacing + pinSize)) - pinSpacing;
    const auto spaceNeededOuts = (numOuts * (pinSpacing + pinSize)) - pinSpacing;

    if (vertical)
    {
        int startX = box.getX() + 9;
        int startXOuts = startX;
        switch (_portAlign)
        {
            case PortsBefore:
                // noop
                break;
            case PortsMiddle:
                startX = startXOuts = box.getX() + ((box.getWidth() - spaceNeeded) / 2);
                break;
            case PortsAfter:
                startX = box.getRight() - spaceNeeded - 9;
                startXOuts = box.getRight() - spaceNeededOuts - 9;
                break;
        }

        Rectangle<int> pri (startX, 0, getWidth(), pinSize);
        Rectangle<int> pro (startXOuts, getHeight() - pinSize, getWidth(), pinSize);

        for (int i = 0; i < getNumChildComponents(); ++i)
        {
            if (PortComponent* const pc = dynamic_cast<PortComponent*> (getChildComponent (i)))
            {
                pc->setBounds (pc->isInput() ? pri.removeFromLeft (pinSize)
                                             : pro.removeFromLeft (pinSize));
                pc->isInput() ? pri.removeFromLeft (pinSpacing)
                              : pro.removeFromLeft (pinSpacing);
            }
        }
    }
    else
    {
        const int padY = displayMode == Compact || displayMode == Small ? 8 : 22;
        int startY = box.getY() + padY;
        int startYOuts = startY;

        switch (_portAlign)
        {
            case PortsBefore:
                startY = startYOuts = box.getY() + padY;
                break;
            case PortsMiddle:
                startY = startYOuts = box.getY() + ((box.getHeight() - spaceNeeded) / 2);
                break;
            case PortsAfter:
                startY = box.getBottom() - spaceNeeded - padY;
                startYOuts = box.getBottom() - spaceNeededOuts - padY;
                break;
        }

        Rectangle<int> pri (box.getX() - halfPinSize,
                            startY,
                            pinSize,
                            box.getHeight());
        Rectangle<int> pro (pri.withY (startYOuts)
                                .withX (box.getWidth() - 1));

        for (int i = 0; i < getNumChildComponents(); ++i)
        {
            if (PortComponent* const pc = dynamic_cast<PortComponent*> (getChildComponent (i)))
            {
                pc->setBounds (pc->isInput() ? pri.removeFromTop (pinSize)
                                             : pro.removeFromTop (pinSize));
                pc->isInput() ? pri.removeFromTop (pinSpacing)
                              : pro.removeFromTop (pinSpacing);
            }
        }
    }
}

bool BlockComponent::getPortPos (const int index, const bool isInput, float& x, float& y)
{
    bool res = false;

    for (int i = 0; i < getNumChildComponents(); ++i)
    {
        if (auto* const pc = dynamic_cast<PortComponent*> (getChildComponent (i)))
        {
            if (pc->getPortIndex() == (uint32_t) index && isInput == pc->isInput())
            {
                x = getX() + pc->getX() + pc->getWidth() * 0.5f;
                y = getY() + pc->getY() + pc->getHeight() * 0.5f;
                res = true;
                break;
            }
        }
    }

    return res;
}

void BlockComponent::update (const bool doPosition, const bool forcePins)
{
    auto* const ged = getGraphPanel();
    if (nullptr == ged)
    {
        jassertfalse;
        return;
    }

    if (! node.data().getParent().hasType (tags::nodes))
    {
        delete this;
        return;
    }

    vertical = ged->isLayoutVertical();

    setDisplayMode (getDisplayModeFromString (displayModeValue.getValue()));

    setName (node.getDisplayName());
    updatePins (forcePins);
    // update size relies on port counts being updated.
    if (displayMode != Embed)
        updateSize();

    if (doPosition)
    {
        updatePosition();
    }

    if (node.getUIValueTree().hasProperty ("color"))
    {
        color = Colour::fromString (node.getUIValueTree().getProperty ("color").toString());
    }
    else
    {
        /* Default: no fill bar — category lives on the outline border
         * (see typeColor lambda in paint()), not the title-bar fill. */
        color = Colour (0x00000000);
    }

    repaint();
    resized();
}

void BlockComponent::getMinimumSize (int& width, int& height)
{
    auto* ged = getGraphPanel();
    if (! ged)
        return;

    int w = roundToInt ((! vertical ? 120.0 : 90) * ged->getZoomScale());
    int h = roundToInt (46.0 * ged->getZoomScale());
    const int maxPorts = jmax (numIns, numOuts) + 1;
    font.setHeight (11.f * ged->getZoomScale());
    GlyphArrangement glyphs;
    glyphs.addLineOfText (font, node.getDisplayName(), 0, 0);
    int textWidth = (int) glyphs.getBoundingBox (0, -1, true).getWidth();
    textWidth += (vertical) ? 20 : 36;
    pinSpacing = int (pinSize * (displayMode == Compact || displayMode == Small ? 0.5f : 0.9f));
    int pinSpaceNeeded = int (maxPorts * pinSize) + int (maxPorts * pinSpacing);

    if (vertical)
    {
        w = std::max (w, int (maxPorts * pinSize) + int (maxPorts * pinSpacing));
        h = 60;

        if (displayMode == Compact)
        {
            h = (pinSize * 2) + 24;
        }

        w = std::max (w, textWidth);
    }
    else
    {
        if (displayMode == Compact)
        {
            w = (pinSize * 2) + 24;
            auto pinSpace = pinSpaceNeeded + pinSize;
            if (pinSpace >= textWidth)
                h = pinSpace;
            else
                h = textWidth;
        }
        else if (displayMode == Small)
        {
            w = textWidth + 6;
            h = pinSpaceNeeded + pinSize;
        }
        else
        {
            int endcap = displayMode == Compact ? 9 : 12;
            w = jmax (w, textWidth);
            h = jmax (h, (maxPorts * pinSize) + (maxPorts * jmax (pinSpacing, 2) + endcap));
        }
    }

    width = w;
    height = h;
}

void BlockComponent::updateSize()
{
    auto* ged = getGraphPanel();
    if (! ged)
        return;

    customWidth = (int) node.getBlockValueTree()
                      .getProperty (tags::width, customWidth);
    customHeight = (int) node.getBlockValueTree()
                       .getProperty (tags::height, customHeight);

    int minW = 0, minH = 0;
    getMinimumSize (minW, minH);
    jassert (minW > 0 && minH > 0);

    const auto r1 = getBoundsInParent();

    switch (displayMode)
    {
        case Normal:
        case Compact:
        case Small: {
            setSize (minW, minH);
            break;
        }

        case Embed: {
            if (embedded != nullptr)
            {
                if (detail::canResize (*this) && customWidth > 0 && customHeight > 0)
                {
                    setSize (customWidth, customHeight);
                    resized();
                }
                else
                {
                    setSize (embedded->getWidth() + pinSize,
                             embedded->getHeight() + pinSize + (vertical ? 20 : 18) + 18);
                }
            }
            break;
        }
    }

    if (r1 != getBoundsInParent())
    {
        if (auto panel = getGraphPanel())
            panel->updateConnectorComponents (true);
    }
}

void BlockComponent::setCustomSize (int width, int height)
{
    int mw = width, mh = height;
    getMinimumSize (mw, mh);
    if (width < mw)
        width = mw;
    if (height < mh)
        height = mh;

    if (customWidth != width || customHeight != height)
    {
        customWidth = width;
        customHeight = height;
        node.getBlockValueTree()
            .setProperty (tags::width, customWidth, nullptr)
            .setProperty (tags::height, customHeight, nullptr);

        if (displayMode == Small || displayMode == Compact)
        {
            displayMode = Normal;
            displayModeValue.removeListener (this);
            displayModeValue.setValue (getDisplayModeKey (displayMode));
            displayModeValue.addListener (this);
        }

        setSize (customWidth, customHeight);
    }
}

void BlockComponent::setNodePosition (const int x, const int y)
{
    if (vertical)
    {
        node.setRelativePosition ((x + getWidth() / 2) / (double) getParentWidth(),
                                  (y + getHeight() / 2) / (double) getParentHeight());
        node.setProperty (tags::x, (double) x);
        node.setProperty (tags::y, (double) y);
    }
    else
    {
        node.setRelativePosition ((y + getHeight() / 2) / (double) getParentHeight(),
                                  (x + getWidth() / 2) / (double) getParentWidth());
        node.setProperty (tags::y, (double) x);
        node.setProperty (tags::x, (double) y);
    }
}

Point<double> BlockComponent::getNodePosition() const noexcept
{
    Point<double> pos;
    node.getPosition (pos.x, pos.y);
    return pos;
}

void BlockComponent::updatePosition()
{
    if (! node.isValid())
        return;

    double x = 0.0, y = 0.0;
    auto* const panel = getGraphPanel();
    Component* parent = nullptr;
    if (panel != nullptr)
        parent = panel->findParentComponentOfClass<Viewport>();
    if (parent == nullptr)
        parent = panel;

    if (parent->getWidth() <= 0 || parent->getHeight() <= 0)
        return;

    if (! node.hasPosition() && nullptr != parent)
    {
        node.getRelativePosition (x, y);
        x = x * (parent->getWidth()) - (getWidth() / 2);
        y = y * (parent->getHeight()) - (getHeight() / 2);
        node.setPosition (x, y);
    }
    else
    {
        node.getPosition (x, y);
    }

    setBounds ({ roundToInt (vertical ? x : y),
                 roundToInt (vertical ? y : x),
                 getWidth(),
                 getHeight() });
}

void BlockComponent::updatePins (bool force)
{
    int numInputs = 0, numOutputs = 0;
    const auto numPorts = node.getNumPorts();
    for (int i = 0; i < numPorts; ++i)
    {
        const Port port (node.getPort (i));
        if (port.isHiddenOnBlock())
            continue;

        if (port.isInput())
            ++numInputs;
        else
            ++numOutputs;
    }

    if (force || numIns != numInputs || numOuts != numOutputs)
    {
        numIns = numInputs;
        numOuts = numOutputs;

        deleteAllPins();

        for (int i = 0; i < numPorts; ++i)
        {
            const Port port (node.getPort (i));
            const PortType t (port.getType());
            if (port.isHiddenOnBlock())
                continue;

            const bool isInput (port.isInput());
            addAndMakeVisible (new PortComponent (graph, node, filterID, i, isInput, t, vertical));
        }

        resized();
    }
}

void BlockComponent::setButtonVisible (Button& b, bool v)
{
    if (b.isVisible() == v)
        return;
    b.setVisible (v);
    resized();
}

GraphEditorComponent* BlockComponent::getGraphPanel() const noexcept
{
    return findParentComponentOfClass<GraphEditorComponent>();
}

void BlockComponent::addDisplaySubmenu (PopupMenu& menuToAddTo)
{
    PopupMenu dMenu;
    const auto block = node.getBlockValueTree();
    const auto mode = BlockComponent::getDisplayModeFromString (
        block.getProperty (tags::displayMode).toString());

    for (int i = 0; i <= BlockComponent::Embed; ++i)
    {
        const auto m = static_cast<BlockComponent::DisplayMode> (i);
        const bool enabled = m == BlockComponent::Embed ? detail::supportsEmbed (node) : true;
        dMenu.addItem (BlockComponent::getDisplayModeName (m), enabled, mode == m, [this, block, m]() {
            auto b = block;
            b.setProperty (tags::displayMode, BlockComponent::getDisplayModeKey (m), nullptr);
            forEachSibling ([m] (BlockComponent& sibling) {
                if (! sibling.isSelected())
                    return;
                auto sb = sibling.node.getBlockValueTree();
                sb.setProperty (tags::displayMode, BlockComponent::getDisplayModeKey (m), nullptr);
            });

            if (auto* gp = getGraphPanel())
                gp->updateConnectorComponents (true);
        });
    }

    dMenu.addSeparator();
    dMenu.addSectionHeader ("Port Alignment");
    for (int i = PortsBefore; i <= PortsAfter; ++i)
    {
        const auto m = static_cast<BlockComponent::PortAlignment> (i);
        const bool enabled = true;
        dMenu.addItem (portAlignmentName (m, vertical), enabled, m == _portAlign, [this, block, m]() {
            auto b = block;
            b.setProperty (tags::portAlignment, portAlignmentKey (m), nullptr);
            this->_portAlign = m;
            resized();
            forEachSibling ([m] (BlockComponent& sibling) {
                if (! sibling.isSelected())
                    return;
                auto sb = sibling.node.getBlockValueTree();
                sb.setProperty (tags::portAlignment, BlockComponent::portAlignmentKey (m), nullptr);
                sibling._portAlign = m;
                sibling.resized();
            });

            if (auto* gp = getGraphPanel())
                gp->updateConnectorComponents (true);
        });
    }
    menuToAddTo.addSubMenu (TRANS ("Display"), dMenu);
}

bool BlockComponent::isInterestedInDragSource (const SourceDetails& details)
{
    if (! node.isA (EL_NODE_FORMAT_NAME, EL_NODE_ID_PLACEHOLDER))
        return false;
    if (! details.description.isArray())
        return false;
    if (auto* a = details.description.getArray())
    {
        const var type (a->getFirst());
        return type == var ("plugin");
    }
    return false;
}

void BlockComponent::itemDropped (const SourceDetails& details)
{
    if (const auto* a = details.description.getArray())
    {
        auto& plugs (ViewHelpers::getGlobals (this)->plugins());
        if (const auto t = plugs.getKnownPlugins().getTypeForIdentifierString (a->getUnchecked (1).toString()))
            if (auto panel = getGraphPanel())
                panel->postMessage (new ReplaceNodeMessage (node, *t));
    }
}

} // namespace element
