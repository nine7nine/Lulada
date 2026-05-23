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
#include <element/porttype.hpp>
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
    {
        /* Ensure existing subgraphs get the canonical name on first
         * touch -- pre-rename sessions just show empty or the
         * generic plugin id otherwise. */
        if (existing.getName() != "Multi-Track")
            existing.setName ("Multi-Track");
        return existing;
    }

    /* EngineService::addNode adds to the active root graph by default
     * (see services/engineservice.cpp:570 -- delegates to addPlugin
     * which lands on the active controller). */
    Node created = engine.addNode (EL_NODE_ID_ARRANGEMENT_TRACKS,
                                   EL_NODE_FORMAT_NAME);

    if (created.isValid())
        created.setName ("Multi-Track");

    /* Subgraph -> root sink (master Audio Out) wiring intentionally
     * stays manual.  Per design philosophy, the main graph belongs
     * to the user; we don't auto-wire anything visible to them.
     * Auto-wiring happens ONLY INSIDE the subgraph (AudioClipNode
     * outputs -> Multi-Track's internal audio output IO node);
     * see addAudioClipNode. */

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
    Node clip = engine.addPlugin (subgraph, desc);
    if (! clip.isValid())
        return clip;

    /* Auto-wire inside the subgraph:
     *
     *   subgraph audio.input  ->  AudioClipNode input   (record path)
     *   AudioClipNode output  ->  subgraph audio.output (playback path)
     *
     * Both connections happen INSIDE the subgraph only -- the
     * subgraph face <-> session master remains user-controlled on
     * the main graph (per design: don't touch the main graph).
     *
     * Net effect: the user wires ONCE on the main graph -- their
     * source goes into the Multi-Track input face, the Multi-Track
     * output face goes to their master.  Every clip lane added
     * after that point automatically picks up the input signal for
     * recording AND mixes into the subgraph's output for playback.
     *
     * Multi-clip mixing: subsequent clips all wire to the same
     * audio.input and audio.output IO nodes; the graph processor
     * routes the input signal to all clip inputs (each lane's arm
     * state gates capture independently) and sums the playback
     * outputs.  Simple sum (no internal AudioMixerNode yet); upgrade
     * for per-clip level control if needed. */
    const int channels = stereo ? 2 : 1;

    const Node audioInIO  = subgraph.getIONode (PortType::Audio, true  /*input*/);
    const Node audioOutIO = subgraph.getIONode (PortType::Audio, false /*output*/);

    if (audioInIO.isValid())
    {
        for (int ch = 0; ch < channels; ++ch)
            engine.connectChannels (subgraph, audioInIO, ch, clip, ch);
    }
    else
    {
        juce::Logger::writeToLog (
            "[ArrangementTracksService::addAudioClipNode] WARN: subgraph"
            " has no audio.input IO node; clip input not auto-wired");
    }

    if (audioOutIO.isValid())
    {
        for (int ch = 0; ch < channels; ++ch)
            engine.connectChannels (subgraph, clip, ch, audioOutIO, ch);
    }
    else
    {
        juce::Logger::writeToLog (
            "[ArrangementTracksService::addAudioClipNode] WARN: subgraph"
            " has no audio.output IO node; clip output not auto-wired");
    }

    return clip;
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
     * session sink; manual today).
     *
     * Two-step unwrap: clip.getObject() returns the
     * AudioProcessorNode wrapper (element::Processor); the actual
     * AudioClipNode lives inside as the wrapped juce::AudioProcessor.
     * Reach it via getAudioProcessor(). */
    AudioClipNode* clipProc = nullptr;
    if (auto* wrapper = clip.getObject())
        clipProc = dynamic_cast<AudioClipNode*> (wrapper->getAudioProcessor());

    if (clipProc != nullptr)
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
            " -> WARN: could not resolve clip.getObject()->getAudioProcessor()"
            " to AudioClipNode*; immediate playback skipped");
    }

    juce::Logger::writeToLog (" -> SUCCESS");
    return true;
}

} // namespace element
