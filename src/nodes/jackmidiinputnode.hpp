// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if ELEMENT_USE_JACK

#include <atomic>

#include <element/context.hpp>
#include <element/devices.hpp>
#include <element/engine.hpp>
#include <element/node.h>
#include <element/services.hpp>

#include "nodes/baseprocessor.hpp"
#include "engine/jack.hpp"

namespace element {

/** Element: native JACK MIDI input source node.
 *
 *  Each instance is bound to a single JACK MIDI input port
 *  (element:midi_in_<portIndex+1>).  At processBlock time it copies
 *  that port's per-period MidiBuffer — populated sample-accurately by
 *  JackAudioIODevice's RT drain — into the node's output MIDI port.
 *  Multiple instances can coexist with different port indices, giving
 *  the user per-source routing (e.g. controller A → midi_in_1 → synth
 *  A; controller B → midi_in_2 → synth B).
 *
 *  ─── Program-change translation ─────────────────────────────────────
 *
 *  Some VST plugins ignore inbound MIDI Program Change messages but
 *  do honour the VST setProgram dispatcher call.  When
 *  translateProgramChange is enabled, this node detects 0xCx events
 *  on the RT thread, drops them from the forwarded MIDI stream, and
 *  schedules an async dispatch to every plugin reachable downstream
 *  via the graph's MIDI connections.  EngineService walks the graph
 *  on the message thread and invokes setCurrentProgram on each match,
 *  matching the workaround pattern from fsthost (jfst/process.c
 *  MIDI_PC_SELF mode) but per-input rather than per-host-instance. */
class JackMidiInputNode : public BaseProcessor,
                          private juce::AsyncUpdater
{
public:
    explicit JackMidiInputNode (Context& ctx)
        : context (ctx)
    {
        setPlayConfigDetails (0, 0, 44100.0, 512);
    }

    ~JackMidiInputNode() override
    {
        cancelPendingUpdate();
    }

    int  getPortIndex() const noexcept { return portIndex; }
    void setPortIndex (int p) noexcept
    {
        portIndex = juce::jlimit (0, 31, p);
    }

    bool getTranslateProgramChange() const noexcept
    {
        return translateProgramChange.load (std::memory_order_relaxed);
    }

    void setTranslateProgramChange (bool on) noexcept
    {
        translateProgramChange.store (on, std::memory_order_relaxed);
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

        if (portIndex >= provider->getNumJackMidiInputPorts())
            return;

        const auto& src = provider->getCurrentJackMidiInputForPort (portIndex);
        if (src.isEmpty())
            return;

        /* Forward every event — including Program Changes — to
         * downstream plugins.  Many VSTs (especially older VST3 with
         * their own preset systems) only switch programs in response
         * to inbound MIDI PC; dropping it would defeat the common
         * case.  Toggle adds the setCurrentProgram dispatch as a
         * best-effort *backup* for plugins that DON'T honour inbound
         * PC but DO honour the VST setProgram call. */
        midiMessages.addEvents (src, 0, -1, 0);

        if (! translateProgramChange.load (std::memory_order_relaxed))
            return;

        /* Backup dispatch: scan for PC, queue the latest program for
         * the message-thread setCurrentProgram broadcast.  PCs arrive
         * at human rates (knob/button press), so last-write-wins via
         * std::atomic<int> is fine — no SPSC queue needed. */
        bool sawPC = false;
        for (const auto m : src)
        {
            const auto& msg = m.getMessage();
            if (msg.isProgramChange())
            {
                pendingProgram.store (msg.getProgramChangeNumber(), std::memory_order_relaxed);
                sawPC = true;
            }
        }
        if (sawPC)
            triggerAsyncUpdate(); /* lock-free, RT-safe */
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

    inline juce::AudioProcessorEditor* createEditor() override;
    inline bool hasEditor() const override { return true; }

    inline void getStateInformation (juce::MemoryBlock& destData) override
    {
        juce::ValueTree vt ("JackMidiInputNode");
        vt.setProperty ("port", portIndex, nullptr);
        vt.setProperty ("translateProgramChange",
                        translateProgramChange.load (std::memory_order_relaxed),
                        nullptr);
        if (auto xml = vt.createXml())
            copyXmlToBinary (*xml, destData);
    }

    inline void setStateInformation (const void* data, int sizeInBytes) override
    {
        if (auto xml = getXmlFromBinary (data, sizeInBytes))
        {
            const auto vt = juce::ValueTree::fromXml (*xml);
            if (vt.hasType ("JackMidiInputNode"))
            {
                portIndex = juce::jlimit (0, 31, (int) vt.getProperty ("port", 0));
                translateProgramChange.store (
                    (bool) vt.getProperty ("translateProgramChange", false),
                    std::memory_order_relaxed);
            }
        }
    }

protected:
    inline bool isBusesLayoutSupported (const BusesLayout&) const override { return false; }
    inline bool canApplyBusesLayout (const BusesLayout& layouts) const override
    {
        return isBusesLayoutSupported (layouts);
    }

private:
    /** Message-thread drain.  Pulls the last pending program number
        (-1 sentinel if none) and asks EngineService to walk the graph
        downstream from this node, invoking setCurrentProgram on every
        plugin reached.  setCurrentProgram may allocate / reload
        samples in the plugin, which is why this is off the RT path. */
    void handleAsyncUpdate() override
    {
        const int program = pendingProgram.exchange (-1, std::memory_order_relaxed);
        if (program < 0)
            return;
        if (auto* engineSvc = context.services().find<EngineService>())
            engineSvc->dispatchProgramChangeFromNode (this, program);
    }

    Context& context;
    int portIndex = 0;
    std::atomic<bool> translateProgramChange { false };
    std::atomic<int>  pendingProgram { -1 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JackMidiInputNode)
};

/** Minimal editor exposing the per-input PC-translate toggle.  Opens
    via the block's gear button.  Kept tiny on purpose — the only
    knob this node has worth a UI affordance is the PC translation
    workaround; port binding is fixed at creation and visible in the
    node's name. */
class JackMidiInputNodeEditor : public juce::AudioProcessorEditor
{
public:
    explicit JackMidiInputNodeEditor (JackMidiInputNode& proc)
        : juce::AudioProcessorEditor (proc), owner (proc)
    {
        setOpaque (true);

        portLabel.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
        portLabel.setText (owner.getName(), juce::dontSendNotification);
        portLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (portLabel);

        translateLabel.setFont (juce::Font (juce::FontOptions (11.0f)));
        translateLabel.setText ("Also call setProgram on downstream plugins",
                                juce::dontSendNotification);
        translateLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (translateLabel);

        translateButton.setButtonText ("");
        translateButton.setToggleState (owner.getTranslateProgramChange(),
                                        juce::dontSendNotification);
        translateButton.onClick = [this] {
            owner.setTranslateProgramChange (translateButton.getToggleState());
        };
        addAndMakeVisible (translateButton);

        hint.setFont (juce::Font (juce::FontOptions (10.5f).withStyle ("Italic")));
        hint.setText ("Program Change events are always forwarded to plugins.  This "
                      "toggle ADDITIONALLY invokes setCurrentProgram on every plugin "
                      "downstream of this node — a safety net for plugins that ignore "
                      "inbound MIDI PC but honour the VST setProgram dispatcher.  "
                      "Has no effect on plugins exposing fewer than 2 programs.",
                      juce::dontSendNotification);
        hint.setJustificationType (juce::Justification::topLeft);
        hint.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (hint);

        setSize (420, 180);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (findColour (juce::ResizableWindow::backgroundColourId));
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        portLabel.setBounds (r.removeFromTop (22));
        r.removeFromTop (6);

        auto row = r.removeFromTop (24);
        translateButton.setBounds (row.removeFromLeft (44));
        row.removeFromLeft (8);
        translateLabel.setBounds (row);

        r.removeFromTop (6);
        hint.setBounds (r);
    }

private:
    JackMidiInputNode& owner;
    juce::Label portLabel;
    juce::Label translateLabel;
    juce::ToggleButton translateButton;
    juce::Label hint;
};

inline juce::AudioProcessorEditor* JackMidiInputNode::createEditor()
{
    return new JackMidiInputNodeEditor (*this);
}

} // namespace element

#endif // ELEMENT_USE_JACK
