// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/ui/style.hpp>
#include <element/context.hpp>
#include <element/devices.hpp>
#include <element/settings.hpp>

#if ELEMENT_USE_JACK
#include "engine/jack.hpp"
#endif

#include "ui/viewhelpers.hpp"

namespace element {

/** Element: info panel rendered in the node Properties view (and the
 *  Editor view) for the legacy `MIDI In` / `MIDI Out` Graph I/O
 *  pseudo-nodes.  Upstream's stub just told the user to "see
 *  preferences..." which is useless — these pseudo-nodes have no
 *  device picker; they carry the graph's in-band MIDI bus (virtual
 *  keyboard input, generated MIDI clock routed to input, plugin-mode
 *  host MIDI pass-through).  This panel explains that and surfaces
 *  the JACK MIDI port count + a pointer to the JACK MIDI graph
 *  nodes that handle external MIDI on this build. */
class MidiIONodeInfoPanel : public juce::Component,
                            public juce::ChangeListener
{
public:
    explicit MidiIONodeInfoPanel (bool isInput)
        : showInput (isInput)
    {
        heading.setFont (juce::Font (juce::FontOptions (13.0f).withStyle ("Bold")));
        heading.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (heading);

        body.setFont (juce::Font (juce::FontOptions (12.0f)));
        body.setJustificationType (juce::Justification::topLeft);
        body.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (body);

        countLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
        countLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (countLabel);

        updateContent();
    }

    ~MidiIONodeInfoPanel() override
    {
        if (auto* world = ViewHelpers::getGlobals (this))
            world->settings().removeChangeListener (this);
    }

    void parentHierarchyChanged() override
    {
        if (! listenerRegistered)
        {
            if (auto* world = ViewHelpers::getGlobals (this))
            {
                world->settings().addChangeListener (this);
                listenerRegistered = true;
                updateContent();
            }
        }
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        updateContent();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colors::backgroundColor);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 6);
        heading.setBounds (r.removeFromTop (22));
        r.removeFromTop (6);
        body.setBounds (r.removeFromTop (96));
        r.removeFromTop (6);
        countLabel.setBounds (r.removeFromTop (22));
    }

private:
    void updateContent()
    {
        heading.setText (showInput ? "Graph MIDI Input" : "Graph MIDI Output",
                         juce::dontSendNotification);

        juce::String text;
#if ELEMENT_USE_JACK
        text << (showInput
            ? "Carries the in-band MIDI feed for this graph — virtual "
              "keyboard input, generated MIDI clock routed to input, "
              "and (in plugin mode) the host's MIDI input.\n\n"
              "External MIDI from JACK is routed via the dedicated "
              "JACK MIDI Input / JACK MIDI Input (All) graph nodes, "
              "not through this pseudo-node."
            : "Receives MIDI from the graph (e.g. clock output, "
              "sequencer output) and dispatches it to JACK MIDI "
              "Output port 1 by default.\n\n"
              "For per-port routing, wire your MIDI source to a "
              "JACK MIDI Output / JACK MIDI Output (All) graph node "
              "instead.");

        if (auto* world = ViewHelpers::getGlobals (this))
        {
            auto& jack = world->devices().getJackClient();
            const int n = showInput ? jack.getNumMidiInputs() : jack.getNumMidiOutputs();
            juce::String c;
            c << "JACK MIDI " << (showInput ? "input" : "output") << " ports configured: " << n;
            countLabel.setText (c, juce::dontSendNotification);
        }
#else
        text << "Carries the in-band MIDI feed for this graph — virtual "
                "keyboard input, generated MIDI clock routed to input, "
                "and (in plugin mode) the host's MIDI input.\n\n"
                "External MIDI device routing is configured on the MIDI "
                "preferences page.";
        countLabel.setText ({}, juce::dontSendNotification);
#endif
        body.setText (text, juce::dontSendNotification);
    }

    bool showInput;
    bool listenerRegistered = false;
    juce::Label heading;
    juce::Label body;
    juce::Label countLabel;
};

} // namespace element
