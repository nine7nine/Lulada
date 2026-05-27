// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/ui/nodeeditor.hpp>
#include <element/ui/style.hpp>

#include <element/context.hpp>
#include <element/devices.hpp>
#include <element/settings.hpp>

#include "engine/graphnode.hpp"
#include "ui/viewhelpers.hpp"

#if ELEMENT_USE_JACK
#include "engine/jack.hpp"
#endif

namespace element {

/** Element: in-place editor for the Audio In / Audio Out IONode pseudo-
 *  nodes.  Replaces upstream's empty "see preferences..." stub with a
 *  live channel-list view that reflects the JACK driver's currently-
 *  registered ports (e.g. main_in_1 .. main_in_8) and a port-count
 *  picker that writes the matching Settings key + restarts the audio
 *  device so the change takes effect immediately.  The same shape
 *  serves both Audio In and Audio Out — `showIns` / `showOuts` pick
 *  which side to render. */
class AudioIONodeEditor : public NodeEditor,
                          public juce::ChangeListener
{
public:
    AudioIONodeEditor (const Node& node, DeviceManager& devs, bool ins = true, bool outs = true)
        : NodeEditor (node), devices (devs), showIns (ins), showOuts (outs)
    {
        content.reset (new Content (*this));
        view.setViewedComponent (content.get(), false);
        view.setScrollBarsShown (true, false);
        addAndMakeVisible (view);
        devices.addChangeListener (this);
    }

    ~AudioIONodeEditor()
    {
        devices.removeChangeListener (this);
        view.setViewedComponent (nullptr, false);
        content.reset();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::backgroundColor);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (content != nullptr)
            content->updateDevices();
    }

    void resized() override
    {
        view.setBounds (getLocalBounds());
        if (content != nullptr)
            content->setSize (view.getWidth() - view.getScrollBarThickness(),
                              content->computeHeight());
    }

private:
    DeviceManager& devices;
    bool showIns = true;
    bool showOuts = true;
    juce::Viewport view;

    struct Content : public juce::Component,
                     public juce::ChangeListener
    {
        explicit Content (AudioIONodeEditor& ed)
            : owner (ed)
        {
            heading.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
            heading.setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (heading);

            hint.setFont (juce::Font (juce::FontOptions (11.0f).withStyle ("Italic")));
            hint.setJustificationType (juce::Justification::centredLeft);
            hint.setMinimumHorizontalScale (1.0f);
            addAndMakeVisible (hint);

            portCountLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
            portCountLabel.setText ("Port count", juce::dontSendNotification);
            portCountLabel.setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (portCountLabel);

            const juce::StringArray choices { "Auto (mirror hardware)", "2", "4", "8", "16", "32" };
            portValues = { 0, 2, 4, 8, 16, 32 };
            for (int i = 0; i < choices.size(); ++i)
                portCountCombo.addItem (choices[i], i + 1);
            portCountCombo.onChange = [this] { applyPortCount(); };
            addAndMakeVisible (portCountCombo);

            channelsHeader.setFont (juce::Font (juce::FontOptions (12.0f).withStyle ("Bold")));
            channelsHeader.setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (channelsHeader);

            updateDevices();

            if (auto* world = ViewHelpers::getGlobals (this))
                world->settings().addChangeListener (this);
        }

        ~Content() override
        {
            if (auto* world = ViewHelpers::getGlobals (this))
                world->settings().removeChangeListener (this);
        }

        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            updateDevices();
        }

        int computeHeight()
        {
            const int headerH    = 22;
            const int rowH       = 22;
            const int spacing    = 6;
            const int hintH      = 32;
            const int channelsH  = (int) channelLabels.size() * (rowH + 2);
            return headerH                    /* heading           */
                 + spacing + hintH            /* hint              */
                 + spacing + rowH             /* port-count row    */
                 + spacing + headerH          /* channels header   */
                 + spacing + channelsH        /* per-channel list  */
                 + spacing;                   /* bottom margin     */
        }

        void updateDevices()
        {
            const bool isInput = owner.showIns;

            /* Detect whether this IO pseudo-node is on the ROOT graph
             * (proxying the host audio device -- the legacy use case)
             * or inside a SUB-GRAPH (proxying that subgraph's outer
             * port face -- e.g. the Multi-Track per-track direct-out
             * channels).  The two cases have completely different
             * semantics:
             *
             *   ROOT graph IO  -> channels mirror the JACK / hardware
             *                     device; channel count is user-config
             *                     via the JACK port-count combo +
             *                     persists to the audio preferences.
             *   SUB-GRAPH IO   -> channels mirror that subgraph's
             *                     outer port count; channel count is
             *                     auto-managed by whatever owns the
             *                     subgraph (e.g. ArrangementTracksService
             *                     resizes the Multi-Track to 2N channels
             *                     for N audio clips).  Editing host
             *                     device settings here is wrong + made
             *                     the panel useless for inspecting the
             *                     per-track direct-out routing the
             *                     Multi-Track exposes. */
            const Node parent = owner.getNode().getParentGraph();
            const bool onRootGraph = parent.isRootGraph();

            heading.setText (isInput
                                ? (onRootGraph ? "Host Audio Input"  : "Sub-graph Audio Input")
                                : (onRootGraph ? "Host Audio Output" : "Sub-graph Audio Output"),
                             juce::dontSendNotification);
            channelsHeader.setText (isInput ? "Input Channels" : "Output Channels",
                                    juce::dontSendNotification);

            channelLabels.clear (true);
            juce::StringArray names;

            if (onRootGraph)
            {
#if ELEMENT_USE_JACK
                hint.setText ("Port count follows the JACK setting in Audio preferences. "
                              "Changes here apply immediately and persist to the same setting.",
                              juce::dontSendNotification);
#else
                hint.setText ("Channel count follows the active audio device's enabled channels.",
                              juce::dontSendNotification);
#endif
                if (auto* device = owner.devices.getCurrentAudioDevice())
                    names = isInput ? device->getInputChannelNames() : device->getOutputChannelNames();
                if (names.isEmpty())
                    names.add ("(no audio device active)");
            }
            else
            {
                hint.setText ("Port count is managed by the parent sub-graph -- typically grows "
                              "as content is added (e.g. Multi-Track per-track direct-outs).",
                              juce::dontSendNotification);

                /* Pull the channel count directly off the IO node's
                 * own port list.  The parent sub-graph's outer port
                 * count drives IONode::refreshPorts which populates
                 * the IO node's ports.  For audio.input the IO node
                 * exposes one OUTPUT pin per outer-input channel
                 * (and vice-versa for audio.output) -- so we count
                 * whichever direction maps to the user-facing
                 * channel count.  Node::getNumPorts() has no
                 * type-filter overload; iterate + filter. */
                const Node thisNode = owner.getNode();
                int audioPorts = 0;
                for (int i = 0; i < thisNode.getNumPorts(); ++i)
                {
                    const Port p = thisNode.getPort (i);
                    if (p.getType() == PortType::Audio
                        && p.isInput() == ! isInput)
                        ++audioPorts;
                }
                for (int ch = 0; ch < audioPorts; ++ch)
                {
                    names.add ((isInput ? juce::String ("In ")
                                         : juce::String ("Out "))
                                  + juce::String (ch + 1));
                }
                if (names.isEmpty())
                    names.add ("(no channels)");
            }

            for (const auto& n : names)
            {
                auto* lbl = channelLabels.add (new juce::Label ({}, n));
                lbl->setFont (juce::Font (juce::FontOptions (12.0f)));
                lbl->setJustificationType (juce::Justification::centredLeft);
                addAndMakeVisible (lbl);
            }

#if ELEMENT_USE_JACK
            if (onRootGraph)
            {
                if (auto* world = ViewHelpers::getGlobals (this))
                {
                    const auto key = isInput ? Settings::audioJackInputPortCountKey
                                             : Settings::audioJackOutputPortCountKey;
                    const int v = world->settings().getUserSettings()->getIntValue (key, 0);
                    portCountCombo.setSelectedId (selectedIdForValue (v), juce::dontSendNotification);
                    portCountCombo.setEnabled (true);
                    portCountLabel.setVisible (true);
                    portCountCombo.setVisible (true);
                }
            }
            else
            {
                /* Sub-graph IO: hide the JACK port-count picker --
                 * the channel count isn't user-editable from this
                 * panel.  The owning service (ArrangementTracksService
                 * for the Multi-Track) manages it. */
                portCountCombo.setEnabled (false);
                portCountLabel.setVisible (false);
                portCountCombo.setVisible (false);
            }
#else
            portCountCombo.setEnabled (false);
            portCountLabel.setVisible (false);
            portCountCombo.setVisible (false);
#endif

            setSize (getWidth(), computeHeight());
            resized();
            repaint();
        }

        void resized() override
        {
            const int spacing = 6;
            const int headerH = 22;
            const int rowH    = 22;
            auto r = getLocalBounds().reduced (8, 4);

            heading.setBounds (r.removeFromTop (headerH));
            r.removeFromTop (spacing);
            hint.setBounds (r.removeFromTop (32));
            r.removeFromTop (spacing);

            auto row = r.removeFromTop (rowH);
            portCountLabel.setBounds (row.removeFromLeft (juce::jmax (90, getWidth() / 3)));
            row.removeFromLeft (4);
            portCountCombo.setBounds (row.removeFromLeft (juce::jmin (row.getWidth(), 200)));
            r.removeFromTop (spacing);

            channelsHeader.setBounds (r.removeFromTop (headerH));
            r.removeFromTop (spacing);

            for (auto* lbl : channelLabels)
                lbl->setBounds (r.removeFromTop (rowH + 2).reduced (4, 1));
        }

    private:
        int selectedIdForValue (int v) const
        {
            for (int i = 0; i < portValues.size(); ++i)
                if (portValues[i] == v) return i + 1;
            return 1; /* Auto */
        }

        int valueForSelectedId (int id) const
        {
            const int idx = id - 1;
            return idx >= 0 && idx < portValues.size() ? portValues[idx] : 0;
        }

        void applyPortCount()
        {
#if ELEMENT_USE_JACK
            auto* world = ViewHelpers::getGlobals (this);
            if (world == nullptr)
                return;

            const int value = valueForSelectedId (portCountCombo.getSelectedId());
            const auto key  = owner.showIns ? Settings::audioJackInputPortCountKey
                                            : Settings::audioJackOutputPortCountKey;
            world->settings().getUserSettings()->setValue (key, value);

            auto& devs = world->devices();
            devs.applyJackPortCountsFromSettings (world->settings());

            juce::AudioDeviceManager::AudioDeviceSetup current;
            devs.getAudioDeviceSetup (current);
            if (value > 0)
            {
                if (owner.showIns)
                {
                    current.inputChannels.clear();
                    current.inputChannels.setRange (0, value, true);
                    current.useDefaultInputChannels = false;
                }
                else
                {
                    current.outputChannels.clear();
                    current.outputChannels.setRange (0, value, true);
                    current.useDefaultOutputChannels = false;
                }
            }
            devs.setAudioDeviceSetup (current, true);

            /* Also resize the parent graph's abstract audio port count
             * so the Audio In/Out IONode (and the block on the canvas)
             * matches the new device channel count.  Without this the
             * device reopens with N channels but the graph still
             * exposes the old count to its IONodeEnforcer-managed
             * pseudo-node — the block on the canvas would stay at the
             * pre-change number of dots. */
            const int graphPortCount = value > 0 ? value : 2;
            auto parentGraph = owner.getNode().getParentGraph();
            if (auto* gn = dynamic_cast<GraphNode*> (parentGraph.getObject()))
                gn->setNumPorts (PortType::Audio, graphPortCount, owner.showIns, true);
#endif
        }

        AudioIONodeEditor& owner;
        juce::Label heading;
        juce::Label hint;
        juce::Label portCountLabel;
        juce::ComboBox portCountCombo;
        juce::Array<int> portValues;
        juce::Label channelsHeader;
        juce::OwnedArray<juce::Label> channelLabels;
    };

    friend struct Content;
    std::unique_ptr<Content> content;
};

} // namespace element
