// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if ELEMENT_USE_JACK

#include <element/context.hpp>
#include <element/devices.hpp>
#include <element/node.h>

#include "nodes/baseprocessor.hpp"
#include "engine/jack.hpp"

namespace element {

/** Element: combined-mode native JACK MIDI input source node.
 *
 *  Functional equivalent of the legacy ALSA-seq "MIDI Input Device"
 *  node for users who want every enabled inbound JACK MIDI port as a
 *  single stream rather than per-port routing.  At processBlock time
 *  it copies the JackAudioIODevice's combined input MidiBuffer
 *  (already populated sample-accurately by the per-period drain) into
 *  the node's output MIDI port.  Per-port routing is still available
 *  via the JackMidiInputNode type — both can coexist in the same
 *  graph. */
class JackMidiInputAllNode : public BaseProcessor
{
public:
    explicit JackMidiInputAllNode (Context& ctx)
        : context (ctx)
    {
        setPlayConfigDetails (0, 0, 44100.0, 512);
    }

    ~JackMidiInputAllNode() override = default;

    inline const juce::String getName() const override
    {
        return juce::String ("JACK MIDI Input (All)");
    }

    inline void fillInPluginDescription (juce::PluginDescription& desc) const override
    {
        desc.name              = "JACK MIDI Input (All)";
        desc.fileOrIdentifier  = EL_NODE_ID_JACK_MIDI_INPUT_ALL;
        desc.uniqueId          = EL_NODE_UID_JACK_MIDI_INPUT_ALL;
        desc.descriptiveName   = "Combined sample-accurate JACK MIDI input";
        desc.numInputChannels  = 0;
        desc.numOutputChannels = 0;
        desc.hasSharedContainer = false;
        desc.isInstrument      = false;
        desc.category          = "MIDI";
        desc.manufacturerName  = EL_NODE_FORMAT_AUTHOR;
        desc.pluginFormatName  = EL_NODE_FORMAT_NAME;
        desc.version           = "1.0.0";
    }

    inline void prepareToPlay (double sampleRate, int blockSize) override
    {
        setPlayConfigDetails (0, 0, sampleRate, blockSize);
    }

    inline void releaseResources() override {}

    inline void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer& midiMessages) override
    {
        midiMessages.clear();

        auto* dev = context.devices().getCurrentAudioDevice();
        auto* provider = dynamic_cast<JackMidiInputProvider*> (dev);
        if (provider == nullptr)
            return;

        const auto& src = provider->getCurrentJackMidiInput();
        if (! src.isEmpty())
            midiMessages.addEvents (src, 0, -1, 0);
    }

    inline double getTailLengthSeconds() const override { return 0.0; }
    inline bool acceptsMidi()  const override { return false; }
    inline bool producesMidi() const override { return true; }
    inline bool isMidiEffect() const override { return true; }

    inline int  getNumPrograms() override { return 1; }
    inline int  getCurrentProgram() override { return 0; }
    inline void setCurrentProgram (int) override {}
    inline const juce::String getProgramName (int) override { return {}; }
    inline void changeProgramName (int, const juce::String&) override {}

    inline juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    inline bool hasEditor() const override { return false; }

    inline void getStateInformation (juce::MemoryBlock&) override {}
    inline void setStateInformation (const void*, int) override {}

protected:
    inline bool isBusesLayoutSupported (const BusesLayout&) const override { return false; }
    inline bool canApplyBusesLayout (const BusesLayout& layouts) const override
    {
        return isBusesLayoutSupported (layouts);
    }

private:
    Context& context;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JackMidiInputAllNode)
};

} // namespace element

#endif // ELEMENT_USE_JACK
