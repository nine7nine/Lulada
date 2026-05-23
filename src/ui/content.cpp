// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <element/services.hpp>
#include <element/context.hpp>
#include <element/settings.hpp>
#include <element/devices.hpp>
#include <element/node.hpp>
#include <element/plugins.hpp>
#include <element/session.hpp>
#include <element/tags.hpp>
#include <element/ui/commands.hpp>
#include <element/ui/content.hpp>
#include <element/ui/style.hpp>

#include "services/mappingservice.hpp"
#include "services/sessionservice.hpp"
#include "ui/blocktoolbutton.hpp"
#include "ui/midiblinker.hpp"
#include <element/ui/mainwindow.hpp>
#include "ui/mainmenu.hpp"
#include "ui/tempoandmeterbar.hpp"
#include "ui/toolbaricons.hpp"
#include "ui/transportbar.hpp"
#include "ui/viewhelpers.hpp"

namespace element {

/* ===================================================================== */
/* MainDisplayPanel: Bitwig-style central digital read-out for the top   */
/* toolbar.  Custom-painted inset frame with large blue-cyan digits.     */
/* Reads transport monitor + session at 15 Hz; mouse wheel adjusts BPM;  */
/* double-click on position seeks to zero.  Embeds 4 mini toggle dots    */
/* (Sync / Metro / Loop / Count) for the secondary affordances.         */
/* ===================================================================== */
class MainDisplayPanel : public juce::Component,
                         private juce::Timer
{
public:
    MainDisplayPanel (Services* svc)
        : services_ (svc)
    {
        setOpaque (false);

        for (auto* b : { &syncBtn_, &metroBtn_, &loopBtn_, &countBtn_ })
        {
            addAndMakeVisible (*b);
            b->setClickingTogglesState (true);
            b->setActiveTint (juce::Colour (0xff'4a'a5'd5));   // matches digit blue
        }
        syncBtn_  .onClick = [this]() {
            syncBtn_.setLabel (syncBtn_.getToggleState() ? "Ex" : "In");
        };

        startTimerHz (15);
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds();

        /* Outer bezel: matte black with a 1px dark-grey rim.  Same
         * idiom as a piece of hardware faceplate inset into the bar. */
        g.setColour (juce::Colour (0xff'08'08'08));
        g.fillRoundedRectangle (bounds.toFloat(), 4.0f);
        g.setColour (juce::Colour (0xff'3a'3a'3a));
        g.drawRoundedRectangle (bounds.toFloat().reduced (0.5f), 4.0f, 1.0f);

        const auto inner = bounds.reduced (4, 4);

        /* Inner LCD-style fill -- very dark cool grey behind the
         * digits, with a soft top-down gradient for depth. */
        juce::ColourGradient lcdGrad (
            juce::Colour (0xff'14'19'1e),
            (float) inner.getX(), (float) inner.getY(),
            juce::Colour (0xff'0c'0f'12),
            (float) inner.getX(), (float) inner.getBottom(),
            false);
        g.setGradientFill (lcdGrad);
        g.fillRoundedRectangle (inner.toFloat(), 3.0f);

        /* Vertical divider between BPM and position columns. */
        const int dividerX = bpmAreaRect_.getRight() + 6;
        g.setColour (juce::Colour (0xff'2a'30'38));
        g.drawVerticalLine (dividerX,
                              (float) (inner.getY() + 4),
                              (float) (inner.getBottom() - 4));

        const juce::Colour digitCol  { 0xff'5a'be'e5 };   // bright cyan-blue
        const juce::Colour digitDim  { 0xff'3a'78'9a };   // softer for sub-text
        const juce::Colour labelCol  { 0xff'6a'7a'8a };

        /* BPM (large) */
        g.setColour (digitCol);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      20.0f, juce::Font::bold));
        g.drawText (bpmStr_, bpmAreaRect_,
                    juce::Justification::centred, false);

        /* Time signature small below BPM. */
        g.setColour (digitDim);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      10.5f, juce::Font::plain));
        g.drawText (meterStr_,
                    bpmAreaRect_.withY (bpmAreaRect_.getBottom() - 2)
                                .withHeight (12),
                    juce::Justification::centred, false);

        /* Position (large) */
        g.setColour (digitCol);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      20.0f, juce::Font::bold));
        g.drawText (positionStr_, posAreaRect_,
                    juce::Justification::centred, false);

        /* Time elapsed small below position. */
        g.setColour (digitDim);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      10.5f, juce::Font::plain));
        g.drawText (timeStr_,
                    posAreaRect_.withY (posAreaRect_.getBottom() - 2)
                                .withHeight (12),
                    juce::Justification::centred, false);

        /* "BPM" + "POS" labels in the top corners, almost-tooltip
         * tiny so they don't compete with the digits. */
        g.setColour (labelCol);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      8.5f, juce::Font::bold));
        g.drawText ("BPM", bpmAreaRect_.getX(), inner.getY() + 1,
                    bpmAreaRect_.getWidth(), 9,
                    juce::Justification::centredLeft, false);
        g.drawText ("POS", posAreaRect_.getX(), inner.getY() + 1,
                    posAreaRect_.getWidth(), 9,
                    juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        const auto inner = getLocalBounds().reduced (4, 4);

        /* Left edge: 2 stacked mini-toggle dots (Sync + Metro). */
        const int dotW = 22;
        const int dotH = juce::jmax (12, (inner.getHeight() - 6) / 2);
        Rectangle<int> leftCol (inner.getX() + 4, inner.getY() + 3, dotW, inner.getHeight() - 6);
        syncBtn_ .setBounds (leftCol.removeFromTop (dotH));
        leftCol.removeFromTop (2);
        metroBtn_.setBounds (leftCol.removeFromTop (dotH));

        /* Right edge: 2 stacked mini-toggle dots (Loop + Count). */
        Rectangle<int> rightCol (inner.getRight() - dotW - 4, inner.getY() + 3,
                                  dotW, inner.getHeight() - 6);
        loopBtn_ .setBounds (rightCol.removeFromTop (dotH));
        rightCol.removeFromTop (2);
        countBtn_.setBounds (rightCol.removeFromTop (dotH));

        /* Centre area holds the BPM and POS readouts on either side
         * of a divider.  BPM column ~95 px (room for "999.99"), POS
         * column ~125 px (room for "999.4.4.99"). */
        const int leftEdge  = syncBtn_.getRight() + 6;
        const int rightEdge = loopBtn_.getX() - 6;
        const int textTop   = inner.getY() + 10;
        const int textH     = inner.getHeight() - 24;
        const int bpmW      = 95;
        const int posW      = juce::jmax (110, rightEdge - leftEdge - bpmW - 12);
        bpmAreaRect_ = Rectangle<int> (leftEdge, textTop, bpmW, textH);
        posAreaRect_ = Rectangle<int> (leftEdge + bpmW + 12, textTop, posW, textH);
    }

    /* BPM scroll-to-edit: mouse wheel on the BPM cell nudges tempo
     * by 1 (shift = 0.1).  Cheap, no popup needed. */
    void mouseWheelMove (const juce::MouseEvent& e,
                          const juce::MouseWheelDetails& w) override
    {
        if (! bpmAreaRect_.contains (e.x, e.y)) return;
        if (services_ == nullptr) return;
        auto session = services_->context().session();
        if (session == nullptr) return;

        const float step = e.mods.isShiftDown() ? 0.1f : 1.0f;
        const float delta = (w.deltaY > 0 ? 1.0f : (w.deltaY < 0 ? -1.0f : 0.0f)) * step;
        if (delta == 0.0f) return;

        auto tempoVal = session->getPropertyAsValue (tags::tempo);
        const float current = (float) (double) tempoVal.getValue();
        const float next = juce::jlimit (20.0f, 999.0f, current + delta);
        tempoVal.setValue ((double) next);
    }

    /* Double-click on the POS column seeks to zero.  Established
     * convention from the legacy bar label. */
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! posAreaRect_.contains (e.x, e.y)) return;
        if (services_ == nullptr) return;
        if (auto eng = services_->context().audio().get())
            eng->seekToAudioFrame (0);
    }

private:
    void timerCallback() override
    {
        if (services_ == nullptr) return;

        if (monitor_ == nullptr)
        {
            if (auto eng = services_->context().audio().get())
                monitor_ = eng->getTransportMonitor();
        }
        auto session = services_->context().session();
        if (monitor_ == nullptr || session == nullptr) return;

        const float bpm = monitor_->tempo.get();
        const int bpb   = juce::jmax (1, monitor_->beatsPerBar.get());
        const int bt    = juce::jmax (1, monitor_->beatType.get());

        bool dirty = false;
        const auto newBpm   = juce::String (bpm, 2);
        const auto newMeter = juce::String (bpb) + "/" + juce::String (bt);
        if (newBpm   != bpmStr_)   { bpmStr_   = newBpm;   dirty = true; }
        if (newMeter != meterStr_) { meterStr_ = newMeter; dirty = true; }

        int bars = 0, beats = 0, sub = 0;
        monitor_->getBarsAndBeats (bars, beats, sub);
        auto newPos = juce::String (bars + 1) + "."
                    + juce::String (beats + 1) + "."
                    + juce::String (sub + 1);
        if (newPos != positionStr_) { positionStr_ = newPos; dirty = true; }

        const double secs = monitor_->getPositionSeconds();
        const int totalMs = (int) std::lround (secs * 1000.0);
        const int mm = totalMs / 60000;
        const int rest = totalMs - mm * 60000;
        const int ss = rest / 1000;
        const int ms = rest - ss * 1000;
        juce::String newTime;
        newTime << mm << ":";
        if (ss < 10) newTime << "0";
        newTime << ss << ".";
        if (ms < 100) newTime << "0";
        if (ms < 10)  newTime << "0";
        newTime << ms;
        if (newTime != timeStr_) { timeStr_ = newTime; dirty = true; }

        if (dirty) repaint();
    }

    Services*               services_ { nullptr };
    Transport::MonitorPtr   monitor_;

    juce::String bpmStr_      { "120.00" };
    juce::String meterStr_    { "4/4"     };
    juce::String positionStr_ { "1.1.1"   };
    juce::String timeStr_     { "0:00.000" };

    Rectangle<int> bpmAreaRect_;
    Rectangle<int> posAreaRect_;

    BlockToolButton syncBtn_  { "In" };
    BlockToolButton metroBtn_ { "Me" };
    BlockToolButton loopBtn_  { "Lp" };
    BlockToolButton countBtn_ { "Ct" };
};

ContentView::ContentView()
{
}

ContentView::~ContentView()
{
}

void ContentView::paint (Graphics& g)
{
    auto c = findColour (Style::widgetBackgroundColorId).darker();
    g.fillAll (c);
}

bool ContentView::keyPressed (const KeyPress& k)
{
    if (escapeTriggersClose && k == KeyPress::escapeKey)
    {
        ViewHelpers::invokeDirectly (this, Commands::showLastContentView, true);
        return true;
    }

    return false;
}

//=============================================================================
/* 4 colour-coded view selectors that replace the old monolithic
 * "view" toggle.  Each cell maps to one main-window page: PatchBay /
 * Graph / Arrangement / Trackers.  Active page = bright fill;
 * inactive = darker / desaturated variant.  Tick state mirrors the
 * Commands::show* ApplicationCommandInfo so it stays in sync with
 * keyboard shortcuts + menu invocations. */
class ViewSelectorBar : public juce::Component,
                        private juce::Timer
{
public:
    ViewSelectorBar()
        : patchBtn   ("", juce::Colour::fromRGB ( 80, 160, 200)),
          graphBtn   ("", juce::Colour::fromRGB (110, 170, 110)),
          arrBtn     ("", juce::Colour::fromRGB (220, 140,  60)),
          trkBtn     ("", juce::Colour::fromRGB (160, 100, 180)),
          sessionBtn ("", juce::Colour::fromRGB ( 80, 200, 170)),
          diskBtn    ("", juce::Colour::fromRGB (200, 180,  80)),
          plugBtn    ("", juce::Colour::fromRGB (190, 110, 170))
    {
        using IconFn = void(*)(juce::Graphics&, juce::Rectangle<float>, juce::Colour);
        auto wire = [this] (BlockToolButton& b, const juce::String& tip, int commandID,
                              IconFn icon)
        {
            b.setTooltip (tip);
            b.setIcon ([icon] (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fg)
            {
                icon (g, r, fg);
            });
            b.onClick = [this, commandID]() {
                ViewHelpers::invokeDirectly (this, commandID, true);
            };
            addAndMakeVisible (b);
        };
        wire (patchBtn,   "Patch Bay",      Commands::showPatchBay,      &ui::iconPatchBay);
        wire (graphBtn,   "Graph Editor",   Commands::showGraphEditor,   &ui::iconGraph);
        wire (arrBtn,     "Arrangement",    Commands::showArrangement,   &ui::iconArrangement);
        wire (trkBtn,     "Trackers",       Commands::showTrackerHost,   &ui::iconTracker);
        wire (sessionBtn, "Session",        Commands::showSessionView,   &ui::iconSession);
        wire (diskBtn,    "Disk Op",        Commands::showDiskOp,        &ui::iconDisk);
        wire (plugBtn,    "Plugin Manager", Commands::showPluginManager, &ui::iconPluginManager);

        startTimer (150); // tick-state refresh; cheap, no repaint when nothing changed
    }

    void resized() override
    {
        auto r = getLocalBounds();
        const int n  = 7;
        const int w  = r.getWidth() / n;
        patchBtn  .setBounds (r.removeFromLeft (w));
        graphBtn  .setBounds (r.removeFromLeft (w));
        arrBtn    .setBounds (r.removeFromLeft (w));
        trkBtn    .setBounds (r.removeFromLeft (w));
        sessionBtn.setBounds (r.removeFromLeft (w));
        diskBtn   .setBounds (r.removeFromLeft (w));
        plugBtn   .setBounds (r);
    }

private:
    void timerCallback() override
    {
        auto* gui = ViewHelpers::getGuiController (this);
        if (gui == nullptr) return;
        auto& cm = gui->commands();
        auto refresh = [&cm] (BlockToolButton& b, int id)
        {
            const auto* info = cm.getCommandForID (id);
            const bool ticked = info != nullptr
                && (info->flags & juce::ApplicationCommandInfo::isTicked) != 0;
            if (b.getToggleState() != ticked)
            {
                b.setToggleState (ticked, juce::dontSendNotification);
                b.repaint();
            }
        };
        refresh (patchBtn,   Commands::showPatchBay);
        refresh (graphBtn,   Commands::showGraphEditor);
        refresh (arrBtn,     Commands::showArrangement);
        refresh (trkBtn,     Commands::showTrackerHost);
        refresh (sessionBtn, Commands::showSessionView);
        refresh (diskBtn,    Commands::showDiskOp);
        refresh (plugBtn,    Commands::showPluginManager);
    }

    BlockToolButton patchBtn, graphBtn, arrBtn, trkBtn,
                    sessionBtn, diskBtn, plugBtn;
};

class Content::Toolbar : public Component,
                         public Button::Listener,
                         public Timer
{
public:
    Toolbar (Content& o)
        : owner (o)
    {
        addAndMakeVisible (viewSelector);
        addAndMakeVisible (tempoBar);
        addAndMakeVisible (transport);

        mapButton.setButtonText (TRANS ("map"));
        mapButton.setColour (SettingButton::backgroundOnColourId, Colors::toggleBlue);
        mapButton.addListener (this);
        addAndMakeVisible (mapButton);
        mapButton.setVisible (false);

        pluginMenu.setIcon (Icon (getIcons().falBarsOutline,
                                  findColour (TextButton::textColourOffId)));
        pluginMenu.setTriggeredOnMouseDown (true);
        pluginMenu.onClick = [this]() { runPluginMenu(); };
        if (owner.services().getRunMode() == RunMode::Plugin)
            addAndMakeVisible (pluginMenu);

        addAndMakeVisible (midiBlinker);

        /* New central digital readout -- BPM + position + meter +
         * elapsed all bundled in one inset Bitwig-style panel.
         * Replaces TempoAndMeterBar's tiny BPM/TAP/4-4 cluster +
         * TransportBar's bars/beats/sub labels in this toolbar. */
        addAndMakeVisible (display_);

        /* Hide the redundant pieces: the legacy TempoAndMeterBar
         * (BPM in display_), and TransportBar's position labels
         * (also in display_).  TransportBar now reads as a tight
         * Play/Stop/Record/SeekZero cluster on the left. */
        tempoBar.setVisible (false);
        transport.setShowPositionLabels (false);
        transport.updateWidth();

        /* Undo / Redo migrate from the Edit menu to the toolbar.
         * File menu + plugin-windows buttons were pulled per user
         * feedback (file ops live in Disk Op, plugin windows are
         * reachable via the system Window menu when needed). */
        using IconFn = void(*)(juce::Graphics&, juce::Rectangle<float>, juce::Colour);
        auto wireBtn = [&] (BlockToolButton& b, const juce::String& tip,
                              IconFn icon, std::function<void()> action)
        {
            b.setTooltip (tip);
            b.setIcon ([icon] (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fg)
                       { icon (g, r, fg); });
            b.onClick = std::move (action);
            addAndMakeVisible (b);
        };
        wireBtn (undoBtn_, "Undo", &ui::iconUndo,
                  [this]() { ViewHelpers::invokeDirectly (this, Commands::undo, true); });
        wireBtn (redoBtn_, "Redo", &ui::iconRedo,
                  [this]() { ViewHelpers::invokeDirectly (this, Commands::redo, true); });
    }

    ~Toolbar()
    {
        for (const auto& conn : connections)
            conn.disconnect();
        connections.clear();
    }

    void setSession (SessionPtr s)
    {
        session = s;

        auto& context = *ViewHelpers::getGlobals (this);
        auto& settings (context.settings());
        auto engine (context.audio());

        if (midiIOMonitor == nullptr)
        {
            midiIOMonitor = engine->getMidiIOMonitor();
            connections.add (midiIOMonitor->sigSent.connect (
                std::bind (&MidiBlinker::triggerSent, &midiBlinker)));
            connections.add (midiIOMonitor->sigReceived.connect (
                std::bind (&MidiBlinker::triggerReceived, &midiBlinker)));
        }

        auto* props = settings.getUserSettings();
        const bool showExt = context.services().getRunMode() == RunMode::Plugin || props->getValue ("clockSource") == "midiClock";

        if (session)
        {
            tempoBar.setUseExtButton (showExt);
            tempoBar.getTempoValue().referTo (session->getPropertyAsValue (tags::tempo));
            tempoBar.getExternalSyncValue().referTo (session->getPropertyAsValue (tags::externalSync));
            tempoBar.stabilizeWithSession (false);
        }

        mapButton.setEnabled (settings.getBool ("legacyControllers", false));
        mapButton.setVisible (mapButton.isEnabled());
        resized();
    }

    void resized() override
    {
        /* Single-row Bitwig-style layout:
         *
         *   [menu | undo | redo] [transport] [== display ==] [pluginwin] [view selector ...]
         *
         * Migrated-from-menus icon buttons hard-left, transport just
         * to their right, central display floats with the leftover
         * mid-width, view selector + plugin window toggle + plugin
         * menu + midi blinker right-aligned. */
        Rectangle<int> r = getLocalBounds();
        const int H = r.getHeight();
        const int innerPad = juce::jmax (3, (H - 50) / 2);
        const int rowH = H - innerPad * 2;

        constexpr int kSidePad = 6;
        constexpr int kGap     = 10;
        constexpr int kIconBtnW = 30;
        constexpr int kDisplayW = 380;

        r.reduce (kSidePad, 0);
        const int top = r.getY() + innerPad;

        /* ---- LEFT: undo / redo pair ---- */
        undoBtn_.setBounds (r.removeFromLeft (kIconBtnW).withY (top).withHeight (rowH));
        r.removeFromLeft (2);
        redoBtn_.setBounds (r.removeFromLeft (kIconBtnW).withY (top).withHeight (rowH));
        r.removeFromLeft (kGap);

        /* ---- Transport (Play / Stop / Record / SeekZero) ---- */
        if (transport.isVisible())
        {
            transport.setShowPositionLabels (false);
            transport.setSize (transport.getWidth(), rowH);
            transport.updateWidth();
            const int tW = juce::jmax (160, transport.getWidth());
            transport.setBounds (r.getX(), top, tW, rowH);
            r.removeFromLeft (tW + kGap);
        }

        /* ---- RIGHT (right-aligned chain): viewSelector | pluginWin |
                pluginMenu | midiBlinker.  Walk r.removeFromRight in
                the visible order. ---- */
        if (pluginMenu.isVisible())
        {
            const int pms = rowH + 3;
            pluginMenu.setBounds (r.removeFromRight (rowH)
                                       .withSizeKeepingCentre (pms, pms));
            r.removeFromRight (kGap / 2);
        }
        if (midiBlinker.isVisible())
        {
            const int blinkerW = 8;
            midiBlinker.setBounds (r.removeFromRight (blinkerW)
                                       .withSizeKeepingCentre (blinkerW, rowH));
            r.removeFromRight (kGap / 2);
        }
        if (viewSelector.isVisible())
        {
            /* 7 view buttons now: P G A T S D M.  Same per-button
             * size as before (rowH wide each). */
            viewSelector.setBounds (r.removeFromRight (rowH * 7)
                                       .withSizeKeepingCentre (rowH * 7, rowH));
        }
        if (mapButton.isVisible())
        {
            r.removeFromRight (4);
            mapButton.setBounds (r.removeFromRight (rowH * 2)
                                     .withSizeKeepingCentre (rowH * 2, rowH));
        }
        r.removeFromRight (kGap);

        /* ---- CENTRE: digital display fills the remaining mid-band ---- */
        {
            const int dispW = juce::jmin (kDisplayW, juce::jmax (240, r.getWidth() - 8));
            const int dispX = r.getX() + (r.getWidth() - dispW) / 2;
            display_.setBounds (dispX, top, dispW, rowH);
        }
    }

    void paint (Graphics& g) override
    {
        /* Match the parent Content's backgroundColor (0xff16191A — the
         * blue-grey tone) exactly.  This is the color the body area
         * actually shows (Content::paint fills with backgroundColor,
         * NOT contentBackgroundColor), so top + body + bottom read as
         * one continuous frame. */
        g.setColour (Colors::backgroundColor);
        g.fillRect (getLocalBounds());
    }

    void buttonClicked (Button* btn) override
    {
        if (btn == &mapButton)
        {
            if (auto* mapping = owner.services().find<MappingService>())
            {
                mapping->learn (! mapButton.getToggleState());
                mapButton.setToggleState (mapping->isLearning(), dontSendNotification);
                if (mapping->isLearning())
                {
                    startTimer (600);
                }
            }
        }
    }

    void timerCallback() override
    {
        /* MainDisplayPanel runs its own timer for the digital read-
         * out.  Toolbar's timer is now used only for mapping-learn
         * auto-clear (legacy behaviour). */
        if (auto* mapping = owner.services().find<MappingService>())
        {
            if (! mapping->isLearning() && mapButton.getToggleState())
            {
                mapButton.setToggleState (false, dontSendNotification);
                stopTimer();
            }
        }
    }

private:
    Content& owner;
    SessionPtr session;
    MidiIOMonitorPtr midiIOMonitor;
    ViewSelectorBar viewSelector;
    SettingButton mapButton;
    TempoAndMeterBar tempoBar;
    TransportBar transport;
    IconButton pluginMenu;
    MidiBlinker midiBlinker;
    Array<SignalConnection> connections;

    /* Central digital readout (Bitwig-style faceplate).  Holds BPM
     * + meter + position + elapsed + Sync/Metro/Loop/Count micro
     * toggles.  Self-timed at 15 Hz; pulls state directly from the
     * transport monitor + session. */
    MainDisplayPanel display_ { &owner.services() };

    /* Edit Undo / Redo migrated onto the toolbar.  File-menu +
     * plugin-windows buttons removed per user feedback. */
    BlockToolButton undoBtn_ { "" };
    BlockToolButton redoBtn_ { "" };

    void runPluginMenu()
    {
        auto& ui = *owner.services().find<UI>();
        PopupMenu menu;
        MainMenu::buildPluginMainMenu (ui.commands(), menu);
        menu.show();
    }
};

class Content::StatusBar : public Component,
                           public Value::Listener,
                           private Timer
{
public:
    StatusBar (Context& g)
        : world (g),
          devices (world.devices()),
          plugins (world.plugins())
    {
        sampleRate.addListener (this);
        streamingStatus.addListener (this);

        addAndMakeVisible (sampleRateLabel);
        addAndMakeVisible (streamingStatusLabel);
        addAndMakeVisible (statusLabel);

        const Font font (FontOptions (12.0f));

        for (int i = 0; i < getNumChildComponents(); ++i)
        {
            if (Label* label = dynamic_cast<Label*> (getChildComponent (i)))
            {
                label->setFont (font);
                label->setColour (Label::textColourId, Colors::textColor);
                label->setJustificationType (Justification::centredLeft);
            }
        }

        startTimer (2000);
        updateLabels();
    }

    ~StatusBar()
    {
        latencySamplesChangedConnection.disconnect();
        sampleRate.removeListener (this);
        streamingStatus.removeListener (this);
    }

    void paint (Graphics& g) override
    {
        /* Match the top Toolbar + body — Colors::backgroundColor
         * (0xff16191A blue-grey).  Top + body + bottom strips frame
         * the body without a tone shift.  No top 2-line border — the
         * status text sits flush against the body so the bottom strip
         * blends in instead of slicing the window with a bright line. */
        g.setColour (Colors::backgroundColor);
        g.fillRect (getLocalBounds());

        const Colour lineColor (0xff545454);
        g.setColour (lineColor);
        g.drawLine (streamingStatusLabel.getX(), 0, streamingStatusLabel.getX(), getHeight());
        g.drawLine (sampleRateLabel.getX(), 0, sampleRateLabel.getX(), getHeight());
    }

    void resized() override
    {
        Rectangle<int> r (getLocalBounds());
        statusLabel.setBounds (r.removeFromLeft (getWidth() / 5));
        streamingStatusLabel.setBounds (r.removeFromLeft (r.getWidth() / 2));
        sampleRateLabel.setBounds (r);
    }

    void valueChanged (Value&) override
    {
        updateLabels();
    }

    void updateLabels()
    {
        auto engine = world.audio();
        const auto mode = world.services().getRunMode();

        if (auto* dev = devices.getCurrentAudioDevice())
        {
            String text = "Sample Rate: ";
            text << String (dev->getCurrentSampleRate() * 0.001, 1) << " KHz";
            text << ":  Buffer: " << dev->getCurrentBufferSizeSamples();
            sampleRateLabel.setText (text, dontSendNotification);

            text.clear();
            String strText = streamingStatus.getValue().toString();
            if (strText.isEmpty())
                strText = "Running";
            text << "Engine: " << strText << ":  CPU: " << String (devices.getCpuUsage() * 100.f, 1) << "%";
            streamingStatusLabel.setText (text, dontSendNotification);

            statusLabel.setText (String ("Device: ") + dev->getName(), dontSendNotification);
            statusLabel.setColour (Label::textColourId, Colors::textColor);
        }
        else if (mode == RunMode::Plugin)
        {
            sampleRateLabel.setText ("", dontSendNotification);
            String text = String (engine->getExternalLatencySamples());
            text << " samples";
            streamingStatusLabel.setText (text, dontSendNotification);
            statusLabel.setText ("Host", dontSendNotification);
        }
        else
        {
            sampleRateLabel.setText ("N/A", dontSendNotification);
            streamingStatusLabel.setText ("N/A", dontSendNotification);
            statusLabel.setText ("No Device", dontSendNotification);
            statusLabel.setColour (Label::textColourId, Colors::toggleRed);
        }

        if (plugins.isScanningAudioPlugins())
        {
            auto text = streamingStatusLabel.getText();
            auto name = plugins.getCurrentlyScannedPluginName();
            name = File::createFileWithoutCheckingPath (name).getFileName();

            text << " - Scanning: " << name;
            if (name.isNotEmpty())
                streamingStatusLabel.setText (text, dontSendNotification);
        }
    }

private:
    Context& world;
    DeviceManager& devices;
    PluginManager& plugins;

    Label sampleRateLabel, streamingStatusLabel, statusLabel;
    ValueTree node;
    Value sampleRate, streamingStatus, status;

    SignalConnection latencySamplesChangedConnection;

    friend class Timer;
    void timerCallback() override
    {
        updateLabels();
    }
};

struct Content::Tooltips
{
    Tooltips() { tooltipWindow.reset (new TooltipWindow()); }
    std::unique_ptr<TooltipWindow> tooltipWindow;
};

Content::Content (Context& ctx)
    : _context (ctx),
      _services (ctx.services())
{
    setOpaque (true);

    statusBar = std::make_unique<StatusBar> (context());
    addAndMakeVisible (statusBar.get());
    statusBarVisible = true;
    statusBarSize = 22;

    toolBar = std::make_unique<Toolbar> (*this);
    addAndMakeVisible (toolBar.get());
    toolBar->setSession (context().session());
    toolBarVisible = true;
    /* Thicker bar to fit the secondary Bitwig-style row beneath the
     * primary tempo / transport row.  32 -> 60 = +28 px; the two-row
     * layout in Content::Toolbar::resized() splits it ~50/50. */
    toolBarSize = 60;

    const Node node (context().session()->getCurrentGraph());
    setCurrentNode (node);

    resized();
}

Content::~Content() noexcept
{
}

void Content::paint (Graphics& g)
{
    g.fillAll (Colors::backgroundColor);
}

void Content::resized()
{
    Rectangle<int> r (getLocalBounds());

    if (toolBarVisible && toolBar)
        toolBar->setBounds (r.removeFromTop (toolBarSize));
    if (statusBarVisible && statusBar)
        statusBar->setBounds (r.removeFromBottom (statusBarSize));

    resizeContent (r);
}

void Content::post (Message* message)
{
    _services.postMessage (message);
}

void Content::setToolbarVisible (bool visible)
{
    if (toolBarVisible == visible)
        return;
    toolBarVisible = visible;
    toolBar->setVisible (toolBarVisible);
    resized();
    refreshToolbar();
}

void Content::refreshToolbar()
{
    toolBar->setSession (context().session());
}

void Content::setStatusBarVisible (bool vis)
{
    if (statusBarVisible == vis)
        return;
    statusBarVisible = vis;
    if (statusBar)
        statusBar->setVisible (vis);
    resized();
    refreshStatusBar();
}

void Content::refreshStatusBar()
{
    if (statusBar)
        statusBar->updateLabels();
}

Context& Content::context() { return _context; }
SessionPtr Content::session() { return _context.session(); }
void Content::stabilize (const bool refreshDataPathTrees) {}
void Content::stabilizeViews() {}
void Content::saveState (PropertiesFile*) {}
void Content::restoreState (PropertiesFile*) {}
void Content::setCurrentNode (const Node& node) { ignoreUnused (node); }

} // namespace element
