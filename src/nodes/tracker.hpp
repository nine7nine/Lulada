// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include <element/midipipe.hpp>
#include "nodes/midifilter.hpp"
#include "nodes/baseprocessor.hpp"

/* Engine is C; bracket includes so C++ name mangling doesn't break linking. */
extern "C" {
#include "engine/tracker/module.h"
#include "engine/tracker/sequence.h"
#include "engine/tracker/track.h"
#include "engine/tracker/row.h"
#include "engine/tracker/midi_event.h"
#include "engine/tracker/midi_client.h"
}

namespace element {

/** Internal tracker / pattern sequencer node, MIDI-only.
 *
 * Wraps the vendored vht engine (src/engine/tracker/*.c) as a JUCE-side
 * AudioProcessor. */
class TrackerNode : public MidiFilterNode
{
public:
    TrackerNode();
    ~TrackerNode() override;

    void getPluginDescription (juce::PluginDescription& desc) const override
    {
        desc.fileOrIdentifier  = EL_NODE_ID_MIDI_SEQUENCER;
        desc.name              = "Tracker";
        desc.descriptiveName   = "Pattern-based MIDI tracker and sequencer";
        desc.numInputChannels  = 0;
        desc.numOutputChannels = 0;
        desc.hasSharedContainer = false;
        desc.isInstrument      = false;
        desc.category          = "Sequencer";
        desc.manufacturerName  = EL_NODE_FORMAT_AUTHOR;
        desc.pluginFormatName  = EL_NODE_FORMAT_NAME;
        desc.version           = "0.1.0";
        desc.uniqueId          = EL_NODE_UID_MIDI_SEQUENCER;
    }

    void prepareToRender (double sampleRate, int maxBufferSize) override;
    void releaseResources() override;
    void render (RenderContext& rc) override;

    void setState (const void* data, int size) override;
    void getState (juce::MemoryBlock& block) override;

    /** Editor access — read pattern state under engineLock(). */
    juce::CriticalSection& engineLock() noexcept { return engineLock_; }
    module* modulePtr() noexcept { return mod_; }

    /** Programmatically jump to a specific pattern at the next buffer
     *  boundary.  Called by the main-window arrangement view to drive
     *  pattern sequencing via direct C++ call — no MIDI program-change
     *  plumbing.  Thread-safe: acquires engineLock_.  Out-of-range
     *  indices are silently ignored. */
    void advanceToPattern (int patternIdx);

    /** Current pattern index (0-based).  Returns -1 if engine is
     *  uninitialised.  Used by the arrangement view to highlight the
     *  active block. */
    int currentPatternIndex() const noexcept;
    int numPatterns() const noexcept;

    /* ---------------------------------------------------------------
     * Session-view API — operates on vht's per-sequence `playing` flag
     * directly, distinct from `advanceToPattern` which uses the
     * pattern-switch (single-playhead) model.  Multiple sequences in
     * the same module can have playing=1 concurrently; module_advance
     * iterates all of them and each one's emitted events land in the
     * shared per-port MIDI buffer, time-sorted in drainEngineToMidi.
     *
     * All methods acquire engineLock_.  Safe to call from the message
     * thread; calls are infrequent (UI click rate / 30 Hz poll). */

    /** Append a new empty sequence (one track, default channel/port)
     *  with `rowsLength` rows.  Returns the new sequence's index, or
     *  -1 if the engine isn't initialised.  The new sequence starts
     *  with playing=0 — clip launcher arms it explicitly. */
    int  createSequence (int rowsLength = 16);

    /** Clone an existing sequence (deep-copy via vht's sequence_clone)
     *  and append it to the module.  Returns the new sequence's index,
     *  or -1 if the source index is invalid.  The clone starts with
     *  playing=0 regardless of source state. */
    int  cloneSequence  (int sourceIdx);

    /** Remove a sequence by index.  No-op when nseq <= 1 (vht needs
     *  at least one sequence) or when the index is out of range.  If
     *  the removed sequence was `curr_seq`, the module's `curr_seq`
     *  rolls over to a sibling so vht's internal state stays valid. */
    void removeSequence (int sequenceIdx);

    /** Flip a single sequence's playing flag without touching
     *  `curr_seq`.  This is the session-view launch primitive.  When
     *  turning a clip on, the sequence + all its tracks rewind to
     *  position 0 — Bitwig/Ableton "launch restarts from start". */
    void setSequencePlaying (int sequenceIdx, bool on);

    bool   isSequencePlaying       (int sequenceIdx) const noexcept;
    double getSequencePositionRows (int sequenceIdx) const noexcept;
    int    getSequenceLengthRows   (int sequenceIdx) const noexcept;

    /** Sequence length in beats = length_rows / rpb.  Returns 0.0 for
     *  out-of-range indices or null sequences.  Used by ArrangementView
     *  to schedule region cutoffs for non-looped tracker regions:
     *  cutoff = min(region.lengthBeats, getSequenceLengthBeats(seqIdx)). */
    double getSequenceLengthBeats  (int sequenceIdx) const noexcept;

    /** Edge-trigger: returns true once per wrap of the sequence's
     *  playhead (used for followAction at clip end).  Consumes the
     *  wrap on read — repeated calls in the same wrap window return
     *  false until the next wrap.  Internally tracks the previous
     *  song-relative row position per sequence. */
    bool   sequenceWrappedSinceLastQuery (int sequenceIdx) noexcept;

    /** Schedule a future flip of a sequence's playing flag.
     *
     *  beatTarget < 0 means "immediate" (fires at the start of the
     *  next render block).  beatTarget >= 0 is a transport-beat
     *  position; the flip fires inside the render block whose beat
     *  range contains it — sample-accurate to ±1 audio block, with
     *  zero inter-clip skew for clips targeting the same beat.
     *
     *  Lock-free SPSC: safe to call from the message thread without
     *  touching engineLock_.  Latest request per sequenceIdx wins —
     *  the audio thread keeps only the most recent pending action
     *  per sequence, so re-banging a queued clip cancels the prior
     *  request.
     *
     *  Backpressure: if the internal FIFO fills (way over expected
     *  click rate), the new request is dropped silently.  In
     *  practice 64-slot FIFO at human click rate never fills. */
    void   schedulePlaying (int sequenceIdx, double beatTarget, bool wantPlaying) noexcept;

    /** Session-view mute / solo state for this tracker.  Both flags
     *  live on the node so the session view AND the tracker editor
     *  popup can show one consistent state.  These are USER intent;
     *  the effective engine mute (Processor::isMuted) is reconciled
     *  by SessionView -- when any tracker is soloed, non-soloed
     *  trackers get setMuted(true), otherwise setMuted(getUserMuted()). */
    bool getUserMuted() const noexcept { return userMuted_; }
    void setUserMuted (bool b) noexcept { userMuted_ = b; }
    bool getSoloed() const noexcept { return soloed_; }
    void setSoloed (bool b) noexcept { soloed_ = b; }

    /** Undo / redo.  Snapshots whole-module state into a memento stack.
     *  Editor calls pushUndo() before any mutation. */
    void pushUndo();
    bool canUndo() const noexcept;
    bool canRedo() const noexcept;
    void undo();
    void redo();
    void clearUndoHistory();

    /** Callback invoked right after each pushUndo() local-stack
     *  snapshot fires.  The TrackerEditor binds this to push a
     *  bridge UndoableAction into the global GuiService UndoManager
     *  so Cmd+Z / Cmd+Shift+Z unwind tracker mutations alongside
     *  graph / arrangement / session view edits.  Local undo / redo
     *  remain the source of truth for state mementos; the bridge
     *  action delegates back via TrackerNode::undo() / redo(). */
    std::function<void()> onPushUndo;

protected:
    void refreshPorts() override;

private:
    /** Build the empty default pattern (2 tracks, 16 rows, no notes)
     *  on first prepareToPlay so the module starts in a valid state. */
    void installDefaultPattern();

    /** Drain engine output buffers into per-port JUCE MidiBuffers. Replaces
     *  vht's jack_process.c midi_buffer_flush. */
    void drainEngineToMidi (RenderContext& rc, int numSamples);

    module* mod_ = nullptr;
    bool createdPorts_ = false;
    juce::uint64 sampleCounter_ = 0;
    double currentSampleRate_ = 48000.0;
    int currentBufferSize_ = 1024;
    bool lastPlayingState_ = false;
    juce::CriticalSection engineLock_;

    /* Undo / redo memento stacks. Each entry is a serialised state
     * blob (same wire format as getState). Capped at 64 entries to
     * keep memory bounded. */
    enum { kMaxUndo = 64 };
    juce::Array<juce::MemoryBlock> undoStack_;
    juce::Array<juce::MemoryBlock> redoStack_;

    /* Session-view user mute + solo state.  Read + written by both
     * SessionView and TrackerEditor; not used by render() directly. */
    bool userMuted_ = false;
    bool soloed_    = false;

    /* Per-sequence previous song_pos cache for wrap-edge detection
     * in sequenceWrappedSinceLastQuery().  Lazily grown to mod_->nseq;
     * indices follow vht's seq[] array.  Updated only when the
     * session-view scheduler polls; not touched by render(). */
    juce::Array<double> lastSeqPos_;

    /* ---------- Phase-4 sample-accurate launch scheduler ---------- */

    /** One request in the message-thread → audio-thread FIFO. */
    struct LaunchReq {
        int    sequenceIdx;
        double beatTarget;    // <0 = immediate
        int    wantPlaying;   // 0 or 1
    };

    /** Per-sequence pending flip, owned by the audio thread.  Latest
     *  FIFO request for a given sequenceIdx overwrites the slot,
     *  giving us implicit cancellation. */
    struct PendingAction {
        double beatTarget   = 0.0;
        bool   wantPlaying  = false;
        bool   valid        = false;
    };

    static constexpr int kLaunchFifoSize = 64;
    std::array<LaunchReq, (size_t) kLaunchFifoSize> launchFifoStorage_ {};
    juce::AbstractFifo launchFifo_ { kLaunchFifoSize };
    juce::Array<PendingAction> pendingActions_;   // indexed by sequenceIdx

    /** Audio-thread helpers — called from inside render() under
     *  engineLock_, before module_advance. */
    void drainLaunchFifo() noexcept;
    void applyPendingForBlock (double blockStartBeat, double blockEndBeat) noexcept;
};

} // namespace element
