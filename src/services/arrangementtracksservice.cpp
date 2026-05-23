// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/arrangementtracksservice.hpp"

#include <element/engine.hpp>
#include <element/node.h>
#include <element/node.hpp>
#include <element/session.hpp>

namespace element {

Node
ArrangementTracksService::findSubgraph (const Session& session)
{
    const Node root = session.getActiveGraph();
    if (! root.isValid())
        return Node();

    const int n = root.getNumNodes();
    for (int i = 0; i < n; ++i)
    {
        Node child = root.getNode (i);
        if (child.isA (EL_NODE_FORMAT_NAME, EL_NODE_ID_ARRANGEMENT_TRACKS))
            return child;
    }
    return Node();
}

Node
ArrangementTracksService::findOrCreateSubgraph (EngineService& engine,
                                                const Session& session)
{
    Node existing = findSubgraph (session);
    if (existing.isValid())
        return existing;

    /* EngineService::addNode adds to the active root graph by default
     * (see services/engineservice.cpp:570 -- delegates to addPlugin
     * which lands on the active controller). */
    Node created = engine.addNode (EL_NODE_ID_ARRANGEMENT_TRACKS,
                                   EL_NODE_FORMAT_NAME);

    /* TODO Phase 5: auto-wire the subgraph's stereo output to the
     * root graph's audio out.  v1 leaves it disconnected -- the user
     * can wire manually, and the test path uses an explicit
     * connect-to-master step.  Doing it here requires picking the
     * right output port pair on the root graph's IO node, which
     * varies by session config; safer to leave as Phase 5 polish. */

    return created;
}

Node
ArrangementTracksService::addAudioClipNode (EngineService& engine,
                                            const Node&    subgraph,
                                            bool           stereo)
{
    if (! subgraph.isValid() || ! subgraph.isGraph())
        return Node();

    juce::PluginDescription desc;
    desc.fileOrIdentifier = stereo ? EL_NODE_ID_AUDIO_CLIP : EL_NODE_ID_AUDIO_CLIP_MONO;
    desc.pluginFormatName = EL_NODE_FORMAT_NAME;
    desc.name             = stereo ? "Audio Clip" : "Audio Clip (Mono)";

    /* EngineService::addPlugin(graph, desc) adds inside the specified
     * graph -- see services/engineservice.cpp / engine.hpp:80 */
    return engine.addPlugin (subgraph, desc);
}

} // namespace element
