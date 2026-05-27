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

/* Multi-Track patchbay helpers.  The Multi-Track is a regular
 * sub-graph that holds N AudioClipNode children + N MidiPlayerNode
 * children + 4 IO pseudo-nodes (audio.in/out, midi.in/out).  These
 * helpers give it smart spawn behaviour:
 *
 *  - Children land at grid positions (column = ordinal, row by
 *    kind).  rx/ry threaded through GraphManager::addNode BEFORE
 *    the BlockComponent is created so the position is visible on
 *    first paint.  Setting position post-spawn doesn't refresh
 *    block layout (block.cpp:1365 reads relativeX/Y once, writes
 *    absolute tags::x/y, then later relative writes are ignored).
 *
 *  - Outer audio IO grows to 2*N channels as audio clips are
 *    added so each clip gets its own dedicated outer pair (per-
 *    track direct-outs, 1:1 mapping -- user direction 2026-05-26).
 *
 *  - Each audio clip wires 1:1 to its dedicated outer pair on
 *    spawn.  Existing clips keep their wiring (growing IO ports
 *    is append-only, doesn't shift older indices on the IO side).
 *
 *  - Parent-graph cables to/from the Multi-Track block are
 *    snapshotted + replayed around the port-resize -- the resize
 *    fires removeIllegalConnections on the parent GraphManager,
 *    which would otherwise nuke the user's external wiring
 *    because PortCount's port list interleaves inputs-before-
 *    outputs-per-type, so growing inputs shifts output indices.
 *
 *  - MIDI stays as ONE shared midi.output IO (Q-S5).  Wired via
 *    EngineService::connect(PortType::Midi, ...) -- the audio-
 *    shortcut connectChannels overload is hardcoded to
 *    PortType::Audio (engineservice.cpp:487) and never worked
 *    for MIDI.
 */

/* Grid layout (relative 0..1 coords inside the subgraph canvas).
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

void placeIONodesAtCorners (const Node& subgraph)
{
    auto place = [] (Node n, float rx, float ry)
    {
        if (n.isValid()) n.setRelativePosition (rx, ry);
    };
    place (subgraph.getIONode (PortType::Audio, true  /*input*/),  kIoLeftX,  kIoTopY);
    place (subgraph.getIONode (PortType::Audio, false /*output*/), kIoLeftX,  kIoBottomY);
    place (subgraph.getIONode (PortType::Midi,  true),             kIoRightX, kIoTopY);
    place (subgraph.getIONode (PortType::Midi,  false),            kIoRightX, kIoBottomY);
}

/* Count children of `subgraph` with the given fileOrIdentifier.
 * Linear scan; lane counts are tens, fine. */
int countChildrenByPluginId (const Node& subgraph, const juce::String& pluginId)
{
    int n = 0;
    const int total = subgraph.getNumNodes();
    for (int i = 0; i < total; ++i)
    {
        const Node child = subgraph.getNode (i);
        if (child.getProperty (tags::identifier).toString() == pluginId)
            ++n;
    }
    return n;
}

int countAudioClips (const Node& subgraph)
{
    return countChildrenByPluginId (subgraph, juce::String (EL_NODE_ID_AUDIO_CLIP))
         + countChildrenByPluginId (subgraph, juce::String (EL_NODE_ID_AUDIO_CLIP_MONO));
}

int countMidiPlayers (const Node& subgraph)
{
    return countChildrenByPluginId (subgraph, juce::String (EL_NODE_ID_MIDI_PLAYER));
}

/* Walk `node`'s port list + return the channel ordinal (0-based
 * count within its type+direction group) for the port at the
 * given absolute index.  Inverse of absPortForChannel below.
 * Returns -1 if the abs index is out of range. */
int channelForAbsPort (const Node& node, int absPort) noexcept
{
    if (absPort < 0 || absPort >= node.getNumPorts()) return -1;
    const Port target = node.getPort (absPort);
    const PortType type = target.getType();
    const bool isInput = target.isInput();
    int channel = 0;
    for (int i = 0; i < absPort; ++i)
    {
        const Port p = node.getPort (i);
        if (p.getType() == type && p.isInput() == isInput)
            ++channel;
    }
    return channel;
}

/* Inverse of channelForAbsPort: find the absolute port index
 * matching (type, direction, ordinal).  Used after a port-count
 * resize to relocate connections by semantic identity instead
 * of stale absolute indices (PortCount's port list reorders when
 * counts change -- growing audio inputs shifts where audio
 * outputs sit). */
int absPortForChannel (const Node& node, PortType type, int channel, bool isInput) noexcept
{
    int count = 0;
    const int total = node.getNumPorts();
    for (int i = 0; i < total; ++i)
    {
        const Port p = node.getPort (i);
        if (p.getType() == type && p.isInput() == isInput)
        {
            if (count == channel) return i;
            ++count;
        }
    }
    return -1;
}

/* Find a child Node of `parent` whose nodeId matches.  Used by
 * the external-connection replayer to recover the OTHER node
 * referenced in a snapshotted connection. */
Node findChildById (const Node& parent, juce::uint32 nodeId)
{
    const int n = parent.getNumNodes();
    for (int i = 0; i < n; ++i)
    {
        Node child = parent.getNode (i);
        if (child.getNodeId() == nodeId) return child;
    }
    return Node();
}

/* Snapshot every connection on the PARENT graph that involves
 * the given subgraph.  Stored as semantic identity (type +
 * channel ordinal + direction) so re-applying after a port
 * resize finds the correct new absolute port indices.  Without
 * this, GraphNode::setNumPorts triggers removeIllegalConnections
 * on the parent GraphManager (via the Multi-Track's bound
 * NodeModelUpdater) and the user's main-graph cables to/from
 * the Multi-Track block get silently nuked because PortCount
 * reorders the port list when counts change. */
struct ExternalConn
{
    bool        subIsSrc;
    juce::uint32 otherNodeId;
    PortType    type;
    int         subChannel;
    int         otherChannel;
};

std::vector<ExternalConn>
snapshotExternalConnections (const Node& subgraph)
{
    std::vector<ExternalConn> snaps;
    const Node parentNode = subgraph.getParentGraph();
    if (! parentNode.isValid()) return snaps;
    auto* parentGraph = dynamic_cast<GraphNode*> (parentNode.getObject());
    if (parentGraph == nullptr) return snaps;

    const juce::uint32 subId = subgraph.getNodeId();
    const int total = parentGraph->getNumConnections();
    snaps.reserve ((size_t) total);

    for (int i = 0; i < total; ++i)
    {
        const auto* c = parentGraph->getConnection (i);
        if (c == nullptr) continue;

        if (c->sourceNode == subId)
        {
            const int subCh   = channelForAbsPort (subgraph, (int) c->sourcePort);
            const Node other  = findChildById (parentNode, c->destNode);
            if (! other.isValid() || subCh < 0) continue;
            const int otherCh = channelForAbsPort (other, (int) c->destPort);
            if (otherCh < 0) continue;
            const PortType type = subgraph.getPort ((int) c->sourcePort).getType();
            snaps.push_back ({ true, c->destNode, type, subCh, otherCh });
        }
        else if (c->destNode == subId)
        {
            const int subCh   = channelForAbsPort (subgraph, (int) c->destPort);
            const Node other  = findChildById (parentNode, c->sourceNode);
            if (! other.isValid() || subCh < 0) continue;
            const int otherCh = channelForAbsPort (other, (int) c->sourcePort);
            if (otherCh < 0) continue;
            const PortType type = subgraph.getPort ((int) c->destPort).getType();
            snaps.push_back ({ false, c->sourceNode, type, subCh, otherCh });
        }
    }
    return snaps;
}

void replayExternalConnections (EngineService& engine,
                                  const Node& subgraph,
                                  const std::vector<ExternalConn>& snaps)
{
    if (snaps.empty()) return;
    const Node parentNode = subgraph.getParentGraph();
    if (! parentNode.isValid()) return;
    const juce::uint32 subId = subgraph.getNodeId();

    for (const auto& s : snaps)
    {
        const Node other = findChildById (parentNode, s.otherNodeId);
        if (! other.isValid()) continue;

        /* Resolve channel ordinals back to absolute port indices on
         * the post-resize port lists.  If either side's port no
         * longer exists (e.g. a future shrink path drops channels),
         * silently skip -- there's no valid connection to restore. */
        const bool subIsInput = ! s.subIsSrc;
        const int subAbs      = absPortForChannel (subgraph, s.type,
                                                    s.subChannel, subIsInput);
        const bool otherIsInput = s.subIsSrc;
        const int otherAbs    = absPortForChannel (other, s.type,
                                                    s.otherChannel, otherIsInput);
        if (subAbs < 0 || otherAbs < 0) continue;

        const juce::uint32 srcNodeId = s.subIsSrc ? subId : s.otherNodeId;
        const juce::uint32 srcPort   = (juce::uint32) (s.subIsSrc ? subAbs : otherAbs);
        const juce::uint32 dstNodeId = s.subIsSrc ? s.otherNodeId : subId;
        const juce::uint32 dstPort   = (juce::uint32) (s.subIsSrc ? otherAbs : subAbs);

        engine.addConnection (srcNodeId, srcPort, dstNodeId, dstPort, parentNode);
    }
}

/* Resize the subgraph's outer audio IO to the given channel
 * count.  Sync (not async) so the new port topology + IO node
 * port lists are in place by the time we add the new clip's
 * wiring.  Triggers portsChanged signals on both the subgraph
 * GraphNode (parent's NodeModelUpdater + removeIllegalConnections)
 * and each IO pseudo-node (the inside-view BlockComponent
 * refresh path -- see IONode::refreshPorts portsChanged emit). */
void setSubgraphAudioChannels (const Node& subgraph, int channels)
{
    if (auto* proc = subgraph.getObject())
    {
        if (auto* graph = dynamic_cast<GraphNode*> (proc))
        {
            const int currentIn  = graph->getNumPorts (PortType::Audio, true);
            const int currentOut = graph->getNumPorts (PortType::Audio, false);
            if (currentIn == channels && currentOut == channels) return;
            graph->setNumPorts (PortType::Audio, channels, true,  false /*sync*/);
            graph->setNumPorts (PortType::Audio, channels, false, false /*sync*/);
        }
    }
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
        /* Lock canonical name on every lookup -- user-typo renames
         * persist in the ValueTree and the patchbay breadcrumb shows
         * the typo across sessions.  Idempotent: skipped when the
         * name already matches. */
        if (existing.getName() != "Multi-Track")
            existing.setName ("Multi-Track");
        return existing;
    }

    /* Fresh creation.  EngineService::addNode lands on the active
     * root graph (engineservice.cpp:570 delegates to addPlugin
     * against the active controller). */
    Node created = engine.addNode (EL_NODE_ID_ARRANGEMENT_TRACKS,
                                   EL_NODE_FORMAT_NAME);

    if (created.isValid())
    {
        created.setName ("Multi-Track");

        /* Fresh subgraph: outer audio starts at 0 channels + grows
         * as audio clips are added (addAudioClipNode bumps to
         * 2*N).  MIDI stays at the GraphNode default (1 in / 1 out)
         * -- single shared sink per Q-S5.  Park IO pseudo-nodes
         * at the four corners of the patchbay canvas so the
         * inside view opens tidy. */
        setSubgraphAudioChannels (created, 0);
        placeIONodesAtCorners (created);
    }

    /* Subgraph -> root sink wiring stays manual.  The user wires
     * the Multi-Track's outer audio pairs to whatever they want
     * (master, FX chain, external send) from the main graph. */
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

    /* Ordinal of the new clip = existing clip count BEFORE spawn.
     * Drives both the grid column and the outer audio port pair
     * this clip wires to. */
    const int ordinal = countAudioClips (subgraph);
    const float rx = gridXForChild (ordinal);
    const float ry = kAudioRowY;

    /* Spawn with rx/ry threaded through GraphManager::addNode so
     * the BlockComponent lands at the right grid column on FIRST
     * paint -- BlockComponent::updatePosition reads relativeX/Y
     * once at construction (block.cpp:1365) so post-spawn
     * setRelativePosition calls are silently ignored. */
    Node clip = engine.addPlugin (subgraph, desc, rx, ry);
    if (! clip.isValid())
        return clip;

    /* Grow outer audio IO to fit the new clip, snapshotting +
     * replaying parent-graph external connections around the
     * resize so user cables to/from the Multi-Track block survive.
     *
     * Why the snapshot/replay: GraphNode::setNumPorts emits
     * portsChanged which triggers removeIllegalConnections on the
     * parent GraphManager.  PortCount's port list interleaves
     * inputs-before-outputs-per-type (portcount.hpp:80-105), so
     * growing audio inputs shifts the absolute index of audio
     * outputs (and midi).  Connections stored by abs port index
     * become invalid + get removed.  Snapshotting by semantic
     * identity (type + channel ordinal within direction) lets us
     * replay against the new port list. */
    {
        auto snaps = snapshotExternalConnections (subgraph);
        setSubgraphAudioChannels (subgraph, (ordinal + 1) * kStereoChannels);
        replayExternalConnections (engine, subgraph, snaps);
    }

    /* Wire ONLY the new clip to its dedicated outer pair.  Existing
     * clips keep their wiring -- growing IO ports is append-only
     * on the IO node side (channels 2N, 2N+1 are NEW; channels 0..2N-1
     * keep their abs port indices).  No tear-down dance needed.
     *
     * Use EngineService::connect (PortType::Audio, ...) -- the
     * connectChannels shortcut overload (engineservice.cpp:472)
     * is hardcoded to Audio AND has no port-type discrimination
     * in its impl which masks failures; connect with explicit type
     * is the correct path. */
    const Node audioInIO  = subgraph.getIONode (PortType::Audio, true);
    const Node audioOutIO = subgraph.getIONode (PortType::Audio, false);
    if (audioInIO.isValid() && audioOutIO.isValid())
    {
        const int outerLeft  = ordinal * kStereoChannels;
        const int outerRight = outerLeft + 1;

        /* Record path: outer audio.input pair -> this clip's
         * stereo input.  Mono clip variant ignores the second. */
        engine.connect (PortType::Audio, audioInIO,  outerLeft,  clip, 0, 1);
        engine.connect (PortType::Audio, audioInIO,  outerRight, clip, 1, 1);

        /* Playback path: clip's stereo output -> outer audio.output
         * pair.  Per-track direct-out (1:1 mapping), not shared
         * mix bus -- user direction 2026-05-26. */
        engine.connect (PortType::Audio, clip, 0, audioOutIO, outerLeft,  1);
        engine.connect (PortType::Audio, clip, 1, audioOutIO, outerRight, 1);
    }

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

    /* Ordinal = existing MIDI player count.  Drives the grid
     * column on the MIDI row. */
    const int ordinal = countMidiPlayers (subgraph);
    const float rx = gridXForChild (ordinal);
    const float ry = kMidiRowY;

    Node player = engine.addPlugin (subgraph, desc, rx, ry);
    if (! player.isValid())
        return player;

    /* M1 default MIDI sink: auto-wire MidiPlayerNode.midi.out ->
     * subgraph.midi.output IO so the user wires the Multi-Track's
     * outer MIDI face ONCE to their synth / sampler.  All MIDI
     * players route through the same sink; JUCE's graph processor
     * sums MIDI buffers across connections automatically.
     *
     * Uses engine.connect (PortType::Midi, ...) -- the
     * connectChannels shortcut overload is hardcoded to
     * PortType::Audio (engineservice.cpp:487) and silently fails
     * for MIDI nodes (the previous M1 wire never actually worked). */
    const Node midiOutIO = subgraph.getIONode (PortType::Midi, false);
    if (midiOutIO.isValid())
    {
        engine.connect (PortType::Midi, player, 0, midiOutIO, 0, 1);
    }

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
