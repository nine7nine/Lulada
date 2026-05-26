// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <element/audioengine.hpp>
#include <element/midipipe.hpp>
#include <element/node.hpp>
#include <element/portcount.hpp>
#include <element/context.hpp>

#include "engine/graphbuilder.hpp"
#include "engine/ionode.hpp"
#include "nodes/audioprocessor.hpp"
#include "engine/graphnode.hpp"
#include "services/automation/automation_engine.hpp"

#include <pthread.h>
#include <sched.h>
#include <thread>

#ifndef EL_GRAPH_NODE_NAME
#define EL_GRAPH_NODE_NAME "Graph"
#endif

namespace element {

/* GraphWorker: SCHED_FIFO 70 self-promote, WaitableEvent sync, no
 * allocations on render path.  Modelled on SamplerWorker in
 * src/nodes/sampler.cpp.  Receives raw pointers into renderingOps and
 * layerIndices arrays — never holds a reference to GraphNode itself, so
 * the seqLock invariant (held by main throughout the op walk) is
 * preserved trivially.  Workers run one rung below the NSPA audio
 * thread (which sits at FIFO 80) so they cannot starve it. */
class GraphWorker : public juce::Thread
{
public:
    explicit GraphWorker (const String& threadName) : juce::Thread (threadName) {}

    void postJob (void* const* opsArray,
                  const int* indices,
                  int start, int end,
                  AudioSampleBuffer* audio,
                  const OwnedArray<MidiBuffer>* midi,
                  int n) noexcept
    {
        ops        = opsArray;
        layerIdx   = indices;
        jobStart   = start;
        jobEnd     = end;
        audioBuf   = audio;
        midiBufs   = midi;
        numSamples = n;
        workReady.signal();
    }

    void waitForJobCompletion() noexcept { jobCompleted.wait (-1); }

    void requestStop() noexcept
    {
        signalThreadShouldExit();
        workReady.signal();
    }

    void run() override
    {
        /* Best-effort RT promotion — if the user lacks RTPRIO capability
         * the call fails and the worker runs at SCHED_OTHER.  Functionally
         * still correct, just less RT-tight. */
        sched_param p{};
        p.sched_priority = 70;
        pthread_setschedparam (pthread_self(), SCHED_FIFO, &p);

        while (! threadShouldExit())
        {
            workReady.wait (-1);
            if (threadShouldExit()) break;

            for (int k = jobStart; k < jobEnd; ++k)
            {
                const int opIdx = layerIdx[k];
                auto* op = static_cast<GraphOp*> (ops[opIdx]);
                op->perform (*audioBuf, *midiBufs, numSamples);
            }

            jobCompleted.signal();
        }
    }

private:
    juce::WaitableEvent workReady, jobCompleted;
    void* const* ops      = nullptr;
    const int*   layerIdx = nullptr;
    int jobStart = 0, jobEnd = 0;
    AudioSampleBuffer* audioBuf = nullptr;
    const OwnedArray<MidiBuffer>* midiBufs = nullptr;
    int numSamples = 0;
};

struct GraphNode::WorkerPool
{
    std::vector<std::unique_ptr<GraphWorker>> workers;

    void prepare (int desired)
    {
        if ((int) workers.size() == desired) return;
        stop();
        for (int i = 0; i < desired; ++i)
        {
            auto w = std::make_unique<GraphWorker> ("GraphWorker" + String (i));
            w->startThread();
            workers.push_back (std::move (w));
        }
    }

    void stop()
    {
        for (auto& w : workers) w->requestStop();
        for (auto& w : workers) w->stopThread (2000);
        workers.clear();
    }

    ~WorkerPool() { stop(); }
};

GraphNode::Connection::Connection (const uint32 sourceNode_, const uint32 sourcePort_, const uint32 destNode_, const uint32 destPort_) noexcept
    : Arc (sourceNode_, sourcePort_, destNode_, destPort_) {}

GraphNode::GraphNode (Context& c)
    : Processor (PortCount()
                     .with (PortType::Audio, 2, 2)
                     .with (PortType::Midi, 1, 1)
                     .toPortList()),
      _context (c),
      lastNodeId (0),
      renderingBuffers (1, 1),
      currentAudioInputBuffer (nullptr),
      currentAudioOutputBuffer (1, 1),
      currentMidiInputBuffer (nullptr)
{
    for (int i = 0; i < IONode::numDeviceTypes; ++i)
        ioNodes[i] = EL_INVALID_PORT;
    setName (EL_GRAPH_NODE_NAME);

    /* Construct the automation engine eagerly so the audio-thread
     * applyForBlock call in render() never null-checks.  Idle state
     * (zero tracks) costs ~3 atomic ops per block. */
    automationEngine_ = std::make_unique<automation::AutomationEngine>();
}

GraphNode::~GraphNode()
{
    renderingSequenceChanged.disconnect_all_slots();
    clearRenderingSequence();
    clear();
}

void GraphNode::clear()
{
    clearRenderingSequence();
    nodes.clear();
    connections.clear();
}

Processor* GraphNode::getNodeForId (const uint32 nodeId) const
{
    for (int i = nodes.size(); --i >= 0;)
        if (nodes.getUnchecked (i)->nodeId == nodeId)
            return nodes.getUnchecked (i);

    return nullptr;
}

Processor* GraphNode::addNode (Processor* newNode, uint32 nodeId)
{
    if (newNode == nullptr || (void*) newNode->getAudioProcessor() == (void*) this)
    {
        jassertfalse;
        return nullptr;
    }

    for (int i = nodes.size(); --i >= 0;)
    {
        if (nodes.getUnchecked (i).get() == newNode)
        {
            jassertfalse; // Cannot add the same object to the graph twice!
            return nullptr;
        }
    }

    if (nodeId == 0 || nodeId == EL_INVALID_NODE)
    {
        const_cast<uint32&> (newNode->nodeId) = ++lastNodeId;
        jassert (newNode->nodeId == lastNodeId);
    }
    else
    {
        if (nullptr != getNodeForId (nodeId))
        {
            // you can't add a node with an id that already exists in the graph..
            jassertfalse;
            removeNode (nodeId);
        }

        const_cast<uint32&> (newNode->nodeId) = nodeId;
        jassert (newNode->nodeId == nodeId);
        if (nodeId > lastNodeId)
            lastNodeId = nodeId;
    }

    newNode->setPlayHead (playhead);
    newNode->setParentGraph (this);
    newNode->refreshPorts();
    if (prepared())
        newNode->prepare (getSampleRate(), getBlockSize(), this);
    triggerAsyncUpdate();
    return nodes.add (newNode);
}

bool GraphNode::removeNode (const uint32 nodeId)
{
    disconnectNode (nodeId);
    for (int i = nodes.size(); --i >= 0;)
    {
        ProcessorPtr n = nodes.getUnchecked (i);
        if (n->nodeId == nodeId)
        {
            nodes.remove (i);

            handleAsyncUpdate();
            n->setParentGraph (nullptr);
            n->setPlayHead (nullptr);

            if (n->isSubGraph())
            {
                DBG ("[element] sub graph removed");
            }

            return true;
        }
    }

    return false;
}

const GraphNode::Connection*
    GraphNode::getConnectionBetween (const uint32 sourceNode,
                                     const uint32 sourcePort,
                                     const uint32 destNode,
                                     const uint32 destPort) const
{
    const Connection c (sourceNode, sourcePort, destNode, destPort);
    ArcSorter sorter;
    return connections[connections.indexOfSorted (sorter, &c)];
}

bool GraphNode::isConnected (const uint32 sourceNode,
                             const uint32 destNode) const
{
    for (int i = connections.size(); --i >= 0;)
    {
        const Connection* const c = connections.getUnchecked (i);

        if (c->sourceNode == sourceNode
            && c->destNode == destNode)
        {
            return true;
        }
    }

    return false;
}

bool GraphNode::canConnect (const uint32 sourceNode, const uint32 sourcePort, const uint32 destNode, const uint32 destPort) const
{
    if (sourceNode == destNode)
    {
        DBG ("[element] cannot connect to self: " << (int) sourceNode);
        return false;
    }

    const Processor* const source = getNodeForId (sourceNode);
    if (source == nullptr)
    {
        DBG ("[element] source not found");
        return false;
    }

    if (sourcePort >= source->getNumPorts())
    {
        DBG ("[element] source port greater than total: port: "
             << source->getName() << ": "
             << (int) sourcePort << " total: "
             << (int) source->getNumPorts());
        return false;
    }

    if (! source->isPortOutput (sourcePort))
    {
        DBG ("[element] source port is not an output port: " << (int) sourcePort);
        return false;
    }

    const Processor* const dest = getNodeForId (destNode);

    if (dest == nullptr
        || (destPort >= dest->getNumPorts())
        || (! dest->isPortInput (destPort)))
    {
        return false;
    }

    const PortType sourceType (source->getPortType (sourcePort));
    const PortType destType (dest->getPortType (destPort));

    if (! sourceType.canConnect (destType))
        return false;

    // Graph Builder on understands one-to-one for these configs.
    // - Any to Control
    // - Control to CV
    // clang-format off
    if (destType == PortType::Control || 
        (sourceType == PortType::Control && destType == PortType::CV))
    {
        int numSources = 0;
        for (const auto* c : connections)
            if (c->destNode == destNode && c->destPort == destPort)
                if (++numSources > 0)
                    return false;
    }
    // clang-format on

    return getConnectionBetween (sourceNode, sourcePort, destNode, destPort) == nullptr;
}

bool GraphNode::addConnection (const uint32 sourceNode, const uint32 sourcePort, const uint32 destNode, const uint32 destPort)
{
    if (! canConnect (sourceNode, sourcePort, destNode, destPort))
        return false;

    ArcSorter sorter;
    Connection* c = new Connection (sourceNode, sourcePort, destNode, destPort);
    connections.addSorted (sorter, c);
    triggerAsyncUpdate();
    return true;
}

bool GraphNode::connectChannels (PortType type, uint32 sourceNode, int32 sourceChannel, uint32 destNode, int32 destChannel)
{
    Processor* src = getNodeForId (sourceNode);
    Processor* dst = getNodeForId (destNode);
    if (! src && ! dst)
        return false;
    return addConnection (src->nodeId, src->getPortForChannel (type, sourceChannel, false), dst->nodeId, dst->getPortForChannel (type, destChannel, true));
}

void GraphNode::removeConnection (const int index)
{
    connections.remove (index);
    cancelPendingUpdate();
    triggerAsyncUpdate();
}

bool GraphNode::removeConnection (const uint32 sourceNode, const uint32 sourcePort, const uint32 destNode, const uint32 destPort)
{
    bool doneAnything = false;

    for (int i = connections.size(); --i >= 0;)
    {
        const Connection* const c = connections.getUnchecked (i);

        if (c->sourceNode == sourceNode
            && c->destNode == destNode
            && c->sourcePort == sourcePort
            && c->destPort == destPort)
        {
            removeConnection (i);
            doneAnything = true;
        }
    }

    return doneAnything;
}

bool GraphNode::disconnectNode (const uint32 nodeId)
{
    bool doneAnything = false;

    for (int i = connections.size(); --i >= 0;)
    {
        const Connection* const c = connections.getUnchecked (i);

        if (c->sourceNode == nodeId || c->destNode == nodeId)
        {
            removeConnection (i);
            doneAnything = true;
        }
    }

    return doneAnything;
}

bool GraphNode::isConnectionLegal (const Connection* const c) const
{
    jassert (c != nullptr);

    const Processor* const source = getNodeForId (c->sourceNode);
    const Processor* const dest = getNodeForId (c->destNode);

    return source != nullptr && dest != nullptr
           && source->isPortOutput (c->sourcePort) && dest->isPortInput (c->destPort)
           && source->getPortType (c->sourcePort).canConnect (dest->getPortType (c->destPort))
           && c->sourcePort < source->getNumPorts()
           && c->destPort < dest->getNumPorts();
}

bool GraphNode::removeIllegalConnections()
{
    bool doneAnything = false;

    for (int i = connections.size(); --i >= 0;)
    {
        if (! isConnectionLegal (connections.getUnchecked (i)))
        {
            removeConnection (i);
            doneAnything = true;
        }
    }

    return doneAnything;
}

void GraphNode::setMidiChannel (const int channel) noexcept
{
    jassert (isPositiveAndBelow (channel, 17));
    if (channel <= 0)
        midiChannels.setOmni (true);
    else
        midiChannels.setChannel (channel);
}

void GraphNode::setMidiChannels (const BigInteger channels) noexcept
{
    ScopedLock sl (getPropertyLock());
    midiChannels.setChannels (channels);
}

void GraphNode::setMidiChannels (const MidiChannels channels) noexcept
{
    ScopedLock sl (getPropertyLock());
    midiChannels = channels;
}

bool GraphNode::acceptsMidiChannel (const int channel) const noexcept
{
    ScopedLock sl (getPropertyLock());
    return midiChannels.isOn (channel);
}

void GraphNode::setVelocityCurveMode (const VelocityCurve::Mode mode) noexcept
{
    ScopedLock sl (getPropertyLock());
    velocityCurve.setMode (mode);
}

static void deleteRenderOpArray (Array<void*>& ops)
{
    for (int i = ops.size(); --i >= 0;)
        delete static_cast<GraphOp*> (ops.getUnchecked (i));
    ops.clearQuick();
}

void GraphNode::clearRenderingSequence()
{
    Array<void*> oldOps;

    {
        const ScopedLock sl (seqLock);
        renderingOps.swapWith (oldOps);
        renderingLayers.clearQuick();
        expensiveOpCountPerLayer.clearQuick();
    }

    deleteRenderOpArray (oldOps);
}

bool GraphNode::isAnInputTo (const uint32 possibleInputId,
                             const uint32 possibleDestinationId,
                             const int recursionCheck) const
{
    if (recursionCheck > 0)
    {
        for (int i = connections.size(); --i >= 0;)
        {
            const GraphNode::Connection* const c = connections.getUnchecked (i);

            if (c->destNode == possibleDestinationId
                && (c->sourceNode == possibleInputId
                    || isAnInputTo (possibleInputId, c->sourceNode, recursionCheck - 1)))
                return true;
        }
    }

    return false;
}

void GraphNode::buildRenderingSequence()
{
    Array<void*> newRenderingOps;
    Array<Array<int>> newRenderingLayers;
    Array<int> newExpensiveOpCountPerLayer;
    int numRenderingBuffersNeeded = 2;
    int numMidiBuffersNeeded = 1;
    int numAtomBuffersNeeded = 1;

    {
        Array<void*> orderedNodes;

        {
            const LookupTable table (connections);

            for (int i = 0; i < nodes.size(); ++i)
            {
                Processor* const node = nodes.getUnchecked (i);

                int j = 0;
                for (; j < orderedNodes.size(); ++j)
                    if (table.isAnInputTo (node->nodeId, ((Processor*) orderedNodes.getUnchecked (j))->nodeId))
                        break;

                orderedNodes.insert (j, node);
            }
        }

        GraphBuilder builder (*this, orderedNodes, newRenderingOps);
        numRenderingBuffersNeeded = builder.buffersNeeded (PortType::Audio);
        numMidiBuffersNeeded = builder.buffersNeeded (PortType::Midi);
        numAtomBuffersNeeded = builder.buffersNeeded (PortType::Atom);
        setLatencySamples (builder.getTotalLatencySamples());

        GraphBuilder::computeRenderingLayers (newRenderingOps, newRenderingLayers);
        GraphBuilder::countExpensiveOpsPerLayer (newRenderingOps, newRenderingLayers, newExpensiveOpCountPerLayer);

       #if JUCE_DEBUG
        int maxLayerSize = 0;
        int parallelCandidateLayers = 0;
        for (int i = 0; i < newRenderingLayers.size(); ++i)
        {
            maxLayerSize = jmax (maxLayerSize, newRenderingLayers.getReference (i).size());
            if (newExpensiveOpCountPerLayer.getUnchecked (i) >= 2)
                ++parallelCandidateLayers;
        }
        DBG ("[GraphMT] " << newRenderingOps.size() << " ops in "
             << newRenderingLayers.size() << " layers (max layer size " << maxLayerSize
             << ", parallel-candidate layers " << parallelCandidateLayers << ")");
       #endif
    }

    {
        // swap over to the new rendering sequence..
        {
            const ScopedLock sl (getPropertyLock());
            renderingBuffers.setSize (numRenderingBuffersNeeded, 4096);
            renderingBuffers.clear();

            for (int i = midiBuffers.size(); --i >= 0;)
                midiBuffers.getUnchecked (i)->clear();

            while (midiBuffers.size() < numMidiBuffersNeeded)
                midiBuffers.add (new MidiBuffer());
        }

        ScopedLock sl (seqLock);
        renderingOps.swapWith (newRenderingOps);
        renderingLayers.swapWith (newRenderingLayers);
        expensiveOpCountPerLayer.swapWith (newExpensiveOpCountPerLayer);
    }

    // delete the old ones..
    deleteRenderOpArray (newRenderingOps);

    renderingSequenceChanged();
}

void GraphNode::getOrderedNodes (ReferenceCountedArray<Processor>& orderedNodes)
{
    const LookupTable table (connections);
    for (int i = 0; i < nodes.size(); ++i)
    {
        Processor* const node = nodes.getUnchecked (i);

        int j = 0;
        for (; j < orderedNodes.size(); ++j)
            if (table.isAnInputTo (node->nodeId, ((Processor*) orderedNodes.getUnchecked (j))->nodeId))
                break;

        orderedNodes.insert (j, node);
    }
}

void GraphNode::handleAsyncUpdate()
{
    buildRenderingSequence();
}

void GraphNode::prepareToRender (double sampleRate, int estimatedSamplesPerBlock)
{
    const bool paramsChanged = (getSampleRate() != sampleRate || getBlockSize() != estimatedSamplesPerBlock);

    if (prepared() && ! paramsChanged)
        return;

    if (prepared() && paramsChanged)
    {
        releaseResources();
    }

    currentAudioInputBuffer = nullptr;
    currentAudioOutputBuffer.setSize (jmax (1, getNumAudioOutputs()), estimatedSamplesPerBlock);
    currentMidiInputBuffer = nullptr;
    currentMidiOutputBuffer.clear();
    clearRenderingSequence();

    _prepared = true;
    if (paramsChanged)
        setRenderDetails (sampleRate, estimatedSamplesPerBlock);

    for (int i = 0; i < nodes.size(); ++i)
        nodes.getUnchecked (i)->prepare (sampleRate, estimatedSamplesPerBlock, this);

    /* Worker-pool prepare.  Pool size = min(4, hwc - 2): reserve a core
     * for the audio main thread + one for the UI message thread.  hwc==0
     * (unknowable) → no pool, parallel stays off. */
    {
        const int hwc = (int) std::thread::hardware_concurrency();
        const int desired = jmax (0, jmin (4, hwc - 2));
        if (desired > 0)
        {
            if (! workerPool) workerPool = std::make_unique<WorkerPool>();
            workerPool->prepare (desired);
        }
        else if (workerPool)
        {
            workerPool->stop();
        }

        parallelEnabled.store (desired > 0);
    }

    buildRenderingSequence();
}

void GraphNode::releaseResources()
{
    if (! prepared())
        return;

    if (workerPool) workerPool->stop();
    parallelEnabled.store (false);

    for (int i = 0; i < nodes.size(); ++i)
        nodes.getUnchecked (i)->unprepare();

    _prepared = false;

    renderingBuffers.setSize (1, 1);
    midiBuffers.clear();

    currentAudioInputBuffer = nullptr;
    currentAudioOutputBuffer.setSize (1, 1);
    currentMidiInputBuffer = nullptr;
    currentMidiOutputBuffer.clear();
}

void GraphNode::reset()
{
    const ScopedLock sl (getPropertyLock());
    for (auto node : nodes)
        if (auto* const proc = node->getAudioProcessor())
            proc->reset();
}

// MARK: Process Graph

void GraphNode::render (RenderContext& rc)
{
    const int32 numSamples = rc.audio.getNumSamples();
    auto& midiMessages = *rc.midi.getWriteBuffer (0);
    currentAudioInputBuffer = &rc.audio;
    currentAudioOutputBuffer.setSize (jmax (1, rc.audio.getNumChannels()), numSamples);
    currentAudioOutputBuffer.clear();

    /* AutomationEngine per-block pass.  Runs BEFORE renderingOps so
     * parameter writes land before plugins read them.  Empty-engine
     * cost is ~3 atomic ops; loaded-engine cost scales with active
     * track count + active region complexity (one snapshot load +
     * binary search per active track + per-block dispatch per active
     * target).  Beats + bpm derived from the same playhead path
     * audioclip.cpp uses -- absence of a playhead leaves beats == -1
     * which findActiveRegion correctly returns nullptr for, and
     * beatsPerBlock == 0 which disables sample-accurate MIDI emission
     * (engine falls back to coarse single-write for all kinds). */
    if (automationEngine_ != nullptr)
    {
        double currentBeats   = -1.0;
        double beatsPerBlock  = 0.0;
        if (auto* ph = getPlayHead())
        {
            if (auto pos = ph->getPosition())
            {
                if (auto ppq = pos->getPpqPosition())
                    currentBeats = *ppq;

                const auto bpmOpt   = pos->getBpm();
                const double bpm    = (bpmOpt.hasValue() ? *bpmOpt : 0.0);
                const double sr     = getSampleRate();
                if (sr > 0.0 && bpm > 0.0)
                    beatsPerBlock = ((double) numSamples / sr) * (bpm / 60.0);
            }
        }

        automationEngine_->applyForBlock (currentBeats,
                                          beatsPerBlock,
                                          numSamples,
                                          getSampleRate(),
                                          &midiMessages);
    }

    if (midiChannels.isOmni() && velocityCurve.getMode() == VelocityCurve::Linear)
    {
        currentMidiInputBuffer = &midiMessages;
    }
    else
    {
        filteredMidi.clear();
        int chan = 0;

        for (auto m : midiMessages)
        {
            auto msg = m.getMessage();
            chan = msg.getChannel();
            if (chan > 0 && midiChannels.isOff (chan))
                continue;

            if (msg.isNoteOn())
            {
                msg.setVelocity (velocityCurve.process (msg.getFloatVelocity()));
            }

            filteredMidi.addEvent (msg, m.samplePosition);
        }

        currentMidiInputBuffer = &filteredMidi;
    }

    currentMidiOutputBuffer.clear();

    {
        ScopedLock sl (seqLock);

        /* Gate the parallel path: pool prepared + worth-it block size
         * + multi-layer schedule.  Anything else falls through to the
         * linear op walk. */
        const bool useParallel = parallelEnabled.load()
                              && workerPool && ! workerPool->workers.empty()
                              && numSamples >= 64
                              && renderingLayers.size() > 1;

        if (! useParallel)
        {
            for (auto ptr : renderingOps)
            {
                GraphOp* const op = static_cast<GraphOp*> (ptr);
                op->perform (renderingBuffers, midiBuffers, numSamples);
            }
        }
        else
        {
            void* const* opsArray = renderingOps.getRawDataPointer();
            const int numWorkers = (int) workerPool->workers.size();

            for (int L = 0; L < renderingLayers.size(); ++L)
            {
                const auto& layer = renderingLayers.getReference (L);
                const int layerSize = layer.size();

                /* Per-layer gate: parallelise only when 2+ expensive
                 * (ProcessBufferOp) ops sit in the layer.  Trivial
                 * Clear/Copy/Add ops are too cheap to amortise dispatch
                 * cost; run them sequentially. */
                const int expensive = expensiveOpCountPerLayer.getUnchecked (L);
                if (layerSize <= 1 || expensive < 2)
                {
                    for (int k = 0; k < layerSize; ++k)
                    {
                        const int opIdx = layer.getUnchecked (k);
                        auto* op = static_cast<GraphOp*> (opsArray[opIdx]);
                        op->perform (renderingBuffers, midiBuffers, numSamples);
                    }
                    continue;
                }

                /* Contiguous chunking across (numWorkers + main) units.
                 * Workers grab their slices from the front, main drains
                 * the tail, everyone meets at jobCompleted. */
                const int totalUnits = numWorkers + 1;
                const int base       = layerSize / totalUnits;
                const int extra      = layerSize % totalUnits;
                const int* const idx = layer.begin();

                int start = 0;
                int workersUsed = 0;
                for (int w = 0; w < numWorkers; ++w)
                {
                    const int chunk = base + (w < extra ? 1 : 0);
                    if (chunk <= 0) break;
                    workerPool->workers[(size_t) w]->postJob (opsArray, idx,
                                                              start, start + chunk,
                                                              &renderingBuffers, &midiBuffers,
                                                              numSamples);
                    start += chunk;
                    ++workersUsed;
                }
                for (int k = start; k < layerSize; ++k)
                {
                    const int opIdx = idx[k];
                    auto* op = static_cast<GraphOp*> (opsArray[opIdx]);
                    op->perform (renderingBuffers, midiBuffers, numSamples);
                }
                for (int w = 0; w < workersUsed; ++w)
                    workerPool->workers[(size_t) w]->waitForJobCompletion();
            }
        }
    }

    for (int i = 0; i < rc.audio.getNumChannels(); ++i)
        rc.audio.copyFrom (i, 0, currentAudioOutputBuffer, i, 0, numSamples);

    midiMessages.clear();
    midiMessages.addEvents (currentMidiOutputBuffer, 0, numSamples, 0);
}

void GraphNode::getPluginDescription (PluginDescription& d) const
{
    d.name = getName();
    d.uniqueId = d.name.hashCode();
    d.category = "Graphs";
    d.pluginFormatName = EL_NODE_FORMAT_NAME;
    d.fileOrIdentifier = EL_NODE_ID_GRAPH;
    d.manufacturerName = EL_NODE_FORMAT_AUTHOR;
    d.version = "1.0.0";
    d.isInstrument = false;
    d.numInputChannels = getNumAudioInputs();
    d.numOutputChannels = getNumAudioOutputs();
}

void GraphNode::setPlayHead (AudioPlayHead* newPlayHead)
{
    Processor::setPlayHead (newPlayHead);
    playhead = getPlayHead();
    for (auto* const node : nodes)
        node->setPlayHead (playhead);
}

void GraphNode::refreshPorts()
{
    if (! customPortsSet)
    {
        auto count = PortCount().with (PortType::Audio, 2, 2).with (PortType::Midi, 1, 1);
        setPorts (count.toPortList());
    }
    else
    {
        setPorts (userPorts);
    }

    for (auto* node : nodes)
    {
        if (auto io = dynamic_cast<IONode*> (node))
            io->refreshPorts();
    }
}

void GraphNode::setNumPorts (PortType type, int count, bool inputs, bool async)
{
    if (type != PortType::Audio && type != PortType::Midi)
        return;

    count = std::max ((int) 0, count);
    PortCount newCount;
    for (int ti = 0; ti < PortType::Unknown; ++ti)
    {
        auto tp = PortType (ti);
        newCount.set (tp,
                      (inputs && type == tp) ? count : getNumPorts (tp, true),
                      (! inputs && type == tp) ? count : getNumPorts (tp, false));
    }

    userPorts = newCount.toPortList();
    customPortsSet = true;

    if (async)
        triggerPortReset();
    else
    {
        resetPorts();
        portsChanged();
    }
}

void GraphNode::rebuild() noexcept
{
    cancelPendingUpdate();
    handleAsyncUpdate();
}

} // namespace element
