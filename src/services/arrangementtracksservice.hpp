// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/node.hpp>

namespace element {

class EngineService;
class Session;

/** Lookup + lazy-create + accessor helpers for the ArrangementTracks
 *  subgraph -- the GraphNode (subgraph) that holds the timeline's
 *  AudioClipNode instances.  Lives in the session's active root graph
 *  as a single top-level node with fileOrIdentifier =
 *  EL_NODE_ID_ARRANGEMENT_TRACKS.
 *
 *  Why a subgraph: keeps the timeline's audio infrastructure
 *  encapsulated from the user-managed plugin/effect graph.  Adding /
 *  removing audio lanes only mutates inside this subgraph; the
 *  surrounding graph stays clean.  See timeline-audio-design.md §3.3.
 *
 *  Threading: all methods called from the message thread.
 *  EngineService::addNode triggers a GraphManager mutation that
 *  schedules the audio-thread integration asynchronously; callers
 *  who need the new node ready for state queries should respect
 *  Element's standard message-loop pump-once pattern between add
 *  and inspect (audit existing addPluginAsync sites).
 */
struct ArrangementTracksService
{
    /** Find the ArrangementTracks subgraph in the session's active
     *  root graph.  Returns an invalid Node if none exists yet. */
    static Node findSubgraph (const Session& session);

    /** Find the existing subgraph or instantiate a new one if absent.
     *  When creating, the new subgraph is added to the active root
     *  graph; auto-wiring its outputs to the root sink is a Phase 5
     *  polish item -- v1 leaves it disconnected so user can wire it
     *  manually until that lands.  Returns invalid Node on failure. */
    static Node findOrCreateSubgraph (EngineService& engine,
                                      const Session& session);

    /** Add an AudioClipNode (stereo or mono) inside the given subgraph.
     *  `subgraph` must be the Node returned by findOrCreateSubgraph
     *  (or a subgraph with isGraph()==true).  Returns invalid Node on
     *  failure (subgraph isn't a graph, node creation failed, etc.). */
    static Node addAudioClipNode (EngineService& engine,
                                  const Node&    subgraph,
                                  bool           stereo = true);
};

} // namespace element
