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
#include "ui/viewhelpers.hpp"
#include "services/sessionservice.hpp"

namespace element {

/* Flat session-style toggle button.  juce::TextButton routes through
 * LookAndFeel which adds rounded corners + saturation/alpha
 * adjustments + gradient shading -- not the flat fillRect + drawRect
 * idiom that SessionView paints directly for its MUTE / SOLO
 * buttons.  This class paints exactly that: solid fill + 1-px outline
 * + centered mono-bold label.  Caller sets onBg / offBg / onFg /
 * offFg via the public fields and triggers repaint(); the button's
 * toggle state determines which pair fires. */
class FlatTintedButton : public juce::Button
{
public:
    explicit FlatTintedButton (const juce::String& label)
        : juce::Button (label) {}

    juce::Colour onBg  { 0xff'40'40'40 };
    juce::Colour offBg { 0xff'20'20'20 };
    juce::Colour onFg  { juce::Colours::white };
    juce::Colour offFg { juce::Colours::white.withAlpha (0.70f) };
    juce::Colour border { 0xff'22'22'22 };

    void paintButton (juce::Graphics& g, bool isOver, bool isDown) override
    {
        const auto rect = getLocalBounds();
        const bool on = getToggleState();
        auto bg = on ? onBg : offBg;
        if (isDown)        bg = bg.brighter (0.10f);
        else if (isOver)   bg = bg.brighter (0.06f);

        g.setColour (bg);
        g.fillRect (rect);
        g.setColour (border);
        g.drawRect (rect, 1);
        g.setColour (on ? onFg : offFg);
        g.setFont (monoFont (9.0f, juce::Font::bold));
        g.drawText (getButtonText(), rect, juce::Justification::centred);
    }
};

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
        /* Hide the inner ChannelStripComponent's own M button -- the
         * MUTE pill at the top of the strip (added below) is the
         * promoted control.  Same model state via channelStrip.setMuted
         * / channelStrip.isMuted; both buttons would otherwise drive
         * the same flag from two visually-disjoint places. */
        channelStrip.setMuteButtonVisible (false);

        /* No Label sub-component for the name -- session view paints
         * its column names directly via g.drawText (no juce::Label
         * intermediary), and a Label here was preventing the tint
         * band from being visible (transparent-bg defaults notwith-
         * standing, some paint-order edge case under the GL renderer
         * left the band hidden).  Drawing directly mirrors session's
         * approach exactly.  Editable-rename via double-click was the
         * Label feature we drop here; rename now goes through node
         * inspector / right-click property edit (consistent with how
         * session columns work). */

        /* MUTE + SOLO pills side-by-side below the name band.
         * Matches SessionView column header MUTE / SOLO idiom: tint-
         * derived backgrounds when off, signature colours when on
         * (amber-red for MUTE, mustard-yellow for SOLO), full-word
         * labels.  MUTE drives channelStrip.setMuted() so the
         * existing muteChanged signal chain (engine bypass etc.)
         * fires unchanged.  SOLO writes a tags::solo property and
         * fires a callback that consumers (graph host) can hook for
         * solo bus logic when added. */
        addAndMakeVisible (muteButton_);
        muteButton_.setButtonText ("MUTE");
        muteButton_.setClickingTogglesState (true);
        muteButton_.onClick = [this] {
            channelStrip.setMuted (muteButton_.getToggleState(), true);
        };

        addAndMakeVisible (soloButton_);
        soloButton_.setButtonText ("SOLO");
        soloButton_.setClickingTogglesState (true);
        soloButton_.onClick = [this] {
            /* SOLO is visual-only for now (no engine solo bus
             * support); onSoloChanged fires for callers that want
             * to hook in their own routing.  Persisted via the
             * "soloed" Identifier on the node so session save / load
             * round-trips the user's choice. */
            const bool soloed = soloButton_.getToggleState();
            if (node.isValid())
                node.setProperty (juce::Identifier ("soloed"), soloed);
            if (onSoloChanged)
                onSoloChanged (soloed);
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
        /* Header layout matches SessionView column headers:
         *   - 22 px tint band carrying the name
         *   - 18 px MUTE / SOLO row (split horizontally 50/50)
         *   - then existing IO boxes + ChannelStripComponent below */
        constexpr int kNameBandH = 22;
        constexpr int kMuteRowH  = 18;
        constexpr int kHeaderPad = 4;

        auto r (getLocalBounds());
        r.removeFromTop (kNameBandH);  // name painted directly; no Label child here
        auto buttonRow = r.removeFromTop (kMuteRowH).reduced (4, 1);
        const int halfW = buttonRow.getWidth() / 2;
        muteButton_.setBounds (buttonRow.removeFromLeft (halfW - 1));
        buttonRow.removeFromLeft (2);
        soloButton_.setBounds (buttonRow);
        r.removeFromTop (kHeaderPad);

        auto r2 = r.removeFromBottom (jmin (268, r.getHeight()));
        int boxSize = r2.getWidth() - 8;
        flowBox.setBounds (r2.removeFromTop (16).withSizeKeepingCentre (boxSize, 14));
        channelBox.setBounds (r2.removeFromTop (16).withSizeKeepingCentre (boxSize, 14));
        channelStrip.setBounds (r2);
    }

    inline virtual void paint (Graphics& g) override
    {
        /* Style matches SessionView column headers + ArrangementView
         * lane labels + TrackerEditor track headers:
         *   - Dark gutter base for the strip body
         *   - Solid tint band wrapping the full name row at the top
         *     (the band IS the strip's visual identity, not just an
         *     accent stripe)
         *   - Tinted MUTE pill row below the name band (paint draws
         *     the row wash; muteButton_ overlays its own tinted pill)
         *   - Hairline right-edge divider between strips
         * Same idiom across the four primary views. */
        const juce::Colour kGutterColour     { 0xff'14'14'14 };
        const juce::Colour kRowDividerColour { 0xff'22'22'22 };
        /* Prefer the Arrangement Lane colour when the node is bound
         * to a lane (Bitwig / Ableton track-colour convention).  Falls
         * back to category / format colour when no lane binding exists
         * (graph-only nodes like the synth pseudo-nodes, plugins not
         * placed in an arrangement lane, etc.). */
        juce::ValueTree sessionRoot;
        if (auto sess = ViewHelpers::getSession (this))
            sessionRoot = sess->data();
        const juce::Colour tint = colorForNodeWithLane (node, sessionRoot);

        const auto bounds = getLocalBounds();
        constexpr int kNameBandH = 22;
        constexpr int kMuteRowH  = 18;

        g.setColour (kGutterColour);
        g.fillRect (bounds);

        /* Name band -- exact replica of SessionView column header:
         *   - top 4 px = softened tint strip
         *   - below = 0.10-alpha tint wash where the name sits
         *
         * The session column-tint palette uses pre-muted hues like
         * 0xff'c5'5a'5a (~77% RGB).  colorForNode returns fully-
         * saturated A400 colours (0xff'00'e6'76 etc.) -- using them
         * raw made the mixer strip too bright vs. session.  Multiply
         * saturation + brightness down to approximate the same
         * visual weight as session's pre-muted palette. */
        constexpr int kTintStripH = 4;
        const auto softTint = tint.withMultipliedSaturation (0.65f)
                                  .withMultipliedBrightness (0.78f);
        g.setColour (softTint);
        g.fillRect (bounds.getX(), bounds.getY(),
                    bounds.getWidth() - 1, kTintStripH);

        g.setColour (tint.withAlpha (0.10f));
        g.fillRect (bounds.getX(), bounds.getY() + kTintStripH,
                    bounds.getWidth() - 1, kNameBandH - kTintStripH);

        g.setColour (juce::Colours::white);
        g.setFont (monoFont (11.0f, juce::Font::bold));
        g.drawText (nodeNameStr_,
                    bounds.getX() + 2, bounds.getY() + kTintStripH,
                    bounds.getWidth() - 4, kNameBandH - kTintStripH,
                    juce::Justification::centred);

        g.setColour (kRowDividerColour);
        g.drawLine ((float) getWidth() - 1.f, 0.0f,
                    (float) getWidth() - 1.f, (float) getHeight());

        /* MUTE / SOLO pill colours -- signature on-state colours
         * (amber-red for MUTE, mustard-yellow for SOLO), tint-derived
         * dim off-state.  Same formula SessionView uses for its
         * column-header buttons.  Set every paint so node-swap
         * re-tints follow.  FlatTintedButton paints raw fillRect +
         * drawRect directly (no LookAndFeel rounded corners /
         * gradients) so the result matches session 1:1. */
        const juce::Colour btnOffBg = tint.withMultipliedSaturation (0.6f)
                                          .withMultipliedBrightness (0.55f);

        muteButton_.onBg   = juce::Colour { 0xff'c0'30'30 };       // amber-red
        muteButton_.offBg  = btnOffBg;
        muteButton_.onFg   = juce::Colours::white;
        muteButton_.offFg  = juce::Colours::white.withAlpha (0.70f);

        soloButton_.onBg   = juce::Colour { 0xff'd5'b0'30 };       // mustard-yellow
        soloButton_.offBg  = btnOffBg;
        soloButton_.onFg   = juce::Colours::black;
        soloButton_.offFg  = juce::Colours::white.withAlpha (0.70f);
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

    inline void setNodeNameEditable (const bool)
    {
        /* No-op since the Label was removed.  Name editing moved to
         * the node inspector / right-click property edit -- consistent
         * with how session-view column names are renamed. */
    }

    /* No-op compatibility shim for the old per-strip name colour
     * override.  The name now paints in white directly via paint(). */
    inline void setNodeNameColour (Colour) {}

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

        /* Sync MUTE / SOLO pill toggles from the new node's stored
         * properties.  channelStrip's existing muteChanged signal
         * mirror also fires post-stabilizeContent for the MUTE side. */
        muteButton_.setToggleState (channelStrip.isMuted(), dontSendNotification);
        soloButton_.setToggleState ((bool) node.getProperty (juce::Identifier ("soloed"), false),
                                     dontSendNotification);

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

public:
    /** Fires when the SOLO pill is toggled.  Visual-only for now --
     *  consumers (graph host, mixer view) can hook this to drive
     *  whatever solo-bus semantics they want.  No-op if unset. */
    std::function<void (bool)> onSoloChanged;

private:
    friend class NodeChannelStripView;
    GuiService& gui;
    /* Name painted directly in paint() (no Label sub-component) so
     * the tint band beneath always shows.  updateNodeName syncs from
     * node.getDisplayName() + repaints. */
    juce::String nodeNameStr_;
    /* Top-level MUTE + SOLO pills -- promoted from inside
     * ChannelStripComponent so the strip's column header reads like
     * SessionView's columns.  MUTE drives channelStrip.setMuted; the
     * inner mute2 button is hidden via channelStrip.setMuteButtonVisible
     * (false) in the ctor.  SOLO writes the "soloed" property + fires
     * onSoloChanged. */
    FlatTintedButton muteButton_ { "MUTE" };
    FlatTintedButton soloButton_ { "SOLO" };
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
            nodeNameStr_ = node.getDisplayName();
            repaint();
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
        const bool muted = channelStrip.isMuted();
        if (node.isValid())
            node.setProperty (tags::mute, muted);
        if (auto* obj = node.getObject())
            obj->setMuted (muted);
        /* Sync the top-level pill button.  When mute is driven by the
         * model (engine bypass, session restore, undo, etc.) the inner
         * channelStrip.mute2 toggle fires this -- mirror it on the
         * promoted muteButton_ so the visual stays in sync. */
        if (muteButton_.getToggleState() != muted)
            muteButton_.setToggleState (muted, dontSendNotification);
    }

    void setUnityGain()
    {
        channelStrip.setVolume (0.0);
    }
};

} // namespace element
