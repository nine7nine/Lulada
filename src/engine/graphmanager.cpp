// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <element/context.hpp>
#include <element/plugins.hpp>

#include "engine/graphmanager.hpp"
#include "nodes/audioprocessor.hpp"
#include "nodes/audiorouter.hpp"
#include "nodes/midichannelsplitter.hpp"
#include "nodes/midiprogrammap.hpp"
#include "nodes/placeholder.hpp"
#include "engine/rootgraph.hpp"

#include "utils.hpp"

namespace element {

//==============================================================================
static void showFailedInstantiationAlert (const PluginDescription& desc, const bool async = false)
{
    String header = "Plugin Instantiation Failed";
    String message = desc.name;
    message << " could not be instantiated";

    if (async)
        AlertWindow::showMessageBoxAsync (AlertWindow::WarningIcon, header, message);
    else
        AlertWindow::showMessageBox (AlertWindow::WarningIcon, header, message);
}

//==============================================================================
static void removeCoordinatesProperties (ValueTree data)
{
    data.removeProperty (tags::relativeX, nullptr);
    data.removeProperty (tags::relativeY, nullptr);
}

static void removeWindowProperties (ValueTree data)
{
    data.removeProperty (tags::windowX, nullptr);
    data.removeProperty (tags::windowY, nullptr);
    data.removeProperty (tags::windowVisible, nullptr);
}

//==============================================================================
/** This enforces correct IO nodes based on the graph processor's settings
    in virtual methods like 'acceptsMidi' and 'getTotalNumInputChannels'
    It uses the controller for all node operations so the model will
    stay in sync.
  */
struct IONodeEnforcer
{
    IONodeEnforcer (GraphManager& m)
        : manager (m)
    {
        addMissingIONodes();
    }

private:
    GraphManager& manager;
    void addMissingIONodes()
    {
        auto& graph (manager.getGraph());
        Node model (manager.getGraphModel());

        const bool wantsAudioIn = graph.getNumPorts (PortType::Audio, true) > 0;
        const bool wantsAudioOut = graph.getNumPorts (PortType::Audio, false) > 0;
        /* Element: pseudo-node auto-management is driven by the
         * graph's abstract port count.  On the JACK build new graphs
         * default to 0 MIDI ports (see Node::createDefaultGraph) and
         * the right-click + Graph properties surfaces for adding them
         * are hidden, so the auto-creation path stays dormant for
         * fresh sessions.  Existing sessions with MIDI pseudo-nodes
         * continue to work — they're the only path for routing
         * non-JACK MIDI sources (virtual keyboard, generated MIDI
         * clock routed to input, plugin-mode host MIDI) into the
         * graph. */
        const bool wantsMidiIn = graph.getNumPorts (PortType::Midi, true) > 0;
        const bool wantsMidiOut = graph.getNumPorts (PortType::Midi, false) > 0;

        ProcessorPtr ioNodes[IONode::numDeviceTypes];
        for (int i = 0; i < manager.getNumNodes(); ++i)
        {
            ProcessorPtr node = manager.getNode (i);
            if (auto* ioProc = dynamic_cast<IONode*> (node.get()))
                ioNodes[ioProc->getType()] = node;
        }

        Array<uint32> nodesToRemove;

        for (int t = 0; t < IONode::numDeviceTypes; ++t)
        {
            if (nullptr != ioNodes[t])
            {
                if (t == IONode::audioInputNode && ! wantsAudioIn)
                    nodesToRemove.add (ioNodes[t]->nodeId);
                if (t == IONode::audioOutputNode && ! wantsAudioOut)
                    nodesToRemove.add (ioNodes[t]->nodeId);
                ;
                if (t == IONode::midiInputNode && ! wantsMidiIn)
                    nodesToRemove.add (ioNodes[t]->nodeId);
                ;
                if (t == IONode::midiOutputNode && ! wantsMidiOut)
                    nodesToRemove.add (ioNodes[t]->nodeId);
                ;
                continue;
            }

            if (t == IONode::audioInputNode && ! wantsAudioIn)
                continue;
            if (t == IONode::audioOutputNode && ! wantsAudioOut)
                continue;
            if (t == IONode::midiInputNode && ! wantsMidiIn)
                continue;
            if (t == IONode::midiOutputNode && ! wantsMidiOut)
                continue;

            PluginDescription desc;
            desc.pluginFormatName = "Internal";
            double rx = 0.5f, ry = 0.5f;
            switch (t)
            {
                case IONode::audioInputNode:
                    desc.fileOrIdentifier = "audio.input";
                    rx = .25;
                    ry = .25;
                    break;
                case IONode::audioOutputNode:
                    desc.fileOrIdentifier = "audio.output";
                    rx = .25;
                    ry = .75;
                    break;
                case IONode::midiInputNode:
                    desc.fileOrIdentifier = "midi.input";
                    rx = .75;
                    ry = .25;
                    break;
                case IONode::midiOutputNode:
                    desc.fileOrIdentifier = "midi.output";
                    rx = .75;
                    ry = .75;
                    break;
            }

            auto nodeId = manager.addNode (&desc, rx, ry);
            ioNodes[t] = manager.getNodeForId (nodeId);
            jassert (ioNodes[t] != nullptr);
        }

        for (const auto& nodeId : nodesToRemove)
            manager.removeNode (nodeId);

        model.resetPorts();
    }
};

//==============================================================================
class NodeModelUpdater : public ReferenceCountedObject
{
public:
    NodeModelUpdater (GraphManager& m, const ValueTree& d, Processor* o)
        : manager (m), data (d), object (o)
    {
        jassert (object != nullptr);
        if (object)
            portsChangedConnection = object->portsChanged.connect (
                std::bind (&NodeModelUpdater::onPortsChanged, this));
    }

    ~NodeModelUpdater()
    {
        portsChangedConnection.disconnect();
    }

private:
    GraphManager& manager;
    ValueTree data;
    ProcessorPtr object;
    SignalConnection portsChangedConnection;

    void onPortsChanged()
    {
        const auto newPorts = object->createPortsData();
        int index = data.indexOf (data.getChildWithName (tags::ports));
        if (newPorts.isValid())
        {
            if (index >= 0)
                data.removeChild (index, nullptr);
            else
                index = -1;

            data.addChild (newPorts, index, nullptr);
            manager.removeIllegalConnections();
        }
        if (object->isGraph())
        {
            IONodeEnforcer enforce (manager);
        }
        manager.syncArcsModel();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NodeModelUpdater)
};

//==============================================================================
class GraphManager::Binding
{
public:
    Binding() = delete;
    Binding (GraphManager& g, ProcessorPtr o, const Node& n)
        : owner (g),
          object (o),
          node (n)
    {
        data = node.data();
        setDataProperties();

        if (node.getNodeType() == types::Graph && object && object->isGraph())
        {
            auto sub = dynamic_cast<GraphNode*> (object.get());
            jassert (sub);

            for (auto portType : { PortType::Audio, PortType::Midi })
            {
                PortArray savedIns, savedOuts;
                node.getPorts (savedIns, savedOuts, portType);
                if (savedIns.size() != sub->getNumPorts (portType, true))
                {
                    sub->setNumPorts (portType, savedIns.size(), true, false);
                }
                if (savedOuts.size() != sub->getNumPorts (portType, false))
                {
                    sub->setNumPorts (portType, savedOuts.size(), false, false);
                }
            }

            manager = std::make_unique<GraphManager> (*sub, owner.pluginManager);
            manager->setNodeModel (node);
            IONodeEnforcer addIO (*manager);
        }
        else
        {
            if (node.getNodeType() != types::Graph)
            {
                DBG ("[element] data type is not a graph");
            }
            if (object && ! object->isGraph())
            {
                DBG ("[element] object is not a graph type");
            }
        }
    }

    ~Binding()
    {
        if (manager != nullptr)
        {
            manager.reset();
        }

        node = Node();
        Node::sanitizeRuntimeProperties (data);
        data = ValueTree();
        object = nullptr;
    }

    GraphManager* getSubGraphManager() const { return manager.get(); }

    void setDataProperties()
    {
        if (! data.isValid() || object == nullptr)
            return;
        data.setProperty (tags::id, static_cast<int64> (object->nodeId), nullptr)
            .setProperty (tags::object, object.get(), nullptr)
            .setProperty (tags::name, object->getName(), nullptr);
    }

private:
    friend class GraphManager;
    GraphManager& owner;
    ProcessorPtr object;
    Node node;
    ValueTree data;
    std::unique_ptr<GraphManager> manager;
    [[maybe_unused]] UndoManager* undo = nullptr;
};

//==============================================================================
GraphManager::GraphManager (GraphNode& pg, PluginManager& pm)
    : pluginManager (pm), processor (pg), lastUID (0)
{
}

GraphManager::~GraphManager()
{
    // Make sure to dereference Processor's so we don't leak memory
    // If you get warnings by juce's leak detector about graph related
    // objects, then there's probably "object" properties lingering that
    // are referenced in the model;
    Node::sanitizeRuntimeProperties (graph, true);
    graph = arcs = nodes = ValueTree();
}

uint32 GraphManager::getNextUID() noexcept
{
    return ++lastUID;
}

int GraphManager::getNumNodes() const noexcept { return processor.getNumNodes(); }
const ProcessorPtr GraphManager::getNode (const int index) const noexcept { return processor.getNode (index); }

const ProcessorPtr GraphManager::getNodeForId (const uint32 uid) const noexcept
{
    return processor.getNodeForId (uid);
}

const Node GraphManager::getNodeModelForId (const uint32 nodeId) const noexcept
{
    return Node (nodes.getChildWithProperty (tags::id, static_cast<int64> (nodeId)), false);
}

GraphManager* GraphManager::findGraphManagerForGraph (const Node& graph) const noexcept
{
    if (isManaging (graph))
        return const_cast<GraphManager*> (this);

    for (auto* binding : bindings)
    {
        if (auto* m1 = binding->getSubGraphManager())
            if (auto* m2 = m1->findGraphManagerForGraph (graph))
                return m2;
    }

    return nullptr;
}

bool GraphManager::contains (const uint32 nodeId) const
{
    return processor.getNodeForId (nodeId) != nullptr;
}

Processor* GraphManager::createFilter (const PluginDescription* desc, double x, double y, uint32 nodeId)
{
    String errorMessage;
    auto node = std::unique_ptr<Processor> (
        pluginManager.createGraphNode (*desc, errorMessage));

    if (errorMessage.isNotEmpty())
    {
        std::cerr << "[element] error creating audio plugin: " << errorMessage.toStdString() << std::endl;
        jassert (node == nullptr);
    }

    if (node == nullptr && errorMessage.isEmpty())
    {
        jassertfalse;
        errorMessage = "Could not find node";
    }

    return node != nullptr ? processor.addNode (node.release(), nodeId) : nullptr;
}

Processor* GraphManager::createPlaceholder (const Node& node)
{
    auto* ph = new PlaceholderProcessor();
    ph->setupFor (node, processor.getSampleRate(), processor.getBlockSize());
    return processor.addNode (new AudioProcessorNode (node.getNodeId(), ph), node.getNodeId());
}

//==============================================================================
uint32 GraphManager::addNode (const Node& newNode)
{
    if (! newNode.isValid())
    {
        AlertWindow::showMessageBox (AlertWindow::WarningIcon,
                                     TRANS ("Couldn't create Node"),
                                     "Cannot instantiate node without a description");
        return EL_INVALID_NODE;
    }

    uint32 nodeId = EL_INVALID_NODE;
    const PluginDescription desc (pluginManager.findDescriptionFor (newNode));
    if (auto* node = createFilter (&desc, 0, 0, newNode.hasProperty (tags::id) ? newNode.getNodeId() : 0))
    {
        nodeId = node->nodeId;
        ValueTree data = newNode.data().createCopy();
        removeCoordinatesProperties (data);
        removeWindowProperties (data);

        data.setProperty (tags::id, static_cast<int64> (nodeId), nullptr)
            .setProperty (tags::object, node, nullptr)
            .setProperty (tags::type, node->getTypeString(), nullptr)
            .setProperty (tags::pluginIdentifierString, desc.createIdentifierString(), nullptr);

        setupNode (data, node);

        nodes.addChild (data, -1, nullptr);
        changed();
    }
    else
    {
        nodeId = EL_INVALID_NODE;
        AlertWindow::showMessageBox (AlertWindow::WarningIcon, "Couldn't create filter", "The plugin could not be instantiated");
    }

    return nodeId;
}

uint32 GraphManager::addNode (const PluginDescription* desc, double rx, double ry, uint32 nodeId)
{
    if (! desc)
    {
        AlertWindow::showMessageBox (AlertWindow::WarningIcon,
                                     TRANS ("Couldn't create filter"),
                                     TRANS ("Cannot instantiate plugin without a description"));
        return EL_INVALID_NODE;
    }

    if (auto* object = createFilter (desc, rx, ry, nodeId))
    {
        nodeId = finalizeAddedNode (object, desc, rx, ry);
    }
    else
    {
        nodeId = EL_INVALID_NODE;
        showFailedInstantiationAlert (*desc, true);
    }

    return nodeId;
}

uint32 GraphManager::finalizeAddedNode (Processor* object, const PluginDescription* desc, double rx, double ry)
{
    /* Element: extracted from addNode(PluginDescription*, ...) so the
     * async path (addNodeAsync) can share the same post-load setup
     * after the plugin instance comes back from the worker thread.
     * Strictly message-thread + side-effect heavy (graph model
     * updates, port discovery, bus-layout negotiation) — do not call
     * off the message thread. */
    if (object == nullptr || desc == nullptr)
        return EL_INVALID_NODE;

    uint32 nodeId = object->nodeId;
    ValueTree data = ! object->isGraph() ? ValueTree (types::Node)
                                         : Node::createDefaultGraph (desc->name).data();

    data.setProperty (tags::id, static_cast<int64> (nodeId), nullptr)
        .setProperty (tags::format, desc->pluginFormatName, nullptr)
        .setProperty (tags::identifier, desc->fileOrIdentifier, nullptr)
        .setProperty (tags::type, object->getTypeString(), nullptr)
        .setProperty (tags::name, desc->name, nullptr)
        .setProperty (tags::object, object, nullptr)
        .setProperty (tags::updater, new NodeModelUpdater (*this, data, object), nullptr)
        .setProperty (tags::relativeX, rx, nullptr)
        .setProperty (tags::relativeY, ry, nullptr)
        .setProperty (tags::pluginIdentifierString,
                      desc->createIdentifierString(),
                      nullptr);

    Node node (data, true);
    jassert (node.getFormat().toString().isNotEmpty());
    jassert (node.getIdentifier().toString().isNotEmpty());
    node.resetPorts();

    if (node.isIONode())
    {
        node.getBlockValueTree().setProperty (tags::displayMode, "compact", nullptr);
    }

    PortArray pins, pouts;
    std::vector<PortType> toHide = {
        PortType::Control, PortType::CV, PortType::Video, PortType::Event
    };

    for (const auto& pt : toHide)
    {
        node.getPorts (pins, pouts, pt);
    }

    /* Element: batched hidden-port write.  The per-Port
     * setHiddenOnBlock helper re-parses + re-joins the entire
     * hiddenPorts property string for EVERY call, which is an
     * O(N²) string churn that took ~4.6s on a heavy VST3 like
     * Zebra (~700 control parameters).
     *
     * Build the full symbol list in one pass and write the
     * hiddenPorts property ONCE at the end.  Keep the per-port
     * "hiddenOnBlock" flag writes — they're cheap HERE (the node's
     * ValueTree is not yet added to the graph, so no listeners are
     * attached) but if we defer them via the lazy init in
     * Port::isHiddenOnBlock, the writes happen LATER inside the
     * graph tree where listeners are attached → the cost moves to
     * nodes.addChild's listener cascade (7s+) instead of disappearing.
     *
     * Net cost on Zebra: ~480ms vs the original ~4600ms, with no
     * downstream cost shifted elsewhere. */
    {
        auto blockTree = node.getBlockValueTree();
        StringArray hidden = StringArray::fromTokens (
            blockTree.getProperty ("hiddenPorts").toString(), ",", "\"\'");
        hidden.trim();

        auto markHidden = [&] (Port& c) {
            hidden.addIfNotAlreadyThere (c.symbol());
            c.setProperty ("hiddenOnBlock", true);
        };
        for (auto& c : pins)  markHidden (c);
        for (auto& c : pouts) markHidden (c);

        blockTree.setProperty ("hiddenPorts", hidden.joinIntoString (","), nullptr);
    }

    if (object->isSubGraph())
    {
        bindings.add (new Binding (*this, object, node));
    }

    if (auto* const proc = object->getAudioProcessor())
    {
        // try to use stereo by default on newly added plugins
        AudioProcessor::BusesLayout stereoInOut;
        stereoInOut.inputBuses.add (AudioChannelSet::stereo());
        stereoInOut.outputBuses.add (AudioChannelSet::stereo());
        AudioProcessor::BusesLayout stereoOut;
        stereoOut.outputBuses.add (AudioChannelSet::stereo());
        AudioProcessor::BusesLayout* tryStereo = nullptr;
        const auto oldLayout = proc->getBusesLayout();

        if (proc->getTotalNumInputChannels() == 1 && proc->getTotalNumOutputChannels() == 1 && proc->checkBusesLayoutSupported (stereoInOut))
        {
            tryStereo = &stereoInOut;
        }
        else if (proc->getTotalNumInputChannels() == 0 && proc->getTotalNumOutputChannels() == 1 && proc->checkBusesLayoutSupported (stereoOut))
        {
            tryStereo = &stereoOut;
        }

        if (tryStereo != nullptr && proc->checkBusesLayoutSupported (*tryStereo))
        {
            proc->suspendProcessing (true);
            proc->releaseResources();

            if (! proc->setBusesLayout (*tryStereo))
                proc->setBusesLayout (oldLayout);

            proc->prepareToPlay (processor.getSampleRate(), processor.getBlockSize());
            proc->suspendProcessing (false);
        }
    }

    nodes.addChild (data, -1, nullptr);
    changed();
    return nodeId;
}

void GraphManager::addNodeAsync (const PluginDescription* desc, double rx, double ry, uint32 nodeId,
                                 std::function<void (uint32)> callback)
{
    if (! desc)
    {
        AlertWindow::showMessageBox (AlertWindow::WarningIcon,
                                     TRANS ("Couldn't create filter"),
                                     TRANS ("Cannot instantiate plugin without a description"));
        if (callback) callback (EL_INVALID_NODE);
        return;
    }

    /* Copy the description into a value held by the callback —
     * createGraphNodeAsync's worker thread keeps a reference and the
     * caller's `desc` pointer might not survive the async hop.
     * WeakReference guards against `this` being destroyed (session
     * close, app quit) between the load starting and the callback
     * landing on the message thread — if invalidated the callback
     * deletes the orphaned Processor* and bails out instead of
     * touching freed manager state. */
    auto descCopy = std::make_shared<PluginDescription> (*desc);
    WeakReference<GraphManager> safeThis (this);

    pluginManager.createGraphNodeAsync (
        *descCopy,
        [safeThis, descCopy, rx, ry, nodeId, cb = std::move (callback)] (Processor* rawNode, const String& errorMsg) mutable
        {
            if (safeThis.wasObjectDeleted())
            {
                delete rawNode;
                if (cb) cb (EL_INVALID_NODE);
                return;
            }

            if (rawNode == nullptr)
            {
                if (errorMsg.isNotEmpty())
                    std::cerr << "[element] error creating audio plugin: "
                              << errorMsg.toStdString() << std::endl;
                showFailedInstantiationAlert (*descCopy, true);
                if (cb) cb (EL_INVALID_NODE);
                return;
            }

            auto* object = safeThis->processor.addNode (rawNode, nodeId);
            if (object == nullptr)
            {
                if (cb) cb (EL_INVALID_NODE);
                return;
            }

            /* Element: split finalize across threads.
             *
             *   Part A (message thread, here)    — metadata + resetPorts
             *                                      + IONode check.  ~70ms
             *                                      on a 700-param VST3.
             *                                      resetPorts MUST run on
             *                                      the message thread —
             *                                      JUCE's VST3 wrapper
             *                                      takes MessageManagerLock
             *                                      around per-parameter
             *                                      getText / getValueForText
             *                                      calls.  From the message
             *                                      thread those MMLocks are
             *                                      a no-op; from a worker
             *                                      they'd stall the GUI.
             *
             *   Worker (juce::Thread::launch)    — hidePorts loop.  Pure
             *                                      ValueTree property writes
             *                                      on a not-yet-attached
             *                                      tree (no listeners
             *                                      attached, no plugin
             *                                      calls).  Safe to run off
             *                                      the message thread.  Was
             *                                      ~414ms on a 700-param
             *                                      VST3 — the dominant
             *                                      residual freeze.
             *
             *   Part B (message thread via       — subgraph binding +
             *           MessageManager::callAsync) stereo bus negotiation
             *                                      + nodes.addChild +
             *                                      changed().  ~26ms,
             *                                      attaches the tree to
             *                                      the graph (UI listeners
             *                                      fire here). */
            safeThis->finalizeAddedNodeAsync (object, descCopy, rx, ry, std::move (cb));
        });
}

void GraphManager::finalizeAddedNodeAsync (Processor* object,
                                           std::shared_ptr<PluginDescription> desc,
                                           double rx, double ry,
                                           std::function<void (uint32)> callback)
{
    if (object == nullptr || desc == nullptr)
    {
        if (callback) callback (EL_INVALID_NODE);
        return;
    }

    /* ---------- Part A (message thread): metadata + resetPorts ---------- */
    const uint32 nodeId = object->nodeId;
    ValueTree data = ! object->isGraph() ? ValueTree (types::Node)
                                         : Node::createDefaultGraph (desc->name).data();

    data.setProperty (tags::id, static_cast<int64> (nodeId), nullptr)
        .setProperty (tags::format, desc->pluginFormatName, nullptr)
        .setProperty (tags::identifier, desc->fileOrIdentifier, nullptr)
        .setProperty (tags::type, object->getTypeString(), nullptr)
        .setProperty (tags::name, desc->name, nullptr)
        .setProperty (tags::object, object, nullptr)
        .setProperty (tags::updater, new NodeModelUpdater (*this, data, object), nullptr)
        .setProperty (tags::relativeX, rx, nullptr)
        .setProperty (tags::relativeY, ry, nullptr)
        .setProperty (tags::pluginIdentifierString,
                      desc->createIdentifierString(),
                      nullptr);

    Node node (data, true);
    jassert (node.getFormat().toString().isNotEmpty());
    jassert (node.getIdentifier().toString().isNotEmpty());
    node.resetPorts();

    if (node.isIONode())
        node.getBlockValueTree().setProperty (tags::displayMode, "compact", nullptr);

    /* Snapshot the port lists on the message thread — getPorts is a
     * ValueTree traversal that's safe from any thread, but doing it
     * here keeps the worker's payload simple and avoids re-walking the
     * tree later. */
    auto pins  = std::make_shared<PortArray>();
    auto pouts = std::make_shared<PortArray>();
    {
        const std::vector<PortType> toHide = {
            PortType::Control, PortType::CV, PortType::Video, PortType::Event
        };
        for (const auto& pt : toHide)
            node.getPorts (*pins, *pouts, pt);
    }

    /* ---------- Worker thread: hidePorts batched write ---------- */
    WeakReference<GraphManager> safeThis (this);

    juce::Thread::launch (
        [safeThis, data, node, pins, pouts, object, desc, rx, ry, cb = std::move (callback)]() mutable
        {
            /* The hidePorts loop is pure ValueTree property-write work
             * — no plugin calls (no Wine APIs), so a plain pthread is
             * safe here (unlike the LoadLibrary path which needs Win32
             * CreateThread to register the thread with Wine).  The data
             * tree has no listeners attached at this point (it isn't
             * inside `nodes` yet) so writes don't fire cascades.  Only
             * this worker touches the tree during this phase. */
            {
                auto blockTree = node.getBlockValueTree();
                StringArray hidden = StringArray::fromTokens (
                    blockTree.getProperty ("hiddenPorts").toString(), ",", "\"\'");
                hidden.trim();

                auto markHidden = [&] (Port& c) {
                    hidden.addIfNotAlreadyThere (c.symbol());
                    c.setProperty ("hiddenOnBlock", true);
                };
                for (auto& c : *pins)  markHidden (c);
                for (auto& c : *pouts) markHidden (c);

                blockTree.setProperty ("hiddenPorts", hidden.joinIntoString (","), nullptr);
            }

            /* ---------- Part B (message thread via callAsync) ---------- */
            juce::MessageManager::callAsync (
                [safeThis, data, node, object, desc, rx, ry, cb = std::move (cb)]() mutable
                {
                    if (safeThis.wasObjectDeleted())
                    {
                        if (cb) cb (EL_INVALID_NODE);
                        return;
                    }

                    if (object->isSubGraph())
                        safeThis->bindings.add (new Binding (*safeThis, object, node));

                    if (auto* const proc = object->getAudioProcessor())
                    {
                        AudioProcessor::BusesLayout stereoInOut;
                        stereoInOut.inputBuses .add (AudioChannelSet::stereo());
                        stereoInOut.outputBuses.add (AudioChannelSet::stereo());
                        AudioProcessor::BusesLayout stereoOut;
                        stereoOut.outputBuses.add (AudioChannelSet::stereo());
                        AudioProcessor::BusesLayout* tryStereo = nullptr;
                        const auto oldLayout = proc->getBusesLayout();

                        if (proc->getTotalNumInputChannels() == 1 && proc->getTotalNumOutputChannels() == 1
                                && proc->checkBusesLayoutSupported (stereoInOut))
                            tryStereo = &stereoInOut;
                        else if (proc->getTotalNumInputChannels() == 0 && proc->getTotalNumOutputChannels() == 1
                                && proc->checkBusesLayoutSupported (stereoOut))
                            tryStereo = &stereoOut;

                        if (tryStereo != nullptr && proc->checkBusesLayoutSupported (*tryStereo))
                        {
                            proc->suspendProcessing (true);
                            proc->releaseResources();

                            if (! proc->setBusesLayout (*tryStereo))
                                proc->setBusesLayout (oldLayout);

                            proc->prepareToPlay (safeThis->processor.getSampleRate(),
                                                 safeThis->processor.getBlockSize());
                            proc->suspendProcessing (false);
                        }
                    }

                    safeThis->nodes.addChild (data, -1, nullptr);
                    safeThis->changed();

                    if (cb) cb (object->nodeId);
                });
        });
}

void GraphManager::removeNode (const uint32 uid)
{
    if (! processor.removeNode (uid))
        return;
    for (int i = 0; i < nodes.getNumChildren(); ++i)
    {
        const Node node (nodes.getChild (i), false);
        if (node.getNodeId() == uid)
        {
            // the model was probably referencing the node ptr
            ProcessorPtr obj = node.getObject();
            if (obj)
            {
                obj->willBeRemoved();
                obj->releaseResources();
            }

            for (int i = bindings.size(); --i >= 0;)
            {
                auto binding = bindings.getUnchecked (i);
                if (binding->object == obj)
                    bindings.remove (i, true);
            }

            auto data = node.data();
            nodes.removeChild (data, nullptr);
            // clear all referecnce counted objects
            Node::sanitizeProperties (data, true);
            // finally delete the node + plugin instance.
            obj = nullptr;
        }
    }

    jassert (nodes.getNumChildren() == getNumNodes());
    processorArcsChanged();
}

//==============================================================================
void GraphManager::disconnectNode (const uint32 nodeId, const bool inputs, const bool outputs, const bool audio, const bool midi)
{
    jassert (inputs || outputs);
    bool doneAnything = false;

    for (int i = getNumConnections(); --i >= 0;)
    {
        const auto* const c = processor.getConnection (i);
        if ((outputs && c->sourceNode == nodeId) || (inputs && c->destNode == nodeId))
        {
            ProcessorPtr src = processor.getNodeForId (c->sourceNode);
            ProcessorPtr dst = processor.getNodeForId (c->destNode);

            if ((audio && src->getPortType (c->sourcePort) == PortType::Audio && dst->getPortType (c->destPort) == PortType::Audio) || (midi && src->getPortType (c->sourcePort) == PortType::Midi && dst->getPortType (c->destPort) == PortType::Midi))
            {
                removeConnection (i);
                doneAnything = true;
            }
        }
    }

    if (doneAnything)
        processorArcsChanged();
}

void GraphManager::removeIllegalConnections()
{
    if (processor.removeIllegalConnections())
        processorArcsChanged();
}

int GraphManager::getNumConnections() const noexcept
{
    jassert (arcs.getNumChildren() == processor.getNumConnections());
    return processor.getNumConnections();
}

const GraphNode::Connection* GraphManager::getConnection (const int index) const noexcept
{
    return processor.getConnection (index);
}

const GraphNode::Connection* GraphManager::getConnectionBetween (uint32 sourceFilterUID, int sourceFilterChannel, uint32 destFilterUID, int destFilterChannel) const noexcept
{
    return processor.getConnectionBetween (sourceFilterUID, sourceFilterChannel, destFilterUID, destFilterChannel);
}

bool GraphManager::canConnect (uint32 sourceFilterUID, int sourceFilterChannel, uint32 destFilterUID, int destFilterChannel) const noexcept
{
    return processor.canConnect (sourceFilterUID, sourceFilterChannel, destFilterUID, destFilterChannel);
}

bool GraphManager::addConnection (uint32 sourceFilterUID, int sourcePort, uint32 destFilterUID, int destPort)
{
    const bool result = processor.addConnection (sourceFilterUID, (uint32) sourcePort, destFilterUID, (uint32) destPort);
    if (result)
        processorArcsChanged();

    return result;
}

void GraphManager::removeConnection (const int index)
{
    processor.removeConnection (index);
    processorArcsChanged();
}

void GraphManager::removeConnection (uint32 sourceNode, uint32 sourcePort, uint32 destNode, uint32 destPort)
{
    if (processor.removeConnection (sourceNode, sourcePort, destNode, destPort))
        processorArcsChanged();
}

void GraphManager::setNodeModel (const Node& node)
{
    loaded = false;

    processor.clear();
    graph = node.data();
    arcs = node.getArcsValueTree();
    nodes = node.getNodesValueTree();

    if (graph.hasProperty (tags::updater))
    {
        graph.setProperty (tags::updater, (NodeModelUpdater*) nullptr, nullptr);
        graph.removeProperty (tags::updater, nullptr);
    }

    graph.setProperty (tags::updater, new NodeModelUpdater (*this, graph, &processor), nullptr);

    Array<ValueTree> failed;
    for (int i = 0; i < nodes.getNumChildren(); ++i)
    {
        Node node (nodes.getChild (i), false);
        const PluginDescription desc (pluginManager.findDescriptionFor (node));
        if (ProcessorPtr obj = createFilter (&desc, 0, 0, node.getNodeId()))
        {
            setupNode (node.data(), obj);
            obj->setEnabled (node.isEnabled());
            node.setProperty (tags::enabled, obj->isEnabled());
        }
        else if (ProcessorPtr ph = createPlaceholder (node))
        {
            DBG ("[element] couldn't create node: " << node.getName() << ". Creating offline placeholder");
            node.data().setProperty (tags::object, ph.get(), nullptr);
            node.data().setProperty (tags::missing, true, nullptr);
        }
        else
        {
            DBG ("[element] couldn't create node: " << node.getName());
            failed.add (node.data());
        }
    }

    for (const auto& n : failed)
    {
        nodes.removeChild (n, nullptr);
        Node::sanitizeRuntimeProperties (n);
    }
    failed.clearQuick();

    // If you hit this, then failed nodes didn't get handled properly
    jassert (nodes.getNumChildren() == processor.getNumNodes());

    // Cheap way to refresh engine-side nodes
    processor.triggerAsyncUpdate();
    processor.handleUpdateNowIfNeeded();

    for (int i = 0; i < arcs.getNumChildren(); ++i)
    {
        ValueTree arc (arcs.getChild (i));
        const auto sourceNode = (uint32) (int) arc.getProperty (tags::sourceNode);
        const auto destNode = (uint32) (int) arc.getProperty (tags::destNode);

#if 0
        auto src = processor.getNodeForId (sourceNode);
        auto dst = processor.getNodeForId (destNode);
        if (src && dst) {
            DBG("connection " << src->getName() << " to " << dst->getName());
        }
#endif
        bool worked = processor.addConnection (sourceNode, (uint32) (int) arc.getProperty (tags::sourcePort), destNode, (uint32) (int) arc.getProperty (tags::destPort));

        if (worked)
        {
            arc.removeProperty (tags::missing, 0);
        }
        else
        {
            DBG ("[element] failed creating connection: ");
            const Node graphObject (graph, false);

            if (graphObject.getNodeById (sourceNode).isValid() && graphObject.getNodeById (destNode).isValid())
            {
                DBG ("[element] set missing connection");
                // if the nodes are valid then preserve it
                arc.setProperty (tags::missing, true, 0);
            }
            else
            {
                DBG ("[element] purge failed arc");
                failed.add (arc);
            }
        }
    }

    const bool discardFailedConnections = true;
    if (discardFailedConnections)
        for (const auto& n : failed)
            arcs.removeChild (n, nullptr);

    loaded = true;
    jassert (arcs.getNumChildren() == processor.getNumConnections());
    failed.clearQuick();

    IONodeEnforcer enforceIONodes (*this);
    processorArcsChanged();
}

void GraphManager::savePluginStates()
{
    for (int i = 0; i < nodes.getNumChildren(); ++i)
    {
        Node node (nodes.getChild (i), false);
        node.savePluginState();
    }
}

void GraphManager::clear()
{
    loaded = false;

    if (graph.isValid())
    {
        Node::sanitizeRuntimeProperties (graph);
        graph.removeChild (arcs, nullptr);
        graph.removeChild (nodes, nullptr);
        nodes.removeAllChildren (nullptr);
        arcs.removeAllChildren (nullptr);
        graph.addChild (nodes, -1, nullptr);
        graph.addChild (arcs, -1, nullptr);
    }

    processor.clear();
    changed();
}

void GraphManager::processorArcsChanged()
{
    ValueTree newArcs = ValueTree (tags::arcs);
    for (int i = 0; i < processor.getNumConnections(); ++i)
        newArcs.addChild (Node::makeArc (*processor.getConnection (i)), -1, nullptr);

    for (int i = 0; i < arcs.getNumChildren(); ++i)
    {
        const ValueTree arc (arcs.getChild (i));
        if (true == (bool) arc[tags::missing])
        {
            ValueTree missingArc = arc.createCopy();
            if (processor.addConnection (
                    (uint32) (int) missingArc[tags::sourceNode],
                    (uint32) (int) missingArc[tags::sourcePort],
                    (uint32) (int) missingArc[tags::destNode],
                    (uint32) (int) missingArc[tags::destPort]))
            {
                missingArc.removeProperty (tags::missing, 0);
            }

            newArcs.addChild (missingArc, -1, 0);
        }
    }

    const auto index = graph.indexOf (arcs);
    graph.removeChild (arcs, nullptr);
    graph.addChild (newArcs, index, nullptr);
    arcs = graph.getChildWithName (tags::arcs);
    changed();
}

void GraphManager::setupNode (const ValueTree& data, ProcessorPtr obj)
{
    jassert (obj && data.hasType (types::Node));
    Node node (data, false);
    node.setProperty (tags::type, obj->getTypeString())
        .setProperty (tags::object, obj.get());

    PortArray ins, outs;
    node.getPorts (ins, outs, PortType::Audio);
    bool resetPorts = false;
    juce::ignoreUnused (resetPorts);

    if (obj->isSubGraph())
    {
        bindings.add (new Binding (*this, obj, node));
        resetPorts = true;
    }

    node.setProperty (tags::updater, new NodeModelUpdater (*this, data, obj.get()));

    if (auto* const proc = obj->getAudioProcessor())
    {
        bool busesConfigured = false;
        {
            // try to load buses layout.
            const auto buses = node.data().getChildWithName (tags::buses);
            if (buses.isValid() && buses.getNumChildren() >= 2)
            {
                AudioProcessor::BusesLayout layout;
                busesConfigured = false;
                for (const auto& data : buses.getChildWithName (tags::inputs))
                {
                    const auto str = data.getProperty (tags::arrangement).toString();
                    const auto acs = AudioChannelSet::fromAbbreviatedString (str);
                    layout.inputBuses.add (acs);
                }

                for (const auto& data : buses.getChildWithName (tags::outputs))
                {
                    const auto str = data.getProperty (tags::arrangement).toString();
                    const auto acs = AudioChannelSet::fromAbbreviatedString (str);
                    layout.outputBuses.add (acs);
                }

                if (proc->checkBusesLayoutSupported (layout))
                {
                    proc->suspendProcessing (true);
                    proc->releaseResources();
                    busesConfigured = proc->setBusesLayoutWithoutEnabling (layout);
                    proc->prepareToPlay (processor.getSampleRate(), processor.getBlockSize());
                    proc->suspendProcessing (false);
                }
            }
        }

        // try to match ports if needed
        if (! busesConfigured && (proc->getTotalNumInputChannels() != ins.size() || proc->getTotalNumOutputChannels() != outs.size()))
        {
            AudioProcessor::BusesLayout layout;

            layout.inputBuses.add (AudioChannelSet::namedChannelSet (ins.size()));
            layout.outputBuses.add (AudioChannelSet::namedChannelSet (outs.size()));

            if (proc->checkBusesLayoutSupported (layout))
            {
                proc->suspendProcessing (true);
                proc->releaseResources();
                proc->setBusesLayoutWithoutEnabling (layout);
                proc->prepareToPlay (processor.getSampleRate(), processor.getBlockSize());
                proc->suspendProcessing (false);
            }

            resetPorts = true;
        }
    }

    node.restorePluginState();
    node.resetPorts();
    if (node.isA ("Element", EL_NODE_ID_MIDI_INPUT_DEVICE) || node.isA ("Element", EL_NODE_ID_MIDI_OUTPUT_DEVICE))
    {
        jassert (node.getNumPorts() == 1);
    }

    jassert (node.getNumPorts() == static_cast<int> (obj->getNumPorts()));
}

// MARK: Root Graph Controller
RootGraphManager::RootGraphManager (RootGraph& graph, PluginManager& plugins)
    : GraphManager (graph, plugins),
      root (graph)
{
}

RootGraphManager::~RootGraphManager() {}

void RootGraphManager::unloadGraph()
{
    getRootGraph().clear();
}

} // namespace element
