// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/ui.hpp>
#include <element/processor.hpp>
#include <element/signals.hpp>

#include "ElementApp.h"
#include "ui/categorycolors.hpp"
#include "ui/channelstrip.hpp"
#include "ui/fontcache.hpp"
#include "services/sessionservice.hpp"

namespace element {

class NodeChannelStripComponent : public Component,
                                  public Timer,
                                  public ComboBox::Listener,
                                  private Value::Listener
{
public:
    std::function<void()> onNodeChanged;
    NodeChannelStripComponent (GuiService& g, bool handleNodeSelected = true)
        : gui (g), listenForNodeSelected (handleNodeSelected)
    {
        addAndMakeVisible (channelStrip);
        addAndMakeVisible (nodeName);
        nodeName.setText ("", dontSendNotification);
        nodeName.setJustificationType (Justification::centredBottom);
        nodeName.setEditable (false, true, false);
        /* Mono bold to match session view column headers + arrangement
         * view lane labels.  Colour is updated on setNode() to track
         * the node's type-tint so the name reads against the matching
         * top tint band painted in paint(). */
        nodeName.setFont (monoFont (11.0f, juce::Font::bold));
        nodeName.onTextChange = [this] {
            if (node.isValid())
                node.setProperty (tags::name, nodeName.getText());
        };

        addAndMakeVisible (channelBox);
        channelBox.setJustificationType (Justification::centred);

        addAndMakeVisible (flowBox);
        flowBox.setJustificationType (Justification::centred);

        bindSignals();
    }

    ~NodeChannelStripComponent()
    {
        unbindSignals();
    }

    ChannelStripComponent& getChannelStrip() { return channelStrip; }

    void setVolumeMinMax (double minDb, double maxDb, double skew = 2.0)
    {
        channelStrip.setMinMaxDecibels (minDb, maxDb);
        channelStrip.setFaderSkewFactor (skew);
    }

    void bindSignals()
    {
        unbindSignals();
        displayName.addListener (this);
        flowBox.addListener (this);
        if (listenForNodeSelected)
            nodeSelectedConnection = gui.nodeSelected.connect (
                std::bind (&NodeChannelStripComponent::nodeSelected, this));
        volumeChangedConnection = channelStrip.volumeChanged.connect (
            std::bind (&NodeChannelStripComponent::volumeChanged, this, std::placeholders::_1));
        powerChangedConnection = channelStrip.powerChanged.connect (
            std::bind (&NodeChannelStripComponent::powerChanged, this));
        muteChangedConnection = channelStrip.muteChanged.connect (
            std::bind (&NodeChannelStripComponent::muteChanged, this));
        volumeDoubleClickedConnection = channelStrip.volumeLabelDoubleClicked.connect (
            std::bind (&NodeChannelStripComponent::setUnityGain, this));
    }

    void unbindSignals()
    {
        for (auto& c : _conns)
            c.disconnect();
        _conns.clear();

        displayName.removeListener (this);
        flowBox.removeListener (this);
        nodeSelectedConnection.disconnect();
        volumeChangedConnection.disconnect();
        powerChangedConnection.disconnect();
        muteChangedConnection.disconnect();
        volumeDoubleClickedConnection.disconnect();
    }

    void resized() override
    {
        auto r (getLocalBounds());
        nodeName.setBounds (r.removeFromTop (22).reduced (2));
        r.removeFromTop (10); // padding between strip title and IO boxes

        auto r2 = r.removeFromBottom (jmin (268, r.getHeight()));
        int boxSize = r2.getWidth() - 8;
        flowBox.setBounds (r2.removeFromTop (16).withSizeKeepingCentre (boxSize, 14));
        channelBox.setBounds (r2.removeFromTop (16).withSizeKeepingCentre (boxSize, 14));
        channelStrip.setBounds (r2);
    }

    inline virtual void paint (Graphics& g) override
    {
        /* Visual style matches SessionView column headers +
         * ArrangementView lane labels + TrackerEditor track headers:
         *   - Dark gutter base (tracker / session shared colour)
         *   - Top tint band (full width) using colorForNode
         *   - Low-alpha tint wash across the body below the band
         *   - Hairline divider on the right edge between strips
         * Brings the graph mixer in line with the rest of the app's
         * column/row idiom -- nodes are tinted by category (Audio I/O,
         * MIDI, VST/VST3/CLAP/LV2/AU, Element internal) and the strip
         * carries that identity in its top band + wash. */
        const juce::Colour kGutterColour     { 0xff'14'14'14 };
        const juce::Colour kRowDividerColour { 0xff'22'22'22 };
        const juce::Colour tint = colorForNode (node);

        const auto bounds = getLocalBounds();
        constexpr int kTintBandH = 4;

        g.setColour (kGutterColour);
        g.fillRect (bounds);

        g.setColour (tint);
        g.fillRect (bounds.getX(), bounds.getY(),
                    bounds.getWidth() - 1, kTintBandH);

        g.setColour (tint.withAlpha (0.10f));
        g.fillRect (bounds.getX(), bounds.getY() + kTintBandH,
                    bounds.getWidth() - 1,
                    bounds.getHeight() - kTintBandH - 1);

        g.setColour (kRowDividerColour);
        g.drawLine ((float) getWidth() - 1.f, 0.0f,
                    (float) getWidth() - 1.f, (float) getHeight());
    }

    inline void timerCallback() override
    {
        /* Skip the per-tick meter poll + diff-gated setter sweep when
         * the strip itself isn't on screen.  Volume / power / mute are
         * already short-circuited internally (channelstrip.hpp setters
         * early-return on no-change), but getInputRMS / getOutputRMS +
         * meter.setValue still cross the audio side and walk every
         * SimpleMeterValue per tick.  Pattern matches meterbridge.cpp:28
         * + sessionview / diskopview / sampler editors per
         * feedback_gui_must_stay_fast. */
        if (! isShowing()) return;

        auto& meter = channelStrip.getSimpleMeter();
        if (ProcessorPtr ptr = node.getObject())
        {
            const int startChannel = jmax (0, channelBox.getSelectedId() - 1);
            if (ptr->getNumAudioOutputs() == 1)
            {
                if (isAudioOutNode)
                    for (int c = 0; c < 2; ++c)
                        meter.setValue (c, ptr->getInputRMS (startChannel));
                else
                    for (int c = 0; c < 2; ++c)
                        meter.setValue (c, ptr->getOutputRMS (startChannel));
            }
            else
            {
                const int endChannel = startChannel + 2;
                if (isAudioOutNode || isMonitoringInputs())
                    for (int c = startChannel; c < endChannel; ++c)
                        meter.setValue (c - startChannel, ptr->getInputRMS (c));
                else
                    for (int c = startChannel; c < endChannel; ++c)
                        meter.setValue (c - startChannel, ptr->getOutputRMS (c));
            }

            const auto cv = getCurrentVolume();
            if (static_cast<float> (channelStrip.getVolume()) != cv)
                channelStrip.setVolume (cv, dontSendNotification);

            /* setPower already self-gates (channelstrip.hpp:34
             * early-returns when state unchanged), but reading
             * isSuspended() once and comparing here avoids one extra
             * call site per tick and keeps the diff-gate pattern
             * consistent with the volume + mute branches above. */
            const bool curPower = ! ptr->isSuspended();
            if (channelStrip.isPowerOn() != curPower)
                channelStrip.setPower (curPower, false);

            if (channelStrip.isMuted() != ptr->isMuted())
                channelStrip.setMuted (ptr->isMuted(), false);
        }
        else
        {
            meter.resetPeaks();
            stopTimer();
        }

        meter.refresh();
    }

    inline void stabilizeContent()
    {
        updateComboBoxes();
        updateNodeName();
        updateChannelStrip();
    }

    inline void updateChannelStrip()
    {
        if (ProcessorPtr object = node.getObject())
        {
            SharedConnectionBlock b1 (volumeChangedConnection);
            SharedConnectionBlock b2 (powerChangedConnection);
            SharedConnectionBlock b3 (muteChangedConnection);

            channelStrip.setVolume (getCurrentVolume(), dontSendNotification);
            channelStrip.setPower (! object->isSuspended(), false);
            channelStrip.setMuted (object->isMuted(), false);

            b1.unblock();
            b2.unblock();
            b3.unblock();
        }
    }

    inline void setNodeNameEditable (const bool isEditable)
    {
        nodeName.setEditable (false, isEditable, false);
    }

    /* Element: override the channel-name label's text colour.  Used by
     * the graph mixer strip to push the name to pure white so it pops
     * against the type-tinted background. */
    inline void setNodeNameColour (Colour c)
    {
        nodeName.setColour (Label::textColourId, c);
    }

    inline void setNode (const Node& newNode)
    {
        stopTimer();
        node = newNode;
        isAudioOutNode = node.isAudioOutputNode();
        isAudioInNode = node.isAudioInputNode();
        audioIns.clearQuick();
        audioOuts.clearQuick();
        node.getPorts (audioIns, audioOuts, PortType::Audio);
        displayName.referTo (node.getPropertyAsValue (tags::name));
        stabilizeContent();
        startTimerHz (meterSpeedHz);

        /* Re-tint the name label to track the new node's type colour.
         * paint() draws the matching top band + wash from the same
         * colorForNode() source so name + band always agree. */
        nodeName.setColour (Label::textColourId, colorForNode (node));

        // Strip bg is type-dependent (see colorForNode) — a node
        // swap has to repaint the strip itself, not just refresh the
        // children that listen to model values.
        repaint();

        if (onNodeChanged)
            onNodeChanged();
    }

    inline Node getNode() const { return node; }

    inline void setComboBoxesVisible (bool showChannelBox = true, bool showFlowBox = true)
    {
        useChannelBox = showChannelBox;
        useFlowBox = showFlowBox;
        updateComboBoxes();
        resized();
    }

    /** @internal */
    inline void comboBoxChanged (ComboBox* box) override
    {
        if (box == &flowBox)
        {
            updateComboBoxes (false, true);
            updateChannelStrip();
        }
    }

    /** Called when the volume slider changes. If this is set, you probably
        also want to override getCurrentVolume() */
    std::function<void (double)> onVolumeChanged;

protected:
    /** Override this to return volume from the backend/model layer. The
        default returns either the input or output gain of the node */
    virtual float getCurrentVolume()
    {
        ProcessorPtr object = node.getObject();
        if (object == nullptr)
            return 0.f;

        float gain = isMonitoringInputs() || isAudioOutNode ? object->getInputGain() : object->getGain();
        return Decibels::gainToDecibels (gain, -60.f);
    }

private:
    friend class NodeChannelStripView;
    GuiService& gui;
    Label nodeName;
    Node node;
    PortArray audioIns, audioOuts;
    ComboBox channelBox, flowBox;
    ChannelStripComponent channelStrip;
    bool listenForNodeSelected;

    bool useFlowBox = true;
    bool useChannelBox = true;

    int meterSpeedHz = 15;
    bool isAudioOutNode = false;
    bool isAudioInNode = false;
    [[maybe_unused]] bool monoMeter = false;

    Value displayName;

    SignalConnection nodeSelectedConnection;
    SignalConnection volumeChangedConnection;
    SignalConnection powerChangedConnection;
    SignalConnection volumeDoubleClickedConnection;
    SignalConnection muteChangedConnection;
    std::vector<boost::signals2::connection> _conns;

    inline bool isMonitoringInputs() const { return flowBox.getSelectedId() == 1; }
    inline bool isMonitoringOutputs() const { return flowBox.getSelectedId() == 2; }

    void valueChanged (Value& value) override
    {
        if (value.refersToSameSourceAs (displayName))
            updateNodeName();
    }

    void updateNodeName()
    {
        if (node.isValid())
        {
            nodeName.setText (node.getDisplayName(), dontSendNotification);
            String tooltip = node.getDisplayName();
            if (node.hasModifiedName())
                tooltip << " (" << node.getPluginName() << ")";
            nodeName.setTooltip (tooltip);
        }
    }

    void updateComboBoxes (bool doFlowBox = true, bool doChannelBox = true)
    {
        if (doFlowBox)
        {
            int flowId = flowBox.getSelectedId();
            if (flowId <= 0)
                flowId = 2;
            flowBox.clear();
            flowBox.setTooltip ("Signal flow to monitor");

            if (audioIns.size() > 0)
                flowBox.addItem ("Input", 1);
            if (audioOuts.size() > 0)
                flowBox.addItem ("Output", 2);

            if (flowBox.getNumItems() > 0 && ! isAudioInNode && ! isAudioOutNode)
            {
                flowBox.setVisible (useFlowBox);
                flowBox.setSelectedId (flowId, juce::dontSendNotification);
                if (flowBox.getSelectedId() <= 0)
                    flowBox.setSelectedItemIndex (0);
            }
            else
            {
                flowBox.setVisible (false);
            }
        }

        if (doChannelBox)
        {
            channelBox.setTooltip ("Channel(s) to monitor");
            channelBox.clear();

            // audio out node ports flipped until more robust
            auto& ports = isAudioOutNode ? audioIns : isMonitoringInputs() ? audioIns
                                                                           : audioOuts;

            const bool monoPorts = ports.size() % 2 != 0;
            const int step = monoPorts ? 1 : 2;

            for (int i = 0; i < ports.size(); i += step)
            {
                String text (int (i + 1));
                if (! monoPorts)
                    text << " - " << int (i + 2);
                channelBox.addItem (text, i + 1);
            }

            if (channelBox.getNumItems() > 0)
            {
                channelBox.setVisible (useFlowBox);
                channelBox.setSelectedItemIndex (0);
            }
            else
            {
                channelBox.setVisible (false);
            }
        }
    }

    void nodeSelected()
    {
        setNode (gui.getSelectedNode());
    }

    void powerChanged()
    {
        if (node.isValid())
            node.setProperty (tags::bypass, ! channelStrip.isPowerOn());
        if (auto* obj = node.getObject())
            obj->suspendProcessing (! channelStrip.isPowerOn());
    }

    void volumeChanged (double value)
    {
        if (onVolumeChanged != nullptr)
            return onVolumeChanged (value);

        if (ProcessorPtr object = node.getObject())
        {
            auto gain = Decibels::decibelsToGain (value, -60.0);
            if (isAudioOutNode || isMonitoringInputs())
            {
                if (gain != (double) node.getProperty ("inputGain", gain) || gain != (double) object->getInputGain())
                {
                    node.setProperty ("inputGain", gain);
                    object->setInputGain (static_cast<float> (gain));
                }
            }
            else
            {
                if (gain != (double) node.getProperty ("gain", gain) || gain != (double) object->getGain())
                {
                    node.setProperty ("gain", gain);
                    object->setGain (static_cast<float> (gain));
                }
            }
        }
    }

    void muteChanged()
    {
        if (node.isValid())
            node.setProperty (tags::mute, channelStrip.isMuted());
        if (auto* obj = node.getObject())
            obj->setMuted (channelStrip.isMuted());
    }

    void setUnityGain()
    {
        channelStrip.setVolume (0.0);
    }
};

} // namespace element
