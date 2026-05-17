// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if ELEMENT_USE_JACK

#include <element/devices.hpp>
#include <element/node.h>

#include "nodes/baseprocessor.hpp"
#include "engine/jack.hpp"

namespace element {

/** Element-NSPA: native JACK MIDI input source node.
 *
 *  Each instance is bound to a single JACK MIDI input port
 *  (element:midi_in_<portIndex+1>).  At processBlock time it copies
 *  that port's per-period MidiBuffer — populated sample-accurately by
 *  JackAudioIODevice's RT drain — into the node's output MIDI port.
 *  Multiple instances can coexist with different port indices, giving
 *  the user per-source routing (e.g. controller A → midi_in_1 → synth
 *  A; controller B → midi_in_2 → synth B).
 *
 *  The JackMidiInputProvider pointer is resolved on every processBlock
 *  via dynamic_cast against the current AudioIODevice.  This is a
 *  single vtable comparison (~ns) on the RT thread, and gracefully
 *  produces silence when no JACK device is active. */
class JackMidiInputNode : public BaseProcessor
{
public:
    explicit JackMidiInputNode (DeviceManager& dm)
        : devices (dm)
    {
        setPlayConfigDetails (0, 0, 44100.0, 512);
    }

    ~JackMidiInputNode() override = default;

    int  getPortIndex() const noexcept { return portIndex; }
    void setPortIndex (int p) noexcept
    {
        portIndex = juce::jlimit (0, 31, p);
    }

    inline const juce::String getName() const override
    {
        return juce::String ("JACK MIDI In ") + juce::String (portIndex + 1);
    }

    inline void fillInPluginDescription (juce::PluginDescription& desc) const override
    {
        desc.name              = "JACK MIDI Input";
        desc.fileOrIdentifier  = EL_NODE_ID_JACK_MIDI_INPUT;
        desc.uniqueId          = EL_NODE_UID_JACK_MIDI_INPUT;
        desc.descriptiveName   = "Sample-accurate JACK MIDI input port";
        desc.numInputChannels  = 0;
        desc.numOutputChannels = 0;
        desc.hasSharedContainer = false;
        desc.isInstrument      = false;
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

        auto* dev = devices.getCurrentAudioDevice();
        auto* provider = dynamic_cast<JackMidiInputProvider*> (dev);
        if (provider == nullptr)
            return;

        if (portIndex >= provider->getNumJackMidiInputPorts())
            return;

        const auto& src = provider->getCurrentJackMidiInputForPort (portIndex);
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

    inline void getStateInformation (juce::MemoryBlock& destData) override
    {
        juce::ValueTree vt ("JackMidiInputNode");
        vt.setProperty ("port", portIndex, nullptr);
        if (auto xml = vt.createXml())
            copyXmlToBinary (*xml, destData);
    }

    inline void setStateInformation (const void* data, int sizeInBytes) override
    {
        if (auto xml = getXmlFromBinary (data, sizeInBytes))
        {
            const auto vt = juce::ValueTree::fromXml (*xml);
            if (vt.hasType ("JackMidiInputNode"))
                portIndex = juce::jlimit (0, 31, (int) vt.getProperty ("port", 0));
        }
    }

protected:
    inline bool isBusesLayoutSupported (const BusesLayout&) const override { return false; }
    inline bool canApplyBusesLayout (const BusesLayout& layouts) const override
    {
        return isBusesLayoutSupported (layouts);
    }

private:
    DeviceManager& devices;
    int portIndex = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JackMidiInputNode)
};

} // namespace element

#endif // ELEMENT_USE_JACK
