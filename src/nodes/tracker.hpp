// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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

    /** Edge-trigger: returns true once per wrap of the sequence's
     *  playhead (used for followAction at clip end).  Consumes the
     *  wrap on read — repeated calls in the same wrap window return
     *  false until the next wrap.  Internally tracks the previous
     *  song-relative row position per sequence. */
    bool   sequenceWrappedSinceLastQuery (int sequenceIdx) noexcept;

    /** Undo / redo.  Snapshots whole-module state into a memento stack.
     *  Editor calls pushUndo() before any mutation. */
    void pushUndo();
    bool canUndo() const noexcept;
    bool canRedo() const noexcept;
    void undo();
    void redo();
    void clearUndoHistory();

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

    /* Per-sequence previous song_pos cache for wrap-edge detection
     * in sequenceWrappedSinceLastQuery().  Lazily grown to mod_->nseq;
     * indices follow vht's seq[] array.  Updated only when the
     * session-view scheduler polls; not touched by render(). */
    juce::Array<double> lastSeqPos_;
};

} // namespace element
