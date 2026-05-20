// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if ELEMENT_USE_JACK

#include <element/devices.hpp>
#include <element/node.h>

#include "nodes/baseprocessor.hpp"
#include "engine/jack.hpp"

namespace element {

/** Element: native JACK MIDI output sink node.
 *
 *  Each instance is bound to a single JACK MIDI output port
 *  (element:midi_out_<portIndex+1>).  At processBlock time it iterates
 *  the input MIDI buffer and writes each event directly to libjack via
 *  the JackMidiOutputSink with the event's original sample offset
 *  preserved — zero added latency, sample-accurate output scheduling.
 *
 *  The graph render runs synchronously inside the JACK process
 *  callback (JUCE's audio callback IS the JACK process callback on
 *  this device type), so the output port buffer cached by
 *  JackAudioIODevice::process() at the top of the period is live and
 *  writable at exactly this point.  There is no ringbuffer round-trip
 *  and no one-period delay.
 *
 *  Multiple instances can coexist with different port indices, giving
 *  the user per-destination routing (drum machine track →
 *  midi_out_1 → hardware synth A; bass track → midi_out_2 → hardware
 *  synth B).  The JackMidiOutputSink pointer is resolved on every
 *  processBlock via dynamic_cast against the current AudioIODevice —
 *  same one-vtable-comparison cost as the input side. */
class JackMidiOutputNode : public BaseProcessor
{
public:
    explicit JackMidiOutputNode (DeviceManager& dm)
        : devices (dm)
    {
        setPlayConfigDetails (0, 0, 44100.0, 512);
    }

    ~JackMidiOutputNode() override = default;

    int  getPortIndex() const noexcept { return portIndex; }
    void setPortIndex (int p) noexcept
    {
        portIndex = juce::jlimit (0, 31, p);
    }

    inline const juce::String getName() const override
    {
        return juce::String ("JACK MIDI Out ") + juce::String (portIndex + 1);
    }

    inline void fillInPluginDescription (juce::PluginDescription& desc) const override
    {
        desc.name              = "JACK MIDI Output";
        desc.fileOrIdentifier  = EL_NODE_ID_JACK_MIDI_OUTPUT;
        desc.uniqueId          = EL_NODE_UID_JACK_MIDI_OUTPUT;
        desc.descriptiveName   = "JACK MIDI output port";
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
        if (sink == nullptr || portIndex >= sink->getNumJackMidiOutputPorts())
        {
            midiMessages.clear();
            return;
        }

        /* Iterate with sample positions preserved — JackAudioIODevice
         * has already cached + cleared the libjack buffer for this
         * period, so each write lands at the event's original frame
         * offset within the current numSamples. */
        for (const auto m : midiMessages)
        {
            const auto& msg = m.getMessage();
            const auto* raw = msg.getRawData();
            const int size = msg.getRawDataSize();
            if (raw != nullptr && size > 0)
                sink->writeJackMidiOutput (portIndex, m.samplePosition, raw, size);
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

    inline void getStateInformation (juce::MemoryBlock& destData) override
    {
        juce::ValueTree vt ("JackMidiOutputNode");
        vt.setProperty ("port", portIndex, nullptr);
        if (auto xml = vt.createXml())
            copyXmlToBinary (*xml, destData);
    }

    inline void setStateInformation (const void* data, int sizeInBytes) override
    {
        if (auto xml = getXmlFromBinary (data, sizeInBytes))
        {
            const auto vt = juce::ValueTree::fromXml (*xml);
            if (vt.hasType ("JackMidiOutputNode"))
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JackMidiOutputNode)
};

} // namespace element

#endif // ELEMENT_USE_JACK
