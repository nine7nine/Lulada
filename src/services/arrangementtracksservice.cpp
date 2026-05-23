// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/arrangementtracksservice.hpp"

#include "nodes/audioclip.hpp"
#include "services/sources/sourceregistry.hpp"
#include "services/timeline/lane.hpp"
#include "services/timeline/playlist.hpp"
#include "services/timeline/region.hpp"

#include <element/audioengine.hpp>
#include <element/context.hpp>
#include <element/engine.hpp>
#include <element/node.h>
#include <element/node.hpp>
#include <element/services.hpp>
#include <element/session.hpp>
#include <element/tags.hpp>
#include <element/transport.hpp>

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

bool
ArrangementTracksService::importAudioFileAsNewLane (const juce::File& file,
                                                    Services&         services)
{
    juce::Logger::writeToLog (
        juce::String ("[ArrangementTracksService::importAudioFileAsNewLane] file=")
        + file.getFullPathName());

    if (! file.existsAsFile())
    {
        juce::Logger::writeToLog (" -> file does not exist on disk; abort");
        return false;
    }

    auto sess = services.context().session();
    if (sess == nullptr)
    {
        juce::Logger::writeToLog (" -> no active session; abort");
        return false;
    }
    auto* engine = services.find<EngineService>();
    if (engine == nullptr)
    {
        juce::Logger::writeToLog (" -> no EngineService; abort");
        return false;
    }

    /* 1. Open + register source. */
    auto src = SourceRegistry::get().importFromFile (file);
    if (src == nullptr)
    {
        juce::Logger::writeToLog (" -> SourceRegistry::importFromFile returned null");
        return false;
    }

    /* 2. Find/create the subgraph. */
    Node subgraph = findOrCreateSubgraph (*engine, *sess);
    juce::Logger::writeToLog (
        juce::String (" -> subgraph valid=")
        + (subgraph.isValid() ? "yes" : "no")
        + " uuid=" + (subgraph.isValid() ? subgraph.getUuid().toString() : juce::String ("(none)"))
        + " isGraph=" + (subgraph.isValid() && subgraph.isGraph() ? "yes" : "no"));
    if (! subgraph.isValid()) return false;

    /* 3. Add AudioClipNode inside subgraph. */
    Node clip = addAudioClipNode (*engine, subgraph, true /*stereo*/);
    juce::Logger::writeToLog (
        juce::String (" -> clip valid=")
        + (clip.isValid() ? "yes" : "no")
        + " uuid=" + (clip.isValid() ? clip.getUuid().toString() : juce::String ("(none)")));
    if (! clip.isValid()) return false;

    /* 4. Build Lane + Region in memory; serialize into session. */
    Lane lane;
    lane.id             = juce::Uuid();
    lane.targetNodeUuid = clip.getUuid();
    lane.name           = file.getFileNameWithoutExtension();
    if (lane.name.isEmpty())
        lane.name = "Audio";
    lane.colour         = juce::Colour::fromRGB (90, 170, 130);

    /* Compute length in beats from file duration + current session bpm.
     * No active transport monitor -> assume 120 bpm. */
    double bpm = 120.0;
    if (auto audio = services.context().audio())
    {
        if (auto monitor = audio->getTransportMonitor())
            bpm = (double) monitor->tempo.get();
    }
    const double lengthSeconds = src->sourceSampleRate() > 0
        ? (double) src->durationSamples() / (double) src->sourceSampleRate()
        : 0.0;
    const double lengthBeats = juce::jmax (0.25, lengthSeconds * (bpm / 60.0));

    Region r;
    r.id            = juce::Uuid();
    r.sourceId      = src->uuid();
    r.positionBeats = 0.0;
    r.lengthBeats   = lengthBeats;
    r.name          = file.getFileNameWithoutExtension();
    r.colour        = juce::Colour::fromRGB (90, 170, 130);

    const juce::Uuid regionIdCopy = r.id;
    const juce::Uuid sourceIdCopy = r.sourceId;

    lane.playlist.addRegion (std::move (r));

    /* 5. Append into the session's tags::arrangement/lanes ValueTree
     * directly.  Bypasses ArrangementView entirely -- the next time
     * the view loads (or first didBecomeActive), loadLanesFromSession
     * picks the new lane up + rescanLaneTargets resolves the
     * audioClipCache against the live graph. */
    auto sessTree  = sess->data();
    auto arrTree   = sessTree.getOrCreateChildWithName (tags::arrangement, nullptr);
    auto lanesTree = arrTree.getOrCreateChildWithName (juce::Identifier ("lanes"), nullptr);
    lanesTree.appendChild (lane.toValueTree(), nullptr);

    /* 6. Fire schedulePlay so the user hears the new region
     * immediately (subject to the subgraph being wired to the
     * session sink; manual today). */
    if (auto* clipProc = dynamic_cast<AudioClipNode*> (clip.getObject()))
    {
        clipProc->schedulePlay (regionIdCopy, sourceIdCopy,
                                -1.0 /*immediate*/,
                                0 /*sampleOffset*/);
        juce::Logger::writeToLog (
            juce::String (" -> schedulePlay queued for region ")
            + regionIdCopy.toString());
    }
    else
    {
        juce::Logger::writeToLog (
            " -> WARN: clip Node is not a live AudioClipNode at import time;"
            " audible feedback skipped (graph rebind hadn't happened yet)");
    }

    juce::Logger::writeToLog (" -> SUCCESS");
    return true;
}

} // namespace element
