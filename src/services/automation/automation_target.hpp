// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/parameter.hpp>
#include <element/juce/audio_processors.hpp>
#include <element/juce/audio_basics.hpp>

#include <algorithm>
#include <cstdint>

namespace element::automation {

/** A resolved binding between an AutomationTrack's persistent target
 *  key and the LIVE writable destination on the audio graph.  The
 *  AutomationEngine builds + maintains these per-track, refreshing
 *  whenever the graph topology changes (nodes added / removed,
 *  plugins re-scanned).
 *
 *  Three kinds of destination:
 *
 *    PluginParam -- a juce::AudioProcessorParameter on a hosted VST/
 *                   CLAP/AU/etc instance.  In Phase 1 the engine
 *                   writes COARSE (one setValue per block).  Sample-
 *                   accurate plugin emission deferred to Phase 1.5.
 *
 *    NodeParam   -- an element::Parameter owned by an internal
 *                   Element node (TrackerNode BPM, SamplerNode mix
 *                   levels, AudioClipNode gain, etc).  Engine writes
 *                   COARSE here too; sample-accurate consumption is
 *                   the NODE's responsibility once Phase 1.5/4 wires
 *                   ParamChange event lists through.
 *
 *    MidiCc      -- a (channel, CC number) pair.  Engine emits one or
 *                   more juce::MidiMessage::controllerEvent into a
 *                   provided outgoing MidiBuffer at the right sample
 *                   offsets.  Already sample-accurate by construction.
 *
 *  Pointer ownership: pluginParam_ + nodeParam_ are raw non-owning.
 *  Backing objects live in the GraphProcessor / plugin instances;
 *  the engine invalidates a target (setting kind = Invalid) before
 *  the backing object is destroyed, on graph-topology callbacks.
 *  Audio thread never derefs an Invalid target.
 *
 *  Plain struct -- copyable, trivially destructible.  Stored by the
 *  engine in a heap allocation + atomic<const AutomationTarget*> on
 *  the AutomationTrack, with epoch-gated reclaim for rebinds (so the
 *  audio thread can hold the pointer across the block boundary
 *  without UAF).  Atomic-swap of the target ptr happens at engine
 *  bind/unbind time, NOT per render. */
struct AutomationTarget
{
    enum class Kind : std::uint8_t
    {
        Invalid = 0,
        PluginParam,
        NodeParam,
        MidiCc,
    };

    Kind                            kind        { Kind::Invalid };

    /* Active for kind == PluginParam.  Non-owning. */
    juce::AudioProcessorParameter*  pluginParam { nullptr };

    /* Active for kind == NodeParam.  Non-owning ref-counted handle
     *  copied by value into the target after binding -- holds the
     *  Parameter alive against transient graph rebuilds.  Listeners
     *  see no change. */
    element::ParameterPtr           nodeParam;

    /* Active for kind == MidiCc.  JUCE conventions: channel in
     * [1, 16], ccNumber in [0, 127]. */
    int                             midiChannel { 0 };
    int                             midiCcNumber{ -1 };

    bool isValid() const noexcept { return kind != Kind::Invalid; }

    //==========================================================================
    // Audio-thread-safe dispatch.

    /** Write a normalised value to this target.  COARSE: one write,
     *  no frame offset.  Audio-thread safe.
     *
     *  v expected in [0, 1]; clamped internally so callers don't need
     *  to pre-clamp on every block.  For MidiCc the value maps to
     *  [0, 127] and emits a single controllerEvent at frameOffset 0
     *  into outMidi (if non-null; null = drop, used when the engine
     *  is in a no-MIDI-output context). */
    void writeCoarseValue (float v, juce::MidiBuffer* outMidi = nullptr) const noexcept
    {
        const float clamped = std::clamp (v, 0.0f, 1.0f);

        switch (kind)
        {
            case Kind::PluginParam:
                if (pluginParam != nullptr)
                {
                    /* setValue() bypasses the host-notify path (no
                     * gesture, no listener callback).  This is the
                     * correct write for automation -- setValueNotifying
                     * is for UI->plugin direction. */
                    pluginParam->setValue (clamped);
                }
                break;

            case Kind::NodeParam:
                if (nodeParam != nullptr)
                {
                    /* Same intent for Element's parameter type -- the
                     * silent setValue path skips host-notify so the
                     * automation write doesn't fight the listener
                     * loop. */
                    nodeParam->setValue (clamped);
                }
                break;

            case Kind::MidiCc:
                if (outMidi != nullptr && midiCcNumber >= 0)
                    emitMidiCc (*outMidi, /*frameOffset*/ 0, clamped);
                break;

            case Kind::Invalid:
            default:
                break;
        }
    }

    /** Emit a single sample-accurate event at a frame offset within
     *  the current block.  Only meaningful for MidiCc kind; no-op
     *  for other kinds (in Phase 1 plugin + node params are coarse-
     *  per-block; sample-accurate consumption is a node-side
     *  responsibility).  Audio-thread safe. */
    void emitEventAt (int frameOffset, float v, juce::MidiBuffer& outMidi) const noexcept
    {
        if (kind == Kind::MidiCc && midiCcNumber >= 0)
            emitMidiCc (outMidi, frameOffset, std::clamp (v, 0.0f, 1.0f));
    }

private:
    void emitMidiCc (juce::MidiBuffer& outMidi, int frameOffset, float vClamped) const noexcept
    {
        const int chan = juce::jlimit (1, 16, midiChannel == 0 ? 1 : midiChannel);
        const int cc   = juce::jlimit (0, 127, midiCcNumber);
        const int val  = juce::jlimit (0, 127, juce::roundToInt (vClamped * 127.0f));
        outMidi.addEvent (juce::MidiMessage::controllerEvent (chan, cc, val), frameOffset);
    }
};

} // namespace element::automation
