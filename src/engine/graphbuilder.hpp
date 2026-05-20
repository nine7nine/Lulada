// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ElementApp.h"

namespace element {

class GraphNode;
class Processor;

class GraphOp
{
public:
    GraphOp() {}
    virtual ~GraphOp() {}

    virtual void perform (AudioSampleBuffer& sharedBufferChans,
                          const OwnedArray<MidiBuffer>& sharedMidiBuffers,
                          const int numSamples) = 0;

    /** Dependency declaration for layered scheduling.  Default empty;
        ops that touch shared buffers override.  In-place ops declare the
        same slot as both read and write. */
    virtual void getReadAudioBuffers  (Array<int>&) const {}
    virtual void getWriteAudioBuffers (Array<int>&) const {}
    virtual void getReadMidiBuffers   (Array<int>&) const {}
    virtual void getWriteMidiBuffers  (Array<int>&) const {}

private:
    JUCE_LEAK_DETECTOR (GraphOp)
};

/** Used to calculate the correct sequence of rendering ops needed, based on
    the best re-use of shared buffers at each stage. */
class GraphBuilder
{
public:
    GraphBuilder (GraphNode& graph_,
                  const Array<void*>& orderedNodes_,
                  Array<void*>& renderingOps);

    int buffersNeeded (PortType type);
    int getTotalLatencySamples() const { return totalLatency; }

    /** Post-process a linear renderingOps list into layered groups via
        list-scheduling.  Two ops land in the same layer iff their buffer
        deps don't conflict (write/read, read/write, write/write —
        independently on audio and midi buffer namespaces).  Layer order
        is honoured at execution time; ops within a layer are independent
        and may run in parallel. */
    static void computeRenderingLayers (const Array<void*>& renderingOps,
                                        Array<Array<int>>& renderingLayers);

    /** Count "expensive" ops (ProcessBufferOp — i.e. real plugin processBlock
        calls) per layer.  Cheap copy/clear/delay ops are not worth
        parallelising on their own; this drives the parallel-dispatch gate. */
    static void countExpensiveOpsPerLayer (const Array<void*>& renderingOps,
                                           const Array<Array<int>>& renderingLayers,
                                           Array<int>& counts);

private:
    //==============================================================================
    GraphNode& graph;
    const Array<void*>& orderedNodes;
    Array<uint32> allNodes[PortType::Unknown];
    Array<uint32> allPorts[PortType::Unknown];

    enum
    {
        freeNodeID = 0xffffffff,
        zeroNodeID = 0xfffffffe,
        anonymousNodeID = 0xfffffffd
    };

    static bool isNodeBusy (uint32 nodeID) noexcept { return nodeID != freeNodeID && nodeID != zeroNodeID; }

    Array<uint32> nodeDelayIDs;
    Array<int> nodeDelays;
    int totalLatency;

    int getNodeDelay (const uint32 nodeID) const;
    void setNodeDelay (const uint32 nodeID, const int latency);

    int getInputLatency (const uint32 nodeID) const;

    void createRenderingOpsForNode (Processor* const node, Array<void*>& renderingOps, const int ourRenderingIndex);

    int getFreeBuffer (PortType type);
    int getReadOnlyEmptyBuffer() const noexcept;
    int getBufferContaining (const PortType type, const uint32 nodeId, const uint32 outputPort) noexcept;
    void markUnusedBuffersFree (const int stepIndex);
    bool isBufferNeededLater (int stepIndexToSearchFrom, uint32 inputChannelOfIndexToIgnore, const uint32 sourceNode, const uint32 outputPortIndex) const;
    void markBufferAsContaining (int bufferNum, PortType type, uint32 nodeId, uint32 portIndex);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GraphBuilder)
};

} // namespace element
