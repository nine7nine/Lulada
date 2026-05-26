// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/rootgraph.hpp"
#include "engine/mappingengine.hpp"
#include "services/automation/automation_engine.hpp"
#include <element/context.hpp>

namespace element {

RootGraph::RootGraph (Context& c)
    : GraphNode (c)
{
    /* Wire the per-graph AutomationEngine into the Context-owned
     * MappingEngine so live MIDI CC handlers can defer to active
     * automation.  Pointer stored atomically on the MappingEngine;
     * MIDI thread reads via getAutomationEngine() on each CC event.
     * Cleared in dtor before the engine itself is destroyed. */
    c.mapping().setAutomationEngine (automationEngine());
}

RootGraph::~RootGraph()
{
    /* Tear-down ordering:
     *   1. Capture the engine pointer BEFORE clearing it on the
     *      MappingEngine (we still need it for the drain in step 3).
     *   2. Atomic-store nullptr on the MappingEngine -- new MIDI
     *      handler calls see nullptr + skip the engine lookup
     *      entirely.
     *   3. Drain in-flight MIDI handlers via the engine's
     *      lookupLock_ -- any handler that loaded our engine pointer
     *      BEFORE step 2 is still holding the lock (or about to);
     *      acquiring + releasing it ourselves guarantees we wait
     *      for them to complete.
     *   4. GraphNode's dtor (called next) destroys the engine -- safe
     *      now that no MIDI handler holds it. */
    auto* eng = automationEngine();
    _context.mapping().setAutomationEngine (nullptr);
    if (eng != nullptr)
        eng->drainPendingLookups();
}

void RootGraph::refreshPorts()
{
    GraphNode::refreshPorts();
}

void RootGraph::setPlayConfigFor (DeviceManager& devices)
{
    if (auto* const device = devices.getCurrentAudioDevice())
        setPlayConfigFor (device);
}

void RootGraph::setPlayConfigFor (AudioIODevice* device)
{
    jassert (device != nullptr);
    setRenderDetails (device->getCurrentBufferSizeSamples(),
                      device->getCurrentSampleRate());
}

void RootGraph::setPlayConfigFor (const DeviceManager::AudioDeviceSetup& setup)
{
    setRenderDetails (setup.sampleRate, setup.bufferSize);
}

} // namespace element
