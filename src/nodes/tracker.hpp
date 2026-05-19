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
 * AudioProcessor. Phase 1 smoke-test surface: 1 MIDI output, hardcoded
 * test pattern at 120 BPM, plays on load. Real UI / multi-port / state
 * persistence land in Phase 2+.
 *
 * See ~/wine-nspa-notes/tracker-research/DESIGN.md +
 * vht-engine-notes.md for architecture context. */
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
    /** Build a hardcoded 4-row test pattern so Phase 1 emits MIDI without
     *  needing a UI. Real authoring lands in Phase 2. */
    void installTestPattern();

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
};

} // namespace element
