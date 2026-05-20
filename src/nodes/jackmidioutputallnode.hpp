// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if ELEMENT_USE_JACK

#include <element/devices.hpp>
#include <element/node.h>

#include "nodes/baseprocessor.hpp"
#include "engine/jack.hpp"

namespace element {

/** Element: combined-mode native JACK MIDI output sink node.
 *
 *  Functional equivalent of the legacy ALSA-seq "MIDI Output Device"
 *  node for users who want a single MIDI sink broadcast to every
 *  enabled outbound JACK MIDI port.  At processBlock time it iterates
 *  the input MIDI buffer once and writes each event (with the
 *  original sample offset) to every JACK output port via the same
 *  direct-to-libjack path used by JackMidiOutputNode — disabled ports
 *  drop cleanly inside writeJackMidiOutput.  Per-port routing is
 *  still available via the JackMidiOutputNode type. */
class JackMidiOutputAllNode : public BaseProcessor
{
public:
    explicit JackMidiOutputAllNode (DeviceManager& dm)
        : devices (dm)
    {
        setPlayConfigDetails (0, 0, 44100.0, 512);
    }

    ~JackMidiOutputAllNode() override = default;

    inline const juce::String getName() const override
    {
        return juce::String ("JACK MIDI Output (All)");
    }

    inline void fillInPluginDescription (juce::PluginDescription& desc) const override
    {
        desc.name              = "JACK MIDI Output (All)";
        desc.fileOrIdentifier  = EL_NODE_ID_JACK_MIDI_OUTPUT_ALL;
        desc.uniqueId          = EL_NODE_UID_JACK_MIDI_OUTPUT_ALL;
        desc.descriptiveName   = "JACK MIDI broadcast to all output ports";
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
        if (midiMessages.isEmpty())
            return;

        auto* dev = devices.getCurrentAudioDevice();
        auto* sink = dynamic_cast<JackMidiOutputSink*> (dev);
        if (sink == nullptr)
        {
            midiMessages.clear();
            return;
        }

        const int numPorts = sink->getNumJackMidiOutputPorts();
        if (numPorts <= 0)
        {
            midiMessages.clear();
            return;
        }

        for (const auto m : midiMessages)
        {
            const auto& msg = m.getMessage();
            const auto* raw = msg.getRawData();
            const int size = msg.getRawDataSize();
            if (raw == nullptr || size <= 0)
                continue;
            for (int p = 0; p < numPorts; ++p)
                sink->writeJackMidiOutput (p, m.samplePosition, raw, size);
        }

        midiMessages.clear();
    }

    inline double getTailLengthSeconds() const override { return 0.0; }
    inline bool acceptsMidi()  const override { return true; }
    inline bool producesMidi() const override { return false; }
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
    DeviceManager& devices;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JackMidiOutputAllNode)
};

} // namespace element

#endif // ELEMENT_USE_JACK
