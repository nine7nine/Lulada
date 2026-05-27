// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/audioengine.hpp>
#include "engine/graphnode.hpp"
#include <element/node.hpp>

namespace element {

class PluginManager;
class RootGraph;

class GraphManager : public juce::ChangeBroadcaster
{
public:
    static const uint32 invalidNodeId = EL_INVALID_PORT;
    static const int invalidChannel = -1;

    GraphManager (GraphNode&, PluginManager&);
    ~GraphManager();

    /** Returns the controlled graph */
    GraphNode& getGraph() noexcept { return processor; }

    /** Returns true if controlling the given graph model */
    bool isManaging (const Node& model) const { return graph == model.data(); }

    /** Returns the number of nodes on the controlled graph */
    int getNumNodes() const noexcept;

    /** Returns a node by index */
    const ProcessorPtr getNode (const int index) const noexcept;

    /** Returns a node by NodeId */
    const ProcessorPtr getNodeForId (const uint32 uid) const noexcept;

    /** Returns a node model by Node ID */
    const Node getNodeModelForId (const uint32 nodeId) const noexcept;

    /** Find a graph manager (recursive) */
    GraphManager* findGraphManagerForGraph (const Node& graph) const noexcept;

    /** Returns true if this manager contains a node by ID */
    bool contains (const uint32 nodeId) const;

    /** Adds a node for processing */
    uint32 addNode (const Node& node);

    /** Adss a node with a plugin description */
    uint32 addNode (const juce::PluginDescription* desc, double x = 0.0f, double y = 0.0f, uint32 nodeId = 0);

    /** Element: async variant of addNode(PluginDescription*, ...).
        Routes the heavy createPluginInstance call through JUCE's
        async loader on a worker thread; once the plugin instance
        is ready, the rest of the addNode body runs synchronously on
        the message thread.  Callback fires with EL_INVALID_NODE on
        failure (and an error message has already been logged /
        surfaced by the underlying loader). */
    void addNodeAsync (const juce::PluginDescription* desc,
                       double x, double y, juce::uint32 nodeId,
                       std::function<void (juce::uint32 nodeId)> callback);

    /** Remove a node by ID */
    void removeNode (const uint32 nodeId);

    /** Disconnect a node from other nodes */
    void disconnectNode (const uint32 nodeId, const bool inputs = true, const bool outputs = true, const bool audio = true, const bool midi = true);

    /** Returns the number of connections on the graph
        DOES NOT include connections tagged as "missing"
     */
    int getNumConnections() const noexcept;
    const GraphNode::Connection* getConnection (const int index) const noexcept;

    const GraphNode::Connection*
        getConnectionBetween (uint32 sourceNode, int sourcePort, uint32 destNode, int destPort) const noexcept;

    bool canConnect (uint32 sourceFilterUID, int sourceFilterChannel, uint32 destFilterUID, int destFilterChannel) const noexcept;

    bool addConnection (uint32 sourceFilterUID, int sourceFilterChannel, uint32 destFilterUID, int destFilterChannel);

    void removeConnection (const int index);

    void removeConnection (uint32 sourceNode, uint32 sourcePort, uint32 destNode, uint32 destPort);

    void removeIllegalConnections();

    void clear();

    void setNodeModel (const Node& node);
    inline Node getGraphModel() const { return Node (graph, false); }

    void savePluginStates();

    /** Rebuilds the arcs model according to the GraphNode.
     *
     *  During session load (`loaded == false`) the GraphNode is being
     *  populated from the ValueTree -- arcs in the ValueTree are the
     *  source of truth, the GraphNode has zero connections until the
     *  arc-load loop runs at the end of setNodeModel.  Calling
     *  processorArcsChanged() here would REBUILD the arcs ValueTree
     *  from the empty processor + wipe the saved arcs, dropping every
     *  saved connection silently.
     *
     *  This path is reached during load via NodeModelUpdater::onPortsChanged:
     *  child IONode::refreshPorts (fired during setupNode → Node::resetPorts)
     *  emits portsChanged → NMU sees it → calls syncArcsModel.
     *  Guarding on `loaded` keeps post-load behaviour intact (port count
     *  changes after load DO need to rebuild arcs to drop now-illegal
     *  connections) while protecting the load path. */
    inline void syncArcsModel()
    {
        if (! loaded)
            return;
        processor.removeIllegalConnections();
        processorArcsChanged();
    }

    inline bool isLoaded() const { return loaded; }

private:
    PluginManager& pluginManager;
    GraphNode& processor;
    ValueTree graph, arcs, nodes;
    bool loaded = false;

    uint32 lastUID;

    class Binding;
    friend class Binding;
    OwnedArray<Binding> bindings;

    uint32 getNextUID() noexcept;
    inline void changed() { sendChangeMessage(); }
    Processor* createFilter (const PluginDescription* desc, double x = 0.0f, double y = 0.0f, uint32 nodeId = 0);
    Processor* createPlaceholder (const Node& node);

    /* Element: post-load finalization shared between addNode (sync)
     * and addNodeAsync.  Strictly message-thread; mutates the graph
     * model, registers the node in the processor's value tree, and
     * negotiates a default stereo bus layout for AudioProcessor-
     * backed nodes. */
    juce::uint32 finalizeAddedNode (Processor* object,
                                    const juce::PluginDescription* desc,
                                    double rx, double ry);

    /* Element: async finalize for the addNodeAsync path.  Splits the
     * work across threads — Part A (metadata + resetPorts) on the
     * message thread for VST3 MMLock safety, the hidePorts loop on a
     * worker thread (pure ValueTree writes on a not-yet-attached
     * tree), then Part B (stereo bus + addChild + changed) back on
     * the message thread via callAsync.  Callback fires on the
     * message thread once Part B completes.  No-op if either input is
     * null. */
    void finalizeAddedNodeAsync (Processor* object,
                                 std::shared_ptr<juce::PluginDescription> desc,
                                 double rx, double ry,
                                 std::function<void (juce::uint32)> callback);

    void setupNode (const ValueTree& data, ProcessorPtr object);

    void processorArcsChanged();

    /* Element: WeakReference master for the async plugin-load path.
     * addNodeAsync captures a WeakReference into the lambda dispatched
     * back from JUCE's worker thread; if the GraphManager is destroyed
     * (session close, app quit) before the worker completes, the
     * reference is invalidated and the callback no-ops instead of
     * touching freed memory.  No audio-thread involvement — the
     * weak ref is consulted only on the message thread inside the
     * async completion. */
    JUCE_DECLARE_WEAK_REFERENCEABLE (GraphManager)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GraphManager)
};

class RootGraphManager : public GraphManager
{
public:
    RootGraphManager (RootGraph& graph, PluginManager& plugins);
    ~RootGraphManager();

    /** Return the underlying RootGraph processor */
    RootGraph& getRootGraph() const { return root; }

    /** Unload graph nodes without clearing the model */
    void unloadGraph();

private:
    RootGraph& root;
};

} // namespace element
