// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/midipipe.hpp>
#include "nodes/midifilter.hpp"
#include "services/timeline/midi_note_region.hpp"

#include <array>
#include <atomic>
#include <bitset>

namespace element {

/** Arrangement-timeline MIDI player.  Counterpart of AudioClipNode for
 *  the MIDI lane kind -- emits NoteOn / NoteOff (+ velocity, channel)
 *  on the audio thread by walking the bound MidiNoteRegion's COW
 *  snapshot at each block's beat range.
 *
 *  ## Why MidiPlayerNode is a real graph node
 *
 *  The user's mental model from the tracker pattern: add a node ->
 *  wire its MIDI output to a Sampler / synth -> notes come out at
 *  transport time.  ArrangementView's MIDI lane spawns one of these
 *  internally so the user doesn't have to (just like
 *  createEmptyAudioLane spawns an AudioClipNode), but the node is
 *  still a first-class graph citizen -- you can wire its output
 *  anywhere, swap the bound synth, route through MidiChannelMap, etc.
 *
 *  ## Topology
 *
 *  MIDI-only output node.  No MIDI input (Session 2 paint-only --
 *  later phases may add live-record by promoting this to MidiFilter
 *  with input + output; for now keep the surface minimal).  Two
 *  ports: control "active" + MIDI output.
 *
 *  Actually we publish ONE MIDI output port and no other ports.
 *  TrackerNode adds a MIDI input + output; MidiPlayerNode keeps just
 *  the output since live-record into a piano-roll is a Session 3
 *  feature.
 *
 *  ## Region binding
 *
 *  Per-lane MidiPlayerNode tracks an array of bound MidiNoteRegions
 *  via a lock-free pointer table (RegionEntry).  Message thread
 *  publishes a new entry array on every playlist mutation (region
 *  added / removed / moved / resized); audio thread loads the
 *  current entry table once per block.
 *
 *  The entries themselves point at MidiNoteRegions that live in the
 *  Lane's Playlist (on the message thread).  Their lifetime is at
 *  least the message thread's PianoRoll edit-mutate path: the same
 *  COW snapshot discipline that MidiNoteRegion enforces (epoch trash
 *  + UI sweep) keeps the live snapshot pointer valid for the
 *  duration of one audio block.
 *
 *  Region deletion + lane teardown:
 *   - Message thread must `setBoundRegions ({})` BEFORE destroying
 *     the Playlist / Lane so the audio thread observes the empty
 *     table before the regions go away.  ArrangementView's
 *     willBeRemoved + rescanLaneTargets handle this.
 *
 *  ## Audio-thread render loop
 *
 *  Per block:
 *    1. Read transport: blockStartBeat, blockEndBeat, isPlaying.
 *    2. If not playing or no monitor info, emit all-notes-off for
 *       any currently-held notes, return.
 *    3. Load the bound region entry table (atomic snapshot).
 *    4. For each entry whose [position, position+length) overlaps
 *       the block range:
 *         - Map block beat -> local region beat (handle looped).
 *         - Walk the region's note snapshot in the local-beat
 *           window; emit NoteOn at sample offset = (onBeat-blockStart)
 *           * samplesPerBeat.
 *         - Emit NoteOff for notes whose end falls in the block range.
 *    5. Track which notes are currently held (per channel + pitch)
 *       so transport-stop / region-exit can send the matching OFFs.
 *
 *  Lock-free.  No allocations on the audio thread. */
class MidiPlayerNode : public MidiFilterNode
{
public:
    MidiPlayerNode();
    ~MidiPlayerNode() override;

    void getPluginDescription (juce::PluginDescription& desc) const override
    {
        desc.fileOrIdentifier   = EL_NODE_ID_MIDI_PLAYER;
        desc.name               = "MIDI Player";
        desc.descriptiveName    = "Arrangement MIDI playback (piano-roll lane)";
        desc.numInputChannels   = 0;
        desc.numOutputChannels  = 0;
        desc.hasSharedContainer = false;
        desc.isInstrument       = false;
        desc.category           = "Source";
        desc.manufacturerName   = EL_NODE_FORMAT_AUTHOR;
        desc.pluginFormatName   = EL_NODE_FORMAT_NAME;
        desc.version            = "0.1.0";
        desc.uniqueId           = EL_NODE_UID_MIDI_PLAYER;
    }

    void prepareToRender (double sampleRate, int maxBufferSize) override;
    void releaseResources() override;
    void render (RenderContext& rc) override;

    void setState (const void* data, int size) override;
    void getState (juce::MemoryBlock& block) override;

    //==========================================================================
    // Region binding API.
    //
    // Message thread builds a fresh entries vector reflecting the lane's
    // current playlist + calls setBoundRegions; audio thread observes
    // the swap atomically at the start of the next render block.
    //
    // The pointer table is at most ~32 entries (lanes don't carry
    // hundreds of regions in practice).  When a region's onBeat /
    // lengthBeats / loop flag changes the message thread re-publishes
    // a fresh entries array; the underlying MidiNoteRegion pointer can
    // stay the same, only the cached position metadata is reread.

    struct RegionEntry
    {
        MidiNoteRegion* region        { nullptr };
        double          positionBeats { 0.0 };
        double          lengthBeats   { 0.0 };
        bool            looped        { false };
    };

    /** Replace the active region table.  Message thread only.  The
     *  prior table is discarded after a one-block grace period --
     *  enforced via the same epoch-trash discipline as
     *  MidiNoteRegion: every render block bumps audioEpoch_; sweep
     *  drops trashed tables whose stamp is strictly less than the
     *  current epoch. */
    void setBoundRegions (std::vector<RegionEntry> entries);

    /** Drain the trash deque.  Called on the message thread by a
     *  Timer / AsyncUpdater after region edits.  Safe to call from
     *  any non-audio thread.  Idempotent. */
    void sweepBindingsTrash() noexcept;

private:
    bool                createdPorts_   { false };

    double              currentSampleRate_ { 48000.0 };
    int                 currentBlockSize_  { 1024 };
    bool                lastPlayingState_  { false };

    /** Active entry table (atomic pointer).  Always non-null after
     *  ctor publishes an empty table. */
    using EntryList = std::vector<RegionEntry>;
    std::atomic<const EntryList*> activeEntries_ { nullptr };

    /** Trash entry + epoch counter -- mirrors MidiNoteRegion's discipline. */
    struct EntryTrash
    {
        std::unique_ptr<const EntryList> entries;
        std::uint64_t                    stampEpoch;
    };
    std::deque<EntryTrash>          entriesTrash_;
    std::atomic<std::uint64_t>      audioEpoch_ { 0 };

    /** Bitset of currently-held (channel, pitch) pairs -- one bit per
     *  pair.  16 channels x 128 pitches = 2048 bits = 256 bytes.
     *  Audio-thread-owned; never touched by message thread. */
    static constexpr int kHeldBits = 16 * 128;
    std::bitset<kHeldBits> held_ {};

    static int heldIndex (int channel /*1..16*/, int pitch /*0..127*/) noexcept
    {
        const int ch = juce::jlimit (1, 16, channel) - 1;
        const int p  = juce::jlimit (0, 127, pitch);
        return ch * 128 + p;
    }

    void emitAllNotesOff (juce::MidiBuffer& out, int sampleOffset) noexcept;

    /** Beat range covered by the current render block, in transport
     *  beats.  Returned in `blockStartBeat` / `blockEndBeat`.  Returns
     *  false if the host hasn't provided enough info (no playhead,
     *  zero bpm, etc.) -- caller emits no events in that case. */
    bool computeBlockBeats (int numSamples,
                              double& blockStartBeat,
                              double& blockEndBeat,
                              double& samplesPerBeat) noexcept;

    /** For one region entry + computed beat window, emit any
     *  NoteOn / NoteOff events whose sample-offset falls in
     *  [0, numSamples).  Handles loop wrap. */
    void emitRegionInBlock (const RegionEntry& entry,
                              double             blockStartBeat,
                              double             blockEndBeat,
                              double             samplesPerBeat,
                              int                numSamples,
                              juce::MidiBuffer&  out) noexcept;

protected:
    void refreshPorts() override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiPlayerNode)
};

} // namespace element
