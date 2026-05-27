// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/arrangementtracksservice.hpp"

#include "engine/graphnode.hpp"
#include "nodes/audioclip.hpp"
#include "services/sessionservice.hpp"
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

namespace {

/* Multi-Track patchbay layout + wiring helpers.
 *
 * The Multi-Track subgraph is just a regular sub-graph (not a
 * dedicated class -- 6f847bdf and earlier proved a type marker
 * adds nothing observable).  These helpers give it smart defaults
 * that ACTUALLY work:
 *
 *  - Children spawn at grid positions (column = ordinal, row by
 *    kind).  Position is set via the rx/ry path through
 *    GraphManager::addNode, BEFORE the BlockComponent reads it,
 *    so the placement is visible on first paint.  My earlier
 *    attempt (setRelativePosition after addPlugin) didn't work
 *    because BlockComponent::updatePosition reads relativeX/Y
 *    ONCE then writes absolute tags::x/y; subsequent relative
 *    writes are ignored (see block.cpp:1365-1371).
 *  - Outer audio IO grows to 2*N channels (N audio clips) so
 *    each clip gets its OWN dedicated outer port pair (per-track
 *    direct-outs).  Today's "all clips share ch 0/1" mix-bus
 *    behavior is gone -- 1:1 mapping per user direction
 *    2026-05-26.
 *  - MIDI stays as one shared midi.output IO (Q-S5 resolved).
 *  - On opening an existing Multi-Track subgraph, a migration
 *    pass snaps existing children to grid + rewires them per
 *    the 1:1 model.
 */

/* Grid layout (relative 0..1 coords inside the subgraph canvas):
 *
 *   [audio.in]                              [midi.in]
 *
 *   [Clip 0] [Clip 1] [Clip 2] ...
 *   [Midi 0] [Midi 1] [Midi 2] ...
 *
 *   [audio.out]                             [midi.out]
 */
constexpr float kIoLeftX        = 0.05f;
constexpr float kIoRightX       = 0.95f;
constexpr float kIoTopY         = 0.10f;
constexpr float kIoBottomY      = 0.90f;
constexpr float kAudioRowY      = 0.40f;
constexpr float kMidiRowY       = 0.65f;
constexpr float kFirstChildX    = 0.18f;
constexpr float kChildSpacingX  = 0.07f;
constexpr float kMaxChildX      = 0.85f;
constexpr int   kStereoChannels = 2;

float gridXForChild (int ordinal) noexcept
{
    return juce::jmin (kFirstChildX + (float) ordinal * kChildSpacingX,
                       kMaxChildX);
}

/* Snap a node's persisted position so the BlockComponent picks it
 * up on next creation.  We write relativeX/Y AND clear tags::x/y
 * (the absolute cache) so block.cpp:1365's `! node.hasPosition()`
 * branch fires + reads our relative position fresh.  For NEWLY-
 * spawned nodes this isn't needed (rx/ry threaded through the
 * GraphManager::addNode path is sufficient); for MIGRATION of
 * existing nodes the absolute cache HAS to go for the new
 * relative coords to take effect. */
void snapNodePosition (Node n, float rx, float ry)
{
    if (! n.isValid()) return;
    n.setRelativePosition (rx, ry);
    n.data().removeProperty (tags::x, nullptr);
    n.data().removeProperty (tags::y, nullptr);
}

void placeIONodesAtCorners (const Node& subgraph)
{
    snapNodePosition (subgraph.getIONode (PortType::Audio, true  /*input*/),
                       kIoLeftX,  kIoTopY);
    snapNodePosition (subgraph.getIONode (PortType::Audio, false /*output*/),
                       kIoLeftX,  kIoBottomY);
    snapNodePosition (subgraph.getIONode (PortType::Midi,  true),
                       kIoRightX, kIoTopY);
    snapNodePosition (subgraph.getIONode (PortType::Midi,  false),
                       kIoRightX, kIoBottomY);
}

/* Collect AudioClipNode + MidiPlayerNode children in their
 * current display order (the order they appear in the subgraph's
 * ValueTree, which is the order they were added).  Used by the
 * grid pass + the rewire pass so ordinals agree. */
std::vector<Node>
collectChildrenByPluginId (const Node& subgraph, const juce::String& pluginId)
{
    std::vector<Node> out;
    const int total = subgraph.getNumNodes();
    out.reserve ((size_t) total);
    for (int i = 0; i < total; ++i)
    {
        Node child = subgraph.getNode (i);
        if (child.getProperty (tags::identifier).toString() == pluginId)
            out.push_back (child);
    }
    return out;
}

std::vector<Node> collectAudioClips (const Node& subgraph)
{
    auto stereo = collectChildrenByPluginId (subgraph,
                                              juce::String (EL_NODE_ID_AUDIO_CLIP));
    auto mono   = collectChildrenByPluginId (subgraph,
                                              juce::String (EL_NODE_ID_AUDIO_CLIP_MONO));
    stereo.insert (stereo.end(), mono.begin(), mono.end());
    return stereo;
}

std::vector<Node> collectMidiPlayers (const Node& subgraph)
{
    return collectChildrenByPluginId (subgraph,
                                       juce::String (EL_NODE_ID_MIDI_PLAYER));
}

/* Set the subgraph's outer audio port count.  Triggers the
 * GraphManager's addMissingIONodes pass via portsChanged, which
 * resizes (not destroys) the audio.input + audio.output IO
 * pseudo-nodes to the new channel count.  Existing connections
 * to channels still within range are preserved. */
void setSubgraphAudioChannels (const Node& subgraph, int channels)
{
    if (auto* proc = subgraph.getObject())
    {
        if (auto* graph = dynamic_cast<GraphNode*> (proc))
        {
            const int current = graph->getNumPorts (PortType::Audio, false);
            if (current == channels) return;
            graph->setNumPorts (PortType::Audio, channels, true,  false /*sync*/);
            graph->setNumPorts (PortType::Audio, channels, false, false /*sync*/);
        }
    }
}

/* Rewire all AudioClipNode children for 1:1 per-track direct-out
 * mapping.  Tears down every clip's audio connections + rebuilds:
 *   clip[i].input  <- subgraph.audio.input  (channels 2i, 2i+1)
 *   clip[i].output ->  subgraph.audio.output (channels 2i, 2i+1)
 * The subgraph's outer face exposes a stereo pair per clip, so
 * downstream code on the main graph can route each track to its
 * own destination (mastering bus, FX chain, external send, ...)
 * instead of mashing everything into a single shared bus. */
void rewireAudioClips (EngineService& engine, const Node& subgraph)
{
    auto clips = collectAudioClips (subgraph);
    const int n = (int) clips.size();
    setSubgraphAudioChannels (subgraph, juce::jmax (kStereoChannels, n * kStereoChannels));

    const Node audioInIO  = subgraph.getIONode (PortType::Audio, true);
    const Node audioOutIO = subgraph.getIONode (PortType::Audio, false);
    if (! audioInIO.isValid() || ! audioOutIO.isValid()) return;

    /* Clear every clip's audio connections inside the subgraph,
     * then rebuild per-ordinal.  disconnectNode walks the parent
     * graph's connection table so this is safe for clips already
     * partially-wired (legacy shared-bus connections + any user
     * manual cables both get cleared). */
    for (Node& clip : clips)
        engine.disconnectNode (clip, true /*ins*/, true /*outs*/,
                                true /*audio*/, false /*midi*/);

    /* Disconnect the IO nodes themselves from each other / from
     * other targets within the subgraph so previous per-channel
     * connections don't linger.  Audio IO disconnects only --
     * MIDI plumbing stays alone (managed by the M1 sink wire
     * separately). */
    engine.disconnectNode (audioInIO,  true, true, true, false);
    engine.disconnectNode (audioOutIO, true, true, true, false);

    for (int i = 0; i < n; ++i)
    {
        Node& clip = clips[(size_t) i];
        const int outerLeft  = i * kStereoChannels;
        const int outerRight = outerLeft + 1;

        /* Record path: outer audio.input pair -> this clip's
         * input ports 0/1.  AudioClipNode is stereo; mono variant
         * just ignores the second channel. */
        engine.connectChannels (subgraph, audioInIO,  outerLeft,  clip, 0);
        engine.connectChannels (subgraph, audioInIO,  outerRight, clip, 1);

        /* Playback path: clip output 0/1 -> outer audio.output
         * pair.  Each clip lands on its OWN outer pair, not a
         * shared mix bus -- the 1:1 mapping user direction
         * 2026-05-26 confirmed. */
        engine.connectChannels (subgraph, clip, 0, audioOutIO, outerLeft);
        engine.connectChannels (subgraph, clip, 1, audioOutIO, outerRight);
    }
}

/* Ensure each MidiPlayerNode is wired to the subgraph's
 * midi.output IO (Option M1 default sink).  Idempotent -- skips
 * players that already have the connection.  Used by both the
 * spawn path + the migration pass. */
void ensureMidiSinkWires (EngineService& engine, const Node& subgraph)
{
    const Node midiOutIO = subgraph.getIONode (PortType::Midi, false);
    if (! midiOutIO.isValid()) return;

    for (Node& player : collectMidiPlayers (subgraph))
    {
        /* connectChannels is idempotent at the addConnection level
         * (rejects exact duplicates) so re-running this on a
         * player that's already wired is a no-op. */
        engine.connectChannels (subgraph, player, 0, midiOutIO, 0);
    }
}

/* Grid-snap every existing child of the Multi-Track subgraph so
 * the patchbay opens to a tidy layout instead of the default
 * pile.  Audio clips fill the audio row by ordinal, MIDI players
 * fill the MIDI row, IO nodes get pinned to the corners.  Removes
 * tags::x / tags::y on each child so the new relative position
 * is read by BlockComponent::updatePosition on the next paint
 * (existing absolute coords would otherwise win -- see comment
 * in snapNodePosition). */
void snapChildrenToGrid (const Node& subgraph)
{
    placeIONodesAtCorners (subgraph);

    auto audioClips = collectAudioClips (subgraph);
    for (size_t i = 0; i < audioClips.size(); ++i)
        snapNodePosition (audioClips[i], gridXForChild ((int) i), kAudioRowY);

    auto midiPlayers = collectMidiPlayers (subgraph);
    for (size_t i = 0; i < midiPlayers.size(); ++i)
        snapNodePosition (midiPlayers[i], gridXForChild ((int) i), kMidiRowY);
}

/* Full migration pass for an existing Multi-Track subgraph
 * (loaded from a pre-this-commit session, or any session where
 * the layout drifted).  Idempotent: re-running on an already-
 * migrated subgraph snaps identical positions + ensures the
 * same connections still exist. */
void migrateExistingSubgraph (EngineService& engine, const Node& subgraph)
{
    snapChildrenToGrid (subgraph);
    rewireAudioClips (engine, subgraph);
    ensureMidiSinkWires (engine, subgraph);
}

} // namespace

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

        /* Migration pass: snap existing children to the grid,
         * resize outer audio IO to 2*N (one stereo pair per clip),
         * rewire clips for 1:1 per-track direct-outs, ensure each
         * MidiPlayerNode is wired to midi.output (M1).
         * Idempotent so calling per session-load is safe even after
         * the layout is already correct -- relative positions don't
         * change, connections are reasserted but addConnection is
         * dedupe-safe. */
        migrateExistingSubgraph (engine, existing);
        return existing;
    }

    /* EngineService::addNode adds to the active root graph by default
     * (see services/engineservice.cpp:570 -- delegates to addPlugin
     * which lands on the active controller). */
    Node created = engine.addNode (EL_NODE_ID_ARRANGEMENT_TRACKS,
                                   EL_NODE_FORMAT_NAME);

    if (created.isValid())
    {
        created.setName ("Multi-Track");

        /* Fresh subgraph: start with zero outer audio channels
         * (will grow to 2 on first clip).  Park IO nodes at the
         * corners so the patchbay opens to a tidy layout when the
         * user first navigates in.  MIDI ports stay at the
         * GraphNode default (1 in / 1 out) -- single shared sink
         * per Q-S5. */
        setSubgraphAudioChannels (created, 0);
        placeIONodesAtCorners (created);
    }

    /* Subgraph -> root sink (master Audio Out) wiring intentionally
     * stays manual.  Per design philosophy, the main graph belongs
     * to the user; we don't auto-wire anything visible to them.
     * The user wires the Multi-Track's outer audio pairs to wherever
     * they want (master, FX chain, external send, ...). */

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

    /* Ordinal of the new clip = existing clip count.  Drives both
     * the grid column and the outer audio port pair this clip
     * will be wired to. */
    const int ordinal = (int) collectAudioClips (subgraph).size();
    const float rx = gridXForChild (ordinal);
    const float ry = kAudioRowY;

    /* Spawn via the rx/ry-aware overload so the BlockComponent
     * lands at the right grid column on FIRST paint -- not a
     * default-centre then async fixup that doesn't actually
     * refresh (the earlier failure mode). */
    Node clip = engine.addPlugin (subgraph, desc, rx, ry);
    if (! clip.isValid())
        return clip;

    /* Resize outer audio IO + rewire ALL clips (including the new
     * one) for 1:1 per-track direct-outs.  Each clip's input pair
     * comes from its dedicated outer pair on audio.input; output
     * pair goes to its dedicated outer pair on audio.output.  No
     * shared mix bus -- downstream user code on the main graph
     * decides what each pair feeds (master, FX, send, ...).
     *
     * Full rewire on every add is O(N) connections per add, O(N^2)
     * across N adds.  N is small (tens, capped by the user's
     * track count); the connect call is a cheap ValueTree write
     * + GraphManager bookkeeping.  Re-evaluate if N exceeds ~100. */
    rewireAudioClips (engine, subgraph);

    /* Point the new clip's recording target at a session-adjacent
     * "recordings/" directory if the session is saved, else fall
     * back to ~/Documents/Element Recordings/ (matches AudioClipNode
     * default).  Session-adjacent keeps captures with the project
     * file so a session move/copy keeps recordings together. */
    if (auto* sessSvc = engine.sibling<SessionService>())
    {
        const juce::File sessionFile = sessSvc->getSessionFile();
        if (sessionFile.existsAsFile())
        {
            const juce::File recDir =
                sessionFile.getParentDirectory().getChildFile ("recordings");

            if (auto* wrapper = clip.getObject())
            {
                if (auto* audioClipProc =
                        dynamic_cast<AudioClipNode*> (wrapper->getAudioProcessor()))
                {
                    audioClipProc->setRecordingDirectory (recDir);
                    juce::Logger::writeToLog (
                        juce::String ("[ArrangementTracksService::addAudioClipNode]"
                                       " recording dir set to ")
                        + recDir.getFullPathName());
                }
            }
        }
    }

    return clip;
}

void
ArrangementTracksService::migrateMultiTrackSubgraph (EngineService& engine,
                                                       const Node&    subgraph)
{
    if (! subgraph.isValid() || ! subgraph.isGraph()) return;

    /* Defensive identifier guard -- callers (GraphEditorComponent
     * navigation hook) should pre-filter but mis-targets here would
     * disconnect + rewire arbitrary user subgraphs.  Hard refuse. */
    if (subgraph.getProperty (tags::identifier).toString()
            != juce::String (EL_NODE_ID_ARRANGEMENT_TRACKS))
        return;

    migrateExistingSubgraph (engine, subgraph);
}

Node
ArrangementTracksService::addMidiPlayerNode (EngineService& engine,
                                              const Node&    subgraph)
{
    if (! subgraph.isValid() || ! subgraph.isGraph())
        return Node();

    juce::PluginDescription desc;
    desc.fileOrIdentifier = EL_NODE_ID_MIDI_PLAYER;
    desc.pluginFormatName = EL_NODE_FORMAT_NAME;
    desc.name             = "MIDI Player";

    /* Ordinal of the new player = existing player count.  Drives
     * the grid column on the MIDI row. */
    const int ordinal = (int) collectMidiPlayers (subgraph).size();
    const float rx = gridXForChild (ordinal);
    const float ry = kMidiRowY;

    Node player = engine.addPlugin (subgraph, desc, rx, ry);
    if (! player.isValid())
        return player;

    /* M1 default MIDI sink: auto-wire to the subgraph's midi.output
     * IO so the user wires the Multi-Track's outer MIDI face ONCE
     * to their synth / sampler.  All subsequent MIDI lanes route
     * through the same sink (multi-source summing handled by JUCE's
     * graph processor automatically -- no internal MIDI mixer
     * needed).  Q-S5 resolved 2026-05-26: MIDI stays as one shared
     * port; protocol-level multi-channel covers the multiplexing. */
    ensureMidiSinkWires (engine, subgraph);

    return player;
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
    /* Regions inherit their parent lane's tint -- ArrangementView's
     * rescanLaneTargets reassigns lane.colour from the palette on
     * load and propagates to regions, but seeding here keeps the
     * region visible at the correct tint between this write and the
     * first didBecomeActive of the arrangement view. */
    r.colour        = lane.colour;

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
