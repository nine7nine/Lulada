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

#include "ui/fontcache.hpp"
#include "ui/lcdsublabel.hpp"

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

/* Shared paint helper for the LCD-style faceplate frame used by the
 * main toolbar's button groups + central display.  Matte-black outer
 * bezel + 1-px dark-grey rim + cool-grey vertical gradient inside. */
static void paintLcdFrame (juce::Graphics& g, juce::Rectangle<int> r,
                            float cornerSize = 4.0f)
{
    /* LCD-style faceplate: deep-black outer bezel + brighter rim
     * highlight so the frame pops against the toolbar's blue-grey
     * background (Colors::backgroundColor ~= 0xff16191A), plus a
     * cool-grey inner gradient.  Previous matte-black blended with
     * the bar; this brighter rim is what makes the frame visible. */
    const auto frect = r.toFloat();
    g.setColour (juce::Colour (0xff'05'05'05));
    g.fillRoundedRectangle (frect, cornerSize);
    /* Rim colour matches MainDisplayPanel's bezel (0xff3a3a3a) so
     * every cluster reads as the same material as the central LCD. */
    g.setColour (juce::Colour (0xff'3a'3a'3a));
    g.drawRoundedRectangle (frect.reduced (0.5f), cornerSize, 1.0f);

    const auto inner = frect.reduced (3.0f);
    juce::ColourGradient lcdGrad (
        juce::Colour (0xff'1a'1f'24),
        inner.getX(), inner.getY(),
        juce::Colour (0xff'0c'0f'12),
        inner.getX(), inner.getBottom(),
        false);
    g.setGradientFill (lcdGrad);
    g.fillRoundedRectangle (inner, juce::jmax (0.5f, cornerSize - 1.0f));

    /* 1-px top highlight line inside the rim, same "lit from above"
     * cue BlockToolButton uses -- ties the frame to the button
     * family. */
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.drawLine (inner.getX() + 1.0f, inner.getY() + 0.5f,
                inner.getRight() - 1.0f, inner.getY() + 0.5f, 1.0f);
}

/* ===================================================================== */
/* MainDisplayPanel: Bitwig-style central digital read-out for the top   */
/* toolbar.  Custom-painted inset frame with large blue-cyan digits.     */
/* Reads transport monitor + session at 15 Hz; mouse wheel adjusts BPM;  */
/* double-click on position seeks to zero.  Embeds 4 mini toggle dots    */
/* (Sync / Metro / Loop / Count) for the secondary affordances.         */
/* ===================================================================== */
/* Tiny stacked I/O pips for MIDI in / out activity inside the LCD
 * panel.  Two squares; cyan (matches the digit colour) when active,
 * dim cool-grey when idle.  Activity holds for 100 ms then fades. */
class MidiPips : public juce::Component, private juce::Timer
{
public:
    MidiPips() = default;
    void triggerSent()     { haveOut_ = true; repaint(); startTimer (kHoldMs); }
    void triggerReceived() { haveIn_  = true; repaint(); startTimer (kHoldMs); }

    void paint (juce::Graphics& g) override
    {
        const juce::Colour onCol  { 0xff'5a'be'e5 };   // LCD blue
        const juce::Colour offCol { 0xff'1d'25'2c };
        const juce::Colour edge   { 0xff'3a'4a'56 };
        const auto b = getLocalBounds();
        const int half = b.getHeight() / 2;
        const auto in  = juce::Rectangle<int> (b.getX(), b.getY(),
                                                 b.getWidth(), half - 1);
        const auto out = juce::Rectangle<int> (b.getX(), b.getY() + half + 1,
                                                 b.getWidth(), half - 1);
        g.setColour (haveIn_  ? onCol : offCol); g.fillRect (in);
        g.setColour (haveOut_ ? onCol : offCol); g.fillRect (out);
        g.setColour (edge);
        g.drawRect (in,  1);
        g.drawRect (out, 1);
    }

private:
    void timerCallback() override
    {
        haveIn_ = haveOut_ = false;
        stopTimer();
        repaint();
    }
    static constexpr int kHoldMs = 100;
    bool haveIn_ { false }, haveOut_ { false };
};

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

        addAndMakeVisible (midiPips_);

        startTimerHz (15);
    }

    /** Access the embedded MIDI pips so the toolbar can hook
     *  triggerSent / triggerReceived signals from the audio engine
     *  monitor. */
    MidiPips& getMidiPips() noexcept { return midiPips_; }

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
        /* Bright green for the secondary labels + sub-digits --
         * "4/4" under BPM + "0:00.000" under POS + the "BPM" / "POS"
         * corner labels.  Picks up against the cyan main digits like
         * a hardware multi-line LCD. */
        const juce::Colour digitDim  { 0xff'7a'e0'8a };
        const juce::Colour labelCol  { 0xff'7a'e0'8a };

        /* BPM (large) */
        g.setColour (digitCol);
        g.setFont (monoFont (
                                      20.0f, juce::Font::bold));
        g.drawText (bpmStr_, bpmAreaRect_,
                    juce::Justification::centred, false);

        /* Time signature small below BPM. */
        g.setColour (digitDim);
        g.setFont (monoFont (
                                      10.5f, juce::Font::plain));
        g.drawText (meterStr_,
                    bpmAreaRect_.withY (bpmAreaRect_.getBottom() - 2)
                                .withHeight (12),
                    juce::Justification::centred, false);

        /* Position (large) */
        g.setColour (digitCol);
        g.setFont (monoFont (
                                      20.0f, juce::Font::bold));
        g.drawText (positionStr_, posAreaRect_,
                    juce::Justification::centred, false);

        /* Time elapsed small below position. */
        g.setColour (digitDim);
        g.setFont (monoFont (
                                      10.5f, juce::Font::plain));
        g.drawText (timeStr_,
                    posAreaRect_.withY (posAreaRect_.getBottom() - 2)
                                .withHeight (12),
                    juce::Justification::centred, false);

        /* "BPM" + "POS" labels in the top corners, almost-tooltip
         * tiny so they don't compete with the digits. */
        g.setColour (labelCol);
        g.setFont (monoFont (
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

        /* Right edge of the inner panel: the MIDI in/out pips sit
         * in their own slim column at the far right; the Loop +
         * Count mini-buttons sit just to the LEFT of the pips. */
        const int pipW = 10;
        Rectangle<int> pipsCol (inner.getRight() - pipW - 2, inner.getY() + 3,
                                  pipW, inner.getHeight() - 6);
        midiPips_.setBounds (pipsCol);

        Rectangle<int> rightCol (pipsCol.getX() - dotW - 4, inner.getY() + 3,
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
    MidiPips        midiPips_;
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
        : graphBtn   ("", juce::Colour::fromRGB (110, 170, 110)),
          arrBtn     ("", juce::Colour::fromRGB (220, 140,  60)),
          trkBtn     ("", juce::Colour::fromRGB (160, 100, 180)),
          sessionBtn ("", juce::Colour::fromRGB ( 80, 200, 170)),
          patchBtn   ("", juce::Colour::fromRGB ( 80, 160, 200))
    {
        /* Disk Op + Plugin Manager live in the leftmost cluster;
         * this selector covers the 5 graph-surface views, in order:
         * Graph / Arr / Tracker / Session / Patch Bay. */
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
        wire (graphBtn,   "Graph Editor",   Commands::showGraphEditor,   &ui::iconGraph);
        wire (arrBtn,     "Arrangement",    Commands::showArrangement,   &ui::iconArrangement);
        wire (trkBtn,     "Trackers",       Commands::showTrackerHost,   &ui::iconTracker);
        wire (sessionBtn, "Session",        Commands::showSessionView,   &ui::iconSession);
        wire (patchBtn,   "Patch Bay",      Commands::showPatchBay,      &ui::iconPatchBay);

        auto setActiveFromTint = [] (BlockToolButton& b, juce::Colour tint)
        {
            b.setActiveTint (tint);
        };
        setActiveFromTint (graphBtn,   juce::Colour::fromRGB (110, 170, 110));
        setActiveFromTint (arrBtn,     juce::Colour::fromRGB (220, 140,  60));
        setActiveFromTint (trkBtn,     juce::Colour::fromRGB (160, 100, 180));
        setActiveFromTint (sessionBtn, juce::Colour::fromRGB ( 80, 200, 170));
        setActiveFromTint (patchBtn,   juce::Colour::fromRGB ( 80, 160, 200));

        addAndMakeVisible (graphLbl_);
        addAndMakeVisible (arrLbl_);
        addAndMakeVisible (trkLbl_);
        addAndMakeVisible (sessionLbl_);
        addAndMakeVisible (patchLbl_);

        startTimer (150);
    }

    void paint (juce::Graphics& g) override
    {
        paintLcdFrame (g, getLocalBounds());
    }

    void resized() override
    {
        constexpr int kFramePad   = 5;
        constexpr int kGap        = 4;
        constexpr int kSublabelH  = 13;
        constexpr int kSublabelGap = 3;
        /* No cap on btnH -- main clusters scale with the toolbar so
         * the labeled buttons keep dominant visual weight. */
        const int n = 5;
        auto r = getLocalBounds().reduced (kFramePad, kFramePad);
        const int total  = r.getWidth();
        const int w      = (total - kGap * (n - 1)) / n;
        const int btnH   = juce::jmax (16, r.getHeight() - kSublabelH - kSublabelGap);
        /* Vertical-centre the icon+label stack inside the frame. */
        const int stackH = btnH + kSublabelGap + kSublabelH;
        const int btnY   = r.getY() + juce::jmax (0, (r.getHeight() - stackH) / 2);
        const int lblY   = btnY + btnH + kSublabelGap;

        int x = r.getX();
        auto place = [&] (BlockToolButton& b, LcdSublabel& lbl)
        {
            b.setBounds  (x, btnY, w, btnH);
            lbl.setBounds (x, lblY, w, kSublabelH);
            x += w + kGap;
        };
        place (graphBtn,   graphLbl_);
        place (arrBtn,     arrLbl_);
        place (trkBtn,     trkLbl_);
        place (sessionBtn, sessionLbl_);
        /* Last button absorbs any rounding slack in the remainder. */
        const int lastW = r.getRight() - x;
        patchBtn  .setBounds (x, btnY, lastW, btnH);
        patchLbl_ .setBounds (x, lblY, lastW, kSublabelH);
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
        refresh (graphBtn,   Commands::showGraphEditor);
        refresh (arrBtn,     Commands::showArrangement);
        refresh (trkBtn,     Commands::showTrackerHost);
        refresh (sessionBtn, Commands::showSessionView);
        refresh (patchBtn,   Commands::showPatchBay);
    }

    BlockToolButton graphBtn, arrBtn, trkBtn, sessionBtn, patchBtn;

    LcdSublabel graphLbl_   { "GRAPH" };
    LcdSublabel arrLbl_     { "ARR" };
    LcdSublabel trkLbl_     { "TRK" };
    LcdSublabel sessionLbl_ { "SESS" };
    LcdSublabel patchLbl_   { "PATCH" };
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

        /* MidiBlinker moved INTO the central display panel; no
         * standalone copy on the toolbar. */

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

        /* Reordered toolbar layout:
         *   far-left:  Disk Op + Plugin Manager (file + plugin
         *              browser shortcuts) + Preferences (cog).
         *   then:      transport (Play / Stop / Record / SeekZero).
         *   then:      Virtual Keyboard + Undo + Redo.
         *   centre:    digital display.
         *   right:     5-view selector + plugin menu + midi blinker. */
        using IconFn = void(*)(juce::Graphics&, juce::Rectangle<float>, juce::Colour);
        auto wireBtn = [&] (BlockToolButton& b, const juce::String& tip,
                              IconFn icon, std::function<void()> action,
                              juce::Colour borderTint = {})
        {
            b.setTooltip (tip);
            b.setIcon ([icon] (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fg)
                       { icon (g, r, fg); });
            b.onClick = std::move (action);
            addAndMakeVisible (b);
            juce::ignoreUnused (borderTint);
        };
        wireBtn (diskOpBtn_,    "Disk Op",         &ui::iconDisk,
                  [this]() { ViewHelpers::invokeDirectly (this, Commands::showDiskOp, true); });
        wireBtn (pluginMgrBtn_, "Plugin Manager",  &ui::iconPluginManager,
                  [this]() { ViewHelpers::invokeDirectly (this, Commands::showPluginManager, true); });
        wireBtn (prefsBtn_,     "Preferences...",  &ui::iconCog,
                  [this]() { ViewHelpers::invokeDirectly (this, Commands::showPreferences, true); });

        addAndMakeVisible (diskOpLbl_);
        addAndMakeVisible (pluginMgrLbl_);
        addAndMakeVisible (prefsLbl_);
        wireBtn (graphMixBtn_,  "Graph Mixer",      &ui::iconMixer,
                  [this]() { ViewHelpers::invokeDirectly (this, Commands::showGraphMixer, true); });
        wireBtn (vKbdBtn_,      "Virtual keyboard", &ui::iconKeyboard,
                  [this]() { ViewHelpers::invokeDirectly (this, Commands::toggleVirtualKeyboard, true); });
        wireBtn (undoBtn_,      "Undo",             &ui::iconUndo,
                  [this]() { ViewHelpers::invokeDirectly (this, Commands::undo, true); });
        wireBtn (redoBtn_,      "Redo",             &ui::iconRedo,
                  [this]() { ViewHelpers::invokeDirectly (this, Commands::redo, true); });

        /* Per-button border + icon-halo tints. */
        const auto tintDisk    = juce::Colour::fromRGB (200, 180,  80);
        const auto tintPlugMgr = juce::Colour::fromRGB (190, 110, 170);
        const auto tintPrefs   = juce::Colour::fromRGB ( 90, 150, 210);
        const auto tintMixer   = juce::Colour::fromRGB (170, 100, 200);
        const auto tintVKbd    = juce::Colour::fromRGB ( 80, 200, 170);
        const auto tintEdit    = juce::Colour::fromRGB (200, 160,  80);
        diskOpBtn_   .setTint (tintDisk);
        pluginMgrBtn_.setTint (tintPlugMgr);
        prefsBtn_    .setTint (tintPrefs);
        graphMixBtn_ .setTint (tintMixer);
        vKbdBtn_     .setTint (tintVKbd);
        undoBtn_     .setTint (tintEdit);
        redoBtn_     .setTint (tintEdit);

        /* Active-state body wash for the view-style buttons (Disk
         * Op, Plugin Manager, Graph Mixer, Patch Bay) + VKbd toggle.
         * Full-strength tint so the active view button reads bright
         * (was 0.4 brightness which was barely distinguishable from
         * the at-rest state).  Icon foreground auto-flips to near-
         * black on light tints + white on dark tints inside
         * BlockToolButton's paint. */
        auto activeWash = [] (juce::Colour tint) { return tint; };
        diskOpBtn_   .setActiveTint (activeWash (tintDisk));
        pluginMgrBtn_.setActiveTint (activeWash (tintPlugMgr));
        graphMixBtn_ .setActiveTint (activeWash (tintMixer));
        vKbdBtn_     .setActiveTint (activeWash (tintVKbd));

        startTimerHz (8);   // refresh active-view toggle states

        vKbdBtn_.setClickingTogglesState (true);
        vKbdBtn_.setActiveTint (juce::Colour (0xff'4a'a5'5a));
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
                std::bind (&MidiPips::triggerSent,    &display_.getMidiPips())));
            connections.add (midiIOMonitor->sigReceived.connect (
                std::bind (&MidiPips::triggerReceived, &display_.getMidiPips())));
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
        /* Vertical breathing room top + bottom so the cluster bezels
         * never clip into the window-decoration / content boundary
         * above or below. */
        constexpr int kInnerPadY = 6;
        const int innerPad = kInnerPadY;
        const int rowH = juce::jmax (20, H - innerPad * 2);

        constexpr int kSidePad   = 8;
        constexpr int kGap       = 8;     /* tighter between-cluster gap */
        constexpr int kIconGap   = 4;
        constexpr int kFramePad  = 5;
        /* Wider central LCD now that the toolbar is taller -- gives
         * the BPM / position read-outs more breathing room so the
         * faceplate feels proportional to the cluster bezels around
         * it. */
        constexpr int kDisplayW  = 460;
        /* Sublabel block size -- shared with TransportBar +
         * ViewSelectorBar so every labeled cluster computes its icon
         * size the same way. */
        constexpr int kSublabelH    = 13;
        constexpr int kSublabelGap  = 3;
        /* Icon size for ALL main clusters: derived from rowH so the
         * icon + sublabel stack fills the bezel.  No cap -- the
         * labeled clusters scale up with the toolbar so they keep
         * dominant visual weight. */
        const int kMainIconBtnW = juce::jmax (16, rowH - kFramePad * 2 - kSublabelH - kSublabelGap);
        /* Trailing tools cluster stays fixed-smaller (no labels).
         * Always less than main so the labeled clusters read as
         * primary. */
        constexpr int kTrailingBtnW = 40;

        r.reduce (kSidePad, 0);
        const int top = r.getY() + innerPad;

        /* ---- LEFT cluster: Disk Op + Plugin Manager + Preferences
                inside their LCD frame.  Cluster outer width =
                kFramePad + 3*btn + 2*gap + kFramePad.  Reuses the
                outer-scope kSublabelH / kSublabelGap so all clusters
                share one sublabel block size. */
        const int leftClusterW = kFramePad * 2 + kMainIconBtnW * 3 + kIconGap * 2;
        leftClusterRect_ = Rectangle<int> (r.getX(), top, leftClusterW, rowH);
        /* Vertically centre the icon+label stack inside the frame. */
        const int stackH    = kMainIconBtnW + kSublabelGap + kSublabelH;
        const int leftBtnY  = top + (rowH - stackH) / 2;
        const int leftLblY  = leftBtnY + kMainIconBtnW + kSublabelGap;
        int lx = r.getX() + kFramePad;
        diskOpBtn_   .setBounds (lx, leftBtnY, kMainIconBtnW, kMainIconBtnW);
        diskOpLbl_   .setBounds (lx, leftLblY, kMainIconBtnW, kSublabelH);
        lx += kMainIconBtnW + kIconGap;
        pluginMgrBtn_.setBounds (lx, leftBtnY, kMainIconBtnW, kMainIconBtnW);
        pluginMgrLbl_.setBounds (lx, leftLblY, kMainIconBtnW, kSublabelH);
        lx += kMainIconBtnW + kIconGap;
        prefsBtn_    .setBounds (lx, leftBtnY, kMainIconBtnW, kMainIconBtnW);
        prefsLbl_    .setBounds (lx, leftLblY, kMainIconBtnW, kSublabelH);
        r.removeFromLeft (leftClusterW + kGap);

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

        /* ---- LCD display sits right after the transport. ---- */
        {
            const int dispW = kDisplayW;
            const int dispH = juce::jmax (24, rowH);
            display_.setBounds (r.getX(), top, dispW, dispH);
            r.removeFromLeft (dispW + kGap);
        }

        /* ---- View selector right after the display. ---- */
        if (viewSelector.isVisible())
        {
            /* 5 view buttons: Graph / Arr / Tracker / Session / Patch. */
            const int vsW = kFramePad * 2 + kMainIconBtnW * 5 + kIconGap * 4;
            viewSelector.setBounds (r.getX(), top, vsW, rowH);
            r.removeFromLeft (vsW + kGap);
        }

        /* ---- Trailing tools cluster:
                Graph Mixer | VKbd | Undo | Redo.  Kept intentionally
                smaller than the labeled main clusters so they read as
                secondary tools.  Fixed-size buttons (not rowH-derived)
                so they don't grow with the taller toolbar. */
        const int smallClusterH = kTrailingBtnW + kFramePad * 2;
        const int smallClusterW = kFramePad * 2 + kTrailingBtnW * 4 + kIconGap * 3;
        const int smallTop = top + (rowH - smallClusterH) / 2;
        postXportRect_ = Rectangle<int> (r.getX(), smallTop, smallClusterW, smallClusterH);
        const int smallBtnY = smallTop + kFramePad;
        int px = r.getX() + kFramePad;
        graphMixBtn_.setBounds (px, smallBtnY, kTrailingBtnW, kTrailingBtnW); px += kTrailingBtnW + kIconGap;
        vKbdBtn_    .setBounds (px, smallBtnY, kTrailingBtnW, kTrailingBtnW); px += kTrailingBtnW + kIconGap;
        undoBtn_    .setBounds (px, smallBtnY, kTrailingBtnW, kTrailingBtnW); px += kTrailingBtnW + kIconGap;
        redoBtn_    .setBounds (px, smallBtnY, kTrailingBtnW, kTrailingBtnW);
        r.removeFromLeft (smallClusterW + kGap);

        /* pluginMenu + mapButton stay on the far right (only visible
         * in Plugin run mode + when mapping is on, respectively). */
        if (pluginMenu.isVisible())
        {
            const int pms = rowH + 3;
            pluginMenu.setBounds (r.removeFromRight (rowH)
                                       .withSizeKeepingCentre (pms, pms));
        }
        if (mapButton.isVisible())
        {
            r.removeFromRight (4);
            mapButton.setBounds (r.removeFromRight (rowH * 2)
                                     .withSizeKeepingCentre (rowH * 2, rowH));
        }
    }

    void paint (Graphics& g) override
    {
        /* Background fill matches Content::paint so toolbar + body
         * read as one continuous frame. */
        g.setColour (Colors::backgroundColor);
        g.fillRect (getLocalBounds());

        /* LCD frames painted behind each button cluster.  Transport
         * + central display + view selector all paint their own
         * frames internally; left + post-transport clusters use the
         * shared paintLcdFrame helper here. */
        if (! leftClusterRect_.isEmpty())  paintLcdFrame (g, leftClusterRect_);
        if (! postXportRect_  .isEmpty())  paintLcdFrame (g, postXportRect_);
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
        /* Refresh view-style buttons' toggle state from their
         * commands' isTicked flag so the active surface always
         * lights up (same idiom ViewSelectorBar uses). */
        if (auto* gui = ViewHelpers::getGuiController (this))
        {
            auto& cm = gui->commands();
            auto refresh = [&cm] (BlockToolButton& b, int id)
            {
                const auto* info = cm.getCommandForID (id);
                const bool ticked = info != nullptr
                    && (info->flags & juce::ApplicationCommandInfo::isTicked) != 0;
                if (b.getToggleState() != ticked)
                    b.setToggleState (ticked, juce::dontSendNotification);
            };
            refresh (diskOpBtn_,    Commands::showDiskOp);
            refresh (pluginMgrBtn_, Commands::showPluginManager);
            refresh (graphMixBtn_,  Commands::showGraphMixer);
            refresh (vKbdBtn_,      Commands::toggleVirtualKeyboard);
        }

        /* Mapping-learn auto-clear (legacy behaviour). */
        if (auto* mapping = owner.services().find<MappingService>())
        {
            if (! mapping->isLearning() && mapButton.getToggleState())
                mapButton.setToggleState (false, dontSendNotification);
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
    Array<SignalConnection> connections;

    /* Central digital readout (Bitwig-style faceplate).  Holds BPM
     * + meter + position + elapsed + Sync/Metro/Loop/Count micro
     * toggles.  Self-timed at 15 Hz; pulls state directly from the
     * transport monitor + session. */
    MainDisplayPanel display_ { &owner.services() };

    /* Toolbar buttons in left-to-right placement order:
     *   diskOpBtn / pluginMgrBtn -- "file" view + plugin browser
     *                                view shortcuts on the far left.
     *   prefsBtn                 -- direct-invoke Preferences cog.
     *   (transport between)
     *   vKbdBtn / undoBtn / redoBtn -- post-transport cluster. */
    BlockToolButton diskOpBtn_    { "" };
    BlockToolButton pluginMgrBtn_ { "" };
    BlockToolButton prefsBtn_     { "" };
    BlockToolButton graphMixBtn_  { "" };
    BlockToolButton vKbdBtn_      { "" };
    BlockToolButton undoBtn_      { "" };
    BlockToolButton redoBtn_      { "" };

    /* LCD-style sub-labels live in ui/lcdsublabel.hpp -- shared with
     * TransportBar + ViewSelectorBar so all three clusters share one
     * visual treatment.  Will become drop-down menu triggers (spill-
     * over from the soon-to-be-removed top menubar) in a follow-up. */
    LcdSublabel diskOpLbl_    { "FILE" };
    LcdSublabel pluginMgrLbl_ { "PLUG" };
    LcdSublabel prefsLbl_     { "PREF" };

    /* LCD-frame bounds captured in resized(), painted in paint(). */
    Rectangle<int> leftClusterRect_;
    Rectangle<int> postXportRect_;

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
    /* Taller bar -- absorbs the menubar's vertical strip plus
     * additional headroom so the labeled main clusters (FILE/PLUG/
     * PREF, PLAY/STOP/REC/REW, GRAPH/ARR/TRK/SESS/PATCH) scale up to
     * comfortable icon sizes with the LCD sublabel beneath each
     * icon.  Trailing cluster (Mixer/VKbd/Undo/Redo) stays at a
     * fixed smaller size since it has no labels. */
    toolBarSize = 84;

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
