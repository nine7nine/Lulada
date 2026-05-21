// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/diskopview.hpp"
#include "services/diskopservice.hpp"
#include "ui/nativefilelist.hpp"

#include <element/services.hpp>
#include <element/context.hpp>
#include <element/plugins.hpp>
#include <element/nodefactory.hpp>
#include <element/settings.hpp>
#include <element/session.hpp>
#include <element/processor.hpp>
#include <element/node.h>
#include <element/engine.hpp>
#include <element/ui.hpp>

using namespace juce;

#include "ui/pluginmanagercomponent.hpp"  /* uses unqualified juce types */
#include "nodes/sampler.hpp"
#include "services/sessionservice.hpp"

namespace element {

namespace {

/* Palette + font deliberately mirror the tracker editor's look so this
 * page reads as part of the same family.  See trackereditor.cpp. */
const Colour kBgColour       { 0xff'18'18'18 };
const Colour kGutterColour   { 0xff'14'14'14 };
const Colour kPanelColour    { 0xff'1c'1c'1c };
const Colour kRowDividerCol  { 0xff'22'22'22 };
const Colour kOutlineColour  { 0xff'2a'2a'2a };
const Colour kAccentAmber    { 0xff'd0'80'40 }; /* same as kVelTextColour */
const Colour kAccentCyan     { 0xff'40'a0'ff };
const Colour kAccentBlue     { 0xff'5a'a5'd0 }; /* matches tracker stroke */
const Colour kAccentGreen    { 0xff'40'ff'80 };
const Colour kTextColour     { 0xff'd4'd4'd4 }; /* kNoteTextColour */
const Colour kMutedText      { 0xff'6a'6a'6a }; /* kRowTextColour */
const Colour kEmptyText      { 0xff'3a'3a'3a }; /* kEmptyCellColour */

constexpr float kFontSize       = 12.0f;
constexpr float kHeaderFontSize = 13.0f;

} // anonymous namespace


/* ===========================================================================
 * PluginPathsSection — one ListBox + Add/Remove/Up/Down/Save+Rescan per
 * format.  Reads/writes the real PluginManager settings (same persisted
 * key PluginListComponent uses) so edits here have first-class effect.
 *
 * The DiskOpService cache is hydrated from settings on first wire-up
 * and pushed back every time the user mutates the list; we don't
 * shadow-store a parallel copy.
 * ========================================================================*/
class PluginPathsSection : public Component,
                           private ListBoxModel
{
public:
    explicit PluginPathsSection (DiskOpService::PluginFormat fmt) : format_ (fmt)
    {
        title_.setText (DiskOpService::getPluginFormatName (fmt) + " paths",
                        dontSendNotification);
        title_.setColour (Label::textColourId, kTextColour);
        title_.setFont (FontOptions (
                                     kHeaderFontSize, Font::bold));
        addAndMakeVisible (title_);

        list_.setModel (this);
        list_.setRowHeight (20);
        list_.setColour (ListBox::backgroundColourId, kBgColour);
        list_.setColour (ListBox::outlineColourId,    kOutlineColour);
        list_.setOutlineThickness (1);
        addAndMakeVisible (list_);

        configureBtn (addBtn_, "Add cwd", [this] {
            const auto cwd = DiskOpService::get().getCurrentDirectory();
            auto p = paths_;
            if (cwd.isDirectory()) { p.add (cwd); commit (p); }
        });
        configureBtn (removeBtn_, "Remove", [this] {
            const int sel = list_.getSelectedRow();
            if (sel < 0 || sel >= paths_.getNumPaths()) return;
            auto p = paths_;
            p.remove (sel);
            commit (p);
        });
        configureBtn (upBtn_, "Up",   [this] { moveSelection (-1); });
        configureBtn (downBtn_, "Down", [this] { moveSelection (+1); });
        configureBtn (saveBtn_, "Save & Rescan", [this] { rescan(); });
        saveBtn_.setColour (TextButton::buttonColourId,
                            Colour { 0xff'30'4a'30 });
        saveBtn_.setColour (TextButton::textColourOffId, kAccentAmber);

        refresh();
    }

    /** Wire to real PluginManager + properties.  Called from
     *  DiskOpContentView once Services is available.  Pulls the
     *  persisted FileSearchPath for this format and pushes it both into
     *  the list and the DiskOpService cache. */
    void connect (PluginManager* pm, PropertiesFile* props)
    {
        pm_    = pm;
        props_ = props;
        paths_ = readPersisted();
        DiskOpService::get().setPluginPaths (format_, paths_);
        refresh();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        title_.setBounds (r.removeFromTop (18));
        r.removeFromTop (2);

        auto btnRow = r.removeFromBottom (22);
        addBtn_   .setBounds (btnRow.removeFromLeft (74)); btnRow.removeFromLeft (4);
        removeBtn_.setBounds (btnRow.removeFromLeft (66)); btnRow.removeFromLeft (4);
        upBtn_    .setBounds (btnRow.removeFromLeft (40)); btnRow.removeFromLeft (4);
        downBtn_  .setBounds (btnRow.removeFromLeft (40)); btnRow.removeFromLeft (8);
        saveBtn_  .setBounds (btnRow.removeFromLeft (110));
        r.removeFromBottom (4);

        list_.setBounds (r);
    }

    /* === ListBoxModel ============================================== */
    int getNumRows() override { return paths_.getNumPaths(); }
    void paintListBoxItem (int row, Graphics& g, int width, int height,
                           bool rowIsSelected) override
    {
        if (rowIsSelected)
        {
            g.setColour (kAccentBlue.withAlpha (0.3f));
            g.fillRect (0, 0, width, height);
        }
        if (row < 0 || row >= paths_.getNumPaths()) return;
        g.setColour (kTextColour);
        g.setFont (FontOptions (
                                kFontSize, Font::plain));
        g.drawText (paths_[row].getFullPathName(),
                    6, 0, width - 12, height, Justification::centredLeft);
    }

private:
    FileSearchPath readPersisted() const
    {
        if (props_ == nullptr || pm_ == nullptr) return {};
        switch (format_)
        {
            case DiskOpService::kCLAP:
                if (auto* prov = pm_->getProvider ("CLAP"))
                    return PluginListComponent::getLastSearchPath (*props_, *prov);
                break;
            case DiskOpService::kVST2:
                if (auto* fmt = pm_->getAudioPluginFormat ("VST"))
                    return PluginListComponent::getLastSearchPath (*props_, *fmt);
                break;
            case DiskOpService::kVST3:
                if (auto* fmt = pm_->getAudioPluginFormat ("VST3"))
                    return PluginListComponent::getLastSearchPath (*props_, *fmt);
                break;
            default: break;
        }
        return {};
    }

    void writePersisted (const FileSearchPath& p)
    {
        if (props_ == nullptr || pm_ == nullptr) return;
        switch (format_)
        {
            case DiskOpService::kCLAP:
                if (auto* prov = pm_->getProvider ("CLAP"))
                    PluginListComponent::setLastSearchPath (*props_, *prov, p);
                break;
            case DiskOpService::kVST2:
                if (auto* fmt = pm_->getAudioPluginFormat ("VST"))
                    PluginListComponent::setLastSearchPath (*props_, *fmt, p);
                break;
            case DiskOpService::kVST3:
                if (auto* fmt = pm_->getAudioPluginFormat ("VST3"))
                    PluginListComponent::setLastSearchPath (*props_, *fmt, p);
                break;
            default: break;
        }
    }

    void commit (const FileSearchPath& newPaths)
    {
        paths_ = newPaths;
        writePersisted (paths_);
        DiskOpService::get().setPluginPaths (format_, paths_);
        refresh();
    }

    void moveSelection (int delta)
    {
        const int sel = list_.getSelectedRow();
        if (sel < 0 || sel >= paths_.getNumPaths()) return;
        const int dst = sel + delta;
        if (dst < 0 || dst >= paths_.getNumPaths()) return;
        FileSearchPath p;
        for (int i = 0; i < paths_.getNumPaths(); ++i)
            p.add (paths_[i]);
        const File a = p[sel];
        const File b = p[dst];
        p.remove (sel);
        p.add (b, sel);
        p.remove (dst);
        p.add (a, dst);
        commit (p);
        list_.selectRow (dst);
    }

    void rescan()
    {
        if (pm_ == nullptr) return;
        const String fmtName = format_ == DiskOpService::kCLAP ? "CLAP"
                             : format_ == DiskOpService::kVST2 ? "VST"
                             : format_ == DiskOpService::kVST3 ? "VST3"
                             : String();
        if (fmtName.isEmpty()) return;
        pm_->scanAudioPlugins ({ fmtName });
    }

    void configureBtn (TextButton& b, const String& text, std::function<void()> on)
    {
        b.setButtonText (text);
        b.onClick = std::move (on);
        b.setColour (TextButton::buttonColourId, kPanelColour);
        b.setColour (TextButton::textColourOffId, kTextColour);
        addAndMakeVisible (b);
    }
    void refresh()
    {
        list_.updateContent();
        list_.repaint();
    }

    DiskOpService::PluginFormat format_;
    PluginManager*   pm_    = nullptr;
    PropertiesFile*  props_ = nullptr;
    FileSearchPath   paths_;

    Label  title_;
    ListBox list_;
    TextButton addBtn_, removeBtn_, upBtn_, downBtn_, saveBtn_;
};


/* ===========================================================================
 * SampleBankPane — Sample mode's right-side content.  Modern scrolling
 * sample-bank UI: full 128 instrument banks in one scrollable list,
 * each with 16 sample slots.  No FT2-style paging — we have the
 * vertical real estate.
 *
 *   Header — Sampler picker combo + Add Sampler / Refresh buttons
 *   Instrument list — 128 rows, "NNN | name | loaded/16"
 *   Slot list       — 16 rows of the selected bank, "NN | sample name"
 *   Footer          — Load (DiskOp selection → slot) + Fill bank (dir)
 *
 *   - Click instrument row: activate it (slot list updates).
 *   - Double-click instrument row: rename the bank (modal AlertWindow).
 *   - Double-click slot row: load the DiskOp selection into it.
 *   - Instruments are lazily allocated on first write so memory only
 *     grows when the user actually uses a bank.
 * ========================================================================*/
class SampleBankPane : public Component,
                       private Timer,
                       private ChangeListener
{
public:
    static constexpr int kNumBanks         = SamplerNode::kMaxInstruments;   /* 128 */
    static constexpr int kNumSlotsPerBank  = SamplerInstrument::kNumSlots;    /* 16  */

    SampleBankPane()
    {
        title_.setText ("Sample Bank", dontSendNotification);
        title_.setColour (Label::textColourId, kTextColour);
        title_.setFont (FontOptions (
                                     kHeaderFontSize, Font::bold));
        addAndMakeVisible (title_);

        samplerCombo_.setColour (ComboBox::backgroundColourId, kPanelColour);
        samplerCombo_.setColour (ComboBox::textColourId, kTextColour);
        samplerCombo_.onChange = [this] {
            activeSampler_ = samplerCombo_.getSelectedId() - 1;
            refreshAll();
        };
        addAndMakeVisible (samplerCombo_);

        configureBtn (addSamplerBtn_, "+ Sampler", [this] { addSamplerToGraph(); });
        configureBtn (refreshBtn_, "Refresh",     [this] { rebuildSamplerList(); });
        configureBtn (loadBtn_,    "Load to slot", [this] { loadIntoSelectedSlot(); });
        configureBtn (loadBankBtn_, "Fill bank (dir)", [this] { fillBankFromDirectory(); });

        instrumentList_.model = this;
        instrumentList_.list.setModel (&instrumentList_);
        instrumentList_.list.setRowHeight (20);
        instrumentList_.list.setColour (ListBox::backgroundColourId, kBgColour);
        instrumentList_.list.setColour (ListBox::outlineColourId,    kOutlineColour);
        instrumentList_.list.setOutlineThickness (1);
        addAndMakeVisible (instrumentList_.list);

        slotList_.model = this;
        slotList_.list.setModel (&slotList_);
        slotList_.list.setRowHeight (22);
        slotList_.list.setColour (ListBox::backgroundColourId, kBgColour);
        slotList_.list.setColour (ListBox::outlineColourId,    kOutlineColour);
        slotList_.list.setOutlineThickness (1);
        addAndMakeVisible (slotList_.list);

        instrumentLabel_.setText ("Instrument bank", dontSendNotification);
        slotsLabel_     .setText ("Slots",           dontSendNotification);
        for (auto* l : { &instrumentLabel_, &slotsLabel_ })
        {
            l->setColour (Label::textColourId, kMutedText);
            l->setFont (FontOptions (
                                     kFontSize, Font::plain));
            addAndMakeVisible (*l);
        }

        DiskOpService::get().activations.addChangeListener (this);
        startTimerHz (2);
    }

    ~SampleBankPane() override
    {
        DiskOpService::get().activations.removeChangeListener (this);
        stopTimer();
    }

    /** Activation listener — double-click in the file browser.  Skip
     *  when a one-shot DiskOpService::Request is in flight; the
     *  RequestPane handles the activation in that case. */
    void changeListenerCallback (ChangeBroadcaster*) override
    {
        if (! isShowing()) return;
        if (DiskOpService::get().hasPendingRequest()) return;
        loadIntoSelectedSlot();
    }

    void connect (Services* services)
    {
        services_ = services;
        rebuildSamplerList();
    }

    /* ----- ListBoxModel surrogates ------------------------------------ *
     * Two ListBoxes per pane; rather than multi-inherit ListBoxModel we
     * use small adapters that forward into SampleBankPane.            */
    struct InstAdapter : public ListBoxModel {
        SampleBankPane* model = nullptr;
        ListBox list;
        int getNumRows() override { return model ? kNumBanks : 0; }
        void paintListBoxItem (int row, Graphics& g, int w, int h, bool sel) override
            { if (model) model->paintInstrumentRow (row, g, w, h, sel); }
        void selectedRowsChanged (int sel) override
            { if (model) model->onInstrumentSelected (sel); }
        void listBoxItemDoubleClicked (int row, const MouseEvent&) override
            { if (model) model->renameInstrument (row); }
        void deleteKeyPressed (int lastRow) override
            { if (model) model->clearInstrumentInPlace (lastRow); }
    } instrumentList_;

    struct SlotAdapter : public ListBoxModel {
        SampleBankPane* model = nullptr;
        ListBox list;
        int getNumRows() override { return model ? kNumSlotsPerBank : 0; }
        void paintListBoxItem (int row, Graphics& g, int w, int h, bool sel) override
            { if (model) model->paintSlotRow (row, g, w, h, sel); }
        void listBoxItemDoubleClicked (int row, const MouseEvent&) override
            { if (model) { model->slotList_.list.selectRow (row);
                           model->loadIntoSelectedSlot(); } }
        void deleteKeyPressed (int lastRow) override
            { if (model) model->clearSlotInPlace (lastRow); }
    } slotList_;

    void resized() override
    {
        auto r = getLocalBounds().reduced (4);

        /* Header */
        title_.setBounds (r.removeFromTop (18));
        r.removeFromTop (4);

        auto topRow = r.removeFromTop (24);
        refreshBtn_   .setBounds (topRow.removeFromRight (70)); topRow.removeFromRight (4);
        addSamplerBtn_.setBounds (topRow.removeFromRight (84)); topRow.removeFromRight (4);
        samplerCombo_.setBounds (topRow);
        r.removeFromTop (6);

        /* Instrument list label */
        instrumentLabel_.setBounds (r.removeFromTop (16));
        r.removeFromTop (2);

        /* Instrument list — full 128 rows scrollable, no paging. */
        const int instH = juce::jmax (200, r.getHeight() * 6 / 10);
        instrumentList_.list.setBounds (r.removeFromTop (instH));
        r.removeFromTop (8);

        /* Slot list */
        slotsLabel_.setBounds (r.removeFromTop (16));
        r.removeFromTop (2);
        auto footer = r.removeFromBottom (24);
        loadBtn_    .setBounds (footer.removeFromLeft (110)); footer.removeFromLeft (6);
        loadBankBtn_.setBounds (footer.removeFromLeft (130));
        r.removeFromBottom (4);
        slotList_.list.setBounds (r);
    }

    /* ----- row painting ---------------------------------------------- */
    void paintInstrumentRow (int row, Graphics& g, int w, int h, bool sel)
    {
        if (sel) { g.setColour (kAccentBlue.withAlpha (0.3f)); g.fillRect (0, 0, w, h); }

        auto inst = getInstrumentRaw (row);
        const int loaded = inst ? inst->numLoaded() : 0;
        const bool used  = (inst && (loaded > 0 || inst->name.isNotEmpty()));

        g.setFont (FontOptions (
                                kFontSize, Font::plain));
        g.setColour (used ? kTextColour : kMutedText);
        g.drawText (String::formatted ("%03d", row + 1),
                    6, 0, 36, h, Justification::centredLeft);

        g.setColour (used ? kAccentAmber : kEmptyText);
        const String name = (inst && inst->name.isNotEmpty()) ? inst->name
                          : String (CharPointer_UTF8 ("\xe2\x80\x94"));
        g.drawText (name, 50, 0, w - 110, h, Justification::centredLeft);

        if (inst)
        {
            g.setColour (loaded > 0 ? kAccentGreen : kMutedText);
            g.drawText (String::formatted ("%d/%d", loaded, kNumSlotsPerBank),
                        w - 52, 0, 46, h, Justification::centredRight);
        }
    }

    void paintSlotRow (int row, Graphics& g, int w, int h, bool sel)
    {
        if (sel) { g.setColour (kAccentBlue.withAlpha (0.3f)); g.fillRect (0, 0, w, h); }

        auto inst = getInstrumentRaw (activeInstrument_);
        const auto* slot = inst ? inst->getSlot (row) : nullptr;

        g.setFont (FontOptions (
                                kFontSize, Font::plain));
        g.setColour (slot ? kTextColour : kEmptyText);
        g.drawText (String::formatted ("%02d", row + 1),
                    6, 0, 30, h, Justification::centredLeft);
        g.setColour (slot ? kAccentAmber : kEmptyText);
        g.drawText (slot ? slot->name
                         : String (CharPointer_UTF8 ("\xe2\x80\x94")),
                    40, 0, w - 46, h, Justification::centredLeft);
    }

    void onInstrumentSelected (int row)
    {
        if (row < 0) return;
        activeInstrument_ = row;
        slotList_.list.updateContent();
        slotList_.list.repaint();
    }

    void renameInstrument (int row)
    {
        auto* sn = getSamplerProcessor (activeSampler_);
        if (sn == nullptr) return;
        ensureInstrumentExists (sn, row);
        auto inst = sn->getInstrument (row);
        if (inst == nullptr) return;

        auto* editor = new AlertWindow ("Rename instrument bank",
            "Name for bank " + String (row + 1), AlertWindow::NoIcon);
        editor->addTextEditor ("name", inst->name);
        editor->addButton ("OK",     1, KeyPress (KeyPress::returnKey));
        editor->addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));

        /* SafePointer guards against the SampleBankPane (and its
         * surrounding DiskOpContentView) being destroyed while the
         * modal is still open — closing Disk Op or quitting the app
         * before the rename is confirmed would otherwise leave the
         * lambda's `this` capture dangling. */
        Component::SafePointer<SampleBankPane> safeThis (this);
        editor->enterModalState (true,
            ModalCallbackFunction::create ([row, editor, safeThis] (int r) mutable {
                std::unique_ptr<AlertWindow> ownEditor (editor);   /* always delete */
                if (! safeThis) return;                              /* we died — bail */
                if (r != 1)    return;
                if (auto* sn2 = safeThis->getSamplerProcessor (safeThis->activeSampler_))
                    if (auto inst2 = sn2->getInstrument (row))
                        inst2->name = ownEditor->getTextEditorContents ("name");
                safeThis->instrumentList_.list.repaint();
            }), false);
    }

private:
    void timerCallback() override
    {
        /* Skip work entirely when the pane isn't visible — the Disk Op
         * page only renders this in Sample mode.  Saves a graph walk
         * + ListBox repaint pair every 500ms while the user is in
         * Plugin Paths / Session mode or has the whole Disk Op view
         * hidden behind another nav page. */
        if (! isShowing()) return;

        const int curCount = countSamplerNodes();
        if (curCount != lastSamplerCount_) rebuildSamplerList();

        /* Cache invalidation only when graph state actually shifts.
         * Idle-tick paint cost goes from ~128 graph walks (one per
         * row's paintListBoxItem) to 0. */
        cachedNode_ = nullptr;
        instrumentList_.list.repaint();
        slotList_.list.repaint();
    }

    void configureBtn (TextButton& b, const String& text,
                       std::function<void()> on)
    {
        b.setButtonText (text);
        b.onClick = std::move (on);
        b.setColour (TextButton::buttonColourId, kPanelColour);
        b.setColour (TextButton::textColourOffId, kTextColour);
        addAndMakeVisible (b);
    }

    SamplerNode* getSamplerProcessor (int index) const
    {
        /* Cached for the duration of one Timer-tick burst of paints —
         * walking the active graph hundreds of times per frame chokes
         * winelib wineserver round-trips.  The cache is invalidated in
         * timerCallback when sampler count changes, and once per 500ms
         * tick anyway to pick up other graph mutations. */
        if (cachedNode_ && cachedSamplerIdx_ == index)
            return cachedNode_;

        if (services_ == nullptr) return nullptr;
        const auto session = services_->context().session();
        if (session == nullptr) return nullptr;
        const Node graph = session->getActiveGraph();
        if (! graph.isValid()) return nullptr;

        int hit = -1;
        for (int i = 0; i < graph.getNumNodes(); ++i)
        {
            const Node n = graph.getNode (i);
            const auto id = n.getFileOrIdentifier().toString();
            if (id != EL_NODE_ID_SAMPLER) continue;
            ++hit;
            if (hit != index) continue;
            if (auto* obj = n.getObject())
                if (auto* sp = dynamic_cast<SamplerNode*> (obj->getAudioProcessor()))
                {
                    cachedNode_ = sp;
                    cachedSamplerIdx_ = index;
                    return sp;
                }
        }
        return nullptr;
    }

    int countSamplerNodes() const
    {
        if (services_ == nullptr) return 0;
        const auto session = services_->context().session();
        if (session == nullptr) return 0;
        const Node graph = session->getActiveGraph();
        if (! graph.isValid()) return 0;
        int c = 0;
        for (int i = 0; i < graph.getNumNodes(); ++i)
            if (graph.getNode (i).getFileOrIdentifier().toString() == EL_NODE_ID_SAMPLER)
                ++c;
        return c;
    }

    SamplerInstrument::Ptr getInstrumentRaw (int row) const
    {
        auto* sn = getSamplerProcessor (activeSampler_);
        if (sn == nullptr) return nullptr;
        return sn->getInstrument (row);
    }

    /** Lazy allocation — bring instruments[size..targetIndex] into
     *  existence on first write to a row beyond the current count. */
    void ensureInstrumentExists (SamplerNode* sn, int targetIndex)
    {
        if (sn == nullptr) return;
        while (sn->getNumInstruments() <= targetIndex
               && sn->getNumInstruments() < kNumBanks)
            sn->addInstrument();
    }

    void rebuildSamplerList()
    {
        samplerCombo_.clear (dontSendNotification);
        const int n = countSamplerNodes();
        lastSamplerCount_ = n;

        for (int i = 0; i < n; ++i)
            samplerCombo_.addItem ("Sampler " + String (i + 1), i + 1);

        if (n == 0)
        {
            samplerCombo_.addItem ("(no Sampler in graph)", 1);
            samplerCombo_.setSelectedId (1, dontSendNotification);
            samplerCombo_.setEnabled (false);
        }
        else
        {
            samplerCombo_.setEnabled (true);
            if (activeSampler_ >= n) activeSampler_ = 0;
            samplerCombo_.setSelectedId (activeSampler_ + 1, dontSendNotification);
        }
        refreshAll();
    }

    void refreshAll()
    {
        instrumentList_.list.updateContent();
        instrumentList_.list.repaint();
        slotList_.list.updateContent();
        slotList_.list.repaint();
    }

    void addSamplerToGraph()
    {
        if (services_ == nullptr) return;
        auto* eng = services_->find<EngineService>();
        if (eng == nullptr) return;
        eng->addNode (EL_NODE_ID_SAMPLER, EL_NODE_FORMAT_NAME);
        /* EngineService::addNode is synchronous — invalidate the cached
         * SamplerNode pointer so the next getSamplerProcessor walks the
         * graph fresh and picks up the new node. */
        cachedNode_ = nullptr;
        cachedSamplerIdx_ = -1;
        rebuildSamplerList();
    }

    /** Pick a slot to load into when the user hasn't explicitly
     *  selected one in the slot list.  Prefer the first empty slot,
     *  fall back to slot 0. */
    int pickLoadSlot (SamplerNode* sn)
    {
        const int sel = slotList_.list.getSelectedRow();
        if (sel >= 0) return sel;
        if (sn != nullptr)
        {
            if (auto inst = sn->getInstrument (activeInstrument_))
                for (int i = 0; i < SamplerInstrument::kNumSlots; ++i)
                    if (inst->getSlot (i) == nullptr) return i;
        }
        return 0;
    }

    void loadIntoSelectedSlot()
    {
        const auto sel = DiskOpService::get().getSelectedFile();
        if (! sel.existsAsFile()) return;

        /* Auto-create a Sampler if the graph doesn't have one yet —
         * user expects "load a sample" to Just Work, not silently
         * no-op because + Sampler was never pressed. */
        auto* sn = getSamplerProcessor (activeSampler_);
        if (sn == nullptr)
        {
            addSamplerToGraph();
            sn = getSamplerProcessor (activeSampler_);
            if (sn == nullptr) return;   /* genuinely couldn't add */
        }

        const int slotRow = pickLoadSlot (sn);
        ensureInstrumentExists (sn, activeInstrument_);
        sn->loadSampleToSlot (activeInstrument_, slotRow, sel);
        /* Move the slot-list cursor to the slot we just wrote so the
         * user sees the result + the next Load goes to the slot after
         * (FT2-ish auto-advance). */
        slotList_.list.selectRow (slotRow);
        instrumentList_.list.repaint();
        slotList_.list.repaint();
    }

    /** Delete-key handler on the slot list.  Empties the selected slot
     *  without disturbing other slots in the bank. */
    void clearSlotInPlace (int row)
    {
        if (row < 0 || row >= kNumSlotsPerBank) return;
        auto* sn = getSamplerProcessor (activeSampler_);
        if (sn == nullptr) return;
        auto inst = sn->getInstrument (activeInstrument_);
        if (inst == nullptr) return;
        inst->clearSlot (row);
        instrumentList_.list.repaint();
        slotList_.list.repaint();
    }

    /** Delete-key handler on the instrument list.  Wipes the bank's 32
     *  sample slots + clears the user-set name.  Uses
     *  SamplerInstrument::clear() rather than SamplerNode::
     *  removeInstrument so the 128-row table indices stay stable. */
    void clearInstrumentInPlace (int row)
    {
        if (row < 0 || row >= kNumBanks) return;
        auto* sn = getSamplerProcessor (activeSampler_);
        if (sn == nullptr) return;
        auto inst = sn->getInstrument (row);
        if (inst == nullptr) return;        /* row past lazy alloc — nothing to clear */
        inst->clear();
        instrumentList_.list.repaint();
        if (row == activeInstrument_)
        {
            slotList_.list.updateContent();
            slotList_.list.repaint();
        }
    }

    /** Fill the active bank's 16 slots with audio files from the
     *  DiskOp's current directory.  Picks the first 16 audio files in
     *  alphabetical order, matching the active mode's wildcard. */
    void fillBankFromDirectory()
    {
        const auto dir = DiskOpService::get().getCurrentDirectory();
        if (! dir.isDirectory()) return;

        auto* sn = getSamplerProcessor (activeSampler_);
        if (sn == nullptr)
        {
            addSamplerToGraph();
            sn = getSamplerProcessor (activeSampler_);
            if (sn == nullptr) return;
        }

        WildcardFileFilter filter (
            DiskOpService::getWildcardForMode (DiskOpService::Mode::kSample),
            "*", "audio");
        Array<File> files;
        for (const auto& entry : RangedDirectoryIterator (
                 dir, false, "*", File::findFiles))
        {
            const auto f = entry.getFile();
            if (filter.isFileSuitable (f)) files.add (f);
        }
        std::sort (files.begin(), files.end(),
                   [] (const File& a, const File& b) {
                       return a.getFileName().compareIgnoreCase (b.getFileName()) < 0;
                   });

        ensureInstrumentExists (sn, activeInstrument_);
        const int n = juce::jmin (kNumSlotsPerBank, files.size());
        for (int i = 0; i < n; ++i)
            sn->loadSampleToSlot (activeInstrument_, i, files[i]);

        refreshAll();
    }

    Services* services_ = nullptr;
    int activeSampler_    = 0;
    int activeInstrument_ = 0;
    int lastSamplerCount_ = -1;
    mutable SamplerNode* cachedNode_ = nullptr;
    mutable int          cachedSamplerIdx_ = -1;

    Label title_, instrumentLabel_, slotsLabel_;
    ComboBox samplerCombo_;
    TextButton addSamplerBtn_, refreshBtn_, loadBtn_, loadBankBtn_;
};


/* ===========================================================================
 * SessionPane — Session mode's right-side content.  New / Open / Save /
 * Save-As + recents list.  Replaces the placeholder text the Disk Op
 * page used to show in Session mode.
 *
 * Design intent: the DiskOp IS the chooser.  No juce::FileChooser
 * ever surfaces from this pane — Open consumes the file browser's
 * current selection (or the activation signal from double-click),
 * Save uses the current document path, Save As uses the typed
 * filename + current directory.
 * ========================================================================*/
class SessionPane : public Component,
                    private Timer,
                    private ChangeListener
{
public:
    SessionPane()
    {
        title_.setText ("Session", dontSendNotification);
        title_.setColour (Label::textColourId, kTextColour);
        title_.setFont (FontOptions (
                                     kHeaderFontSize, Font::bold));
        addAndMakeVisible (title_);

        currentLabel_.setColour (Label::textColourId, kMutedText);
        currentLabel_.setFont (FontOptions ( kFontSize, Font::plain));
        currentLabel_.setText ("Current: (none)", dontSendNotification);
        addAndMakeVisible (currentLabel_);

        configureBtn (newBtn_,    "New",     [this] { newSession(); });
        configureBtn (openBtn_,   "Open",    [this] { openSelected(); });
        configureBtn (saveBtn_,   "Save",    [this] { saveSession(); });
        configureBtn (saveAsBtn_, "Save As", [this] { saveAsTypedFilename(); });

        recentsLabel_.setText ("Recent sessions", dontSendNotification);
        recentsLabel_.setColour (Label::textColourId, kMutedText);
        recentsLabel_.setFont (FontOptions ( kFontSize, Font::plain));
        addAndMakeVisible (recentsLabel_);

        recents_.model = this;
        recents_.list.setModel (&recents_);
        recents_.list.setRowHeight (20);
        recents_.list.setColour (ListBox::backgroundColourId, kBgColour);
        recents_.list.setColour (ListBox::outlineColourId,    kOutlineColour);
        recents_.list.setOutlineThickness (1);
        addAndMakeVisible (recents_.list);

        DiskOpService::get().activations.addChangeListener (this);
        startTimerHz (2);
    }

    ~SessionPane() override
    {
        DiskOpService::get().activations.removeChangeListener (this);
        stopTimer();
    }

    void connect (Services* services)
    {
        services_ = services;
        refreshRecents();
        refreshCurrent();
    }

    /** File-browser double-click → if Session mode and the activated
     *  file is an .els, treat as Open.  Skip when a one-shot request
     *  is in flight (RequestPane handles activation). */
    void changeListenerCallback (ChangeBroadcaster*) override
    {
        if (! isShowing()) return;
        if (DiskOpService::get().hasPendingRequest()) return;
        const auto sel = DiskOpService::get().getSelectedFile();
        if (sel.hasFileExtension ("els"))
            openFile (sel);
    }

    struct RecentsAdapter : public ListBoxModel {
        SessionPane* model = nullptr;
        ListBox list;
        int getNumRows() override { return model ? model->recentFiles_.size() : 0; }
        void paintListBoxItem (int row, Graphics& g, int w, int h, bool sel) override
            { if (model) model->paintRecentRow (row, g, w, h, sel); }
        void listBoxItemDoubleClicked (int row, const MouseEvent&) override
            { if (model && row >= 0 && row < model->recentFiles_.size())
                  model->openFile (model->recentFiles_[row]); }
    } recents_;

    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        title_.setBounds (r.removeFromTop (18));
        r.removeFromTop (4);

        currentLabel_.setBounds (r.removeFromTop (18));
        r.removeFromTop (8);

        auto btnRow = r.removeFromTop (26);
        newBtn_   .setBounds (btnRow.removeFromLeft (62)); btnRow.removeFromLeft (4);
        openBtn_  .setBounds (btnRow.removeFromLeft (62)); btnRow.removeFromLeft (4);
        saveBtn_  .setBounds (btnRow.removeFromLeft (62)); btnRow.removeFromLeft (4);
        saveAsBtn_.setBounds (btnRow.removeFromLeft (82));
        r.removeFromTop (10);

        recentsLabel_.setBounds (r.removeFromTop (16));
        r.removeFromTop (2);
        recents_.list.setBounds (r);
    }

    void paintRecentRow (int row, Graphics& g, int w, int h, bool sel)
    {
        if (sel) { g.setColour (kAccentBlue.withAlpha (0.3f)); g.fillRect (0, 0, w, h); }
        if (row < 0 || row >= recentFiles_.size()) return;
        const auto f = recentFiles_[row];
        g.setColour (kTextColour);
        g.setFont (FontOptions ( kFontSize, Font::plain));
        g.drawText (f.getFileNameWithoutExtension(),
                    6, 0, w - 12, h, Justification::centredLeft);
    }

private:
    SessionService* session() const
    {
        return services_ ? services_->find<SessionService>() : nullptr;
    }

    UI* ui() const
    {
        return services_ ? services_->find<UI>() : nullptr;
    }

    void timerCallback() override
    {
        if (! isShowing()) return;
        /* Cheap — recent-files list + the current-file label are small.
         * Both can change out from under us (Open from menu, Save from
         * elsewhere) so re-poll at 2Hz with a content-diff cutoff. */
        const int sizeBefore = recentFiles_.size();
        refreshRecents();
        refreshCurrent();
        if (recentFiles_.size() != sizeBefore)
            recents_.list.updateContent();
    }

    void refreshRecents()
    {
        recentFiles_.clearQuick();
        if (auto* u = ui())
        {
            auto& list = u->recentFiles();
            for (int i = 0; i < list.getNumFiles(); ++i)
                recentFiles_.add (list.getFile (i));
        }
        recents_.list.repaint();
    }

    void refreshCurrent()
    {
        const auto cur = currentSessionFile();
        const String text = cur.existsAsFile()
                              ? ("Current: " + cur.getFullPathName())
                              : String ("Current: (unsaved)");
        if (currentLabel_.getText() != text)
            currentLabel_.setText (text, dontSendNotification);
    }

    File currentSessionFile() const
    {
        if (auto* s = session()) return s->getSessionFile();
        return {};
    }

    void openSelected()
    {
        /* Pick the file the browser column has selected, or the
         * filename field's typed path resolved against cwd. */
        File f = DiskOpService::get().getSelectedFile();
        if (! f.existsAsFile())
        {
            const auto typed = DiskOpService::get().getFilename();
            if (typed.isNotEmpty())
                f = DiskOpService::get().getCurrentDirectory().getChildFile (typed);
        }
        openFile (f);
    }

    void openFile (const File& f)
    {
        if (! f.existsAsFile()) return;
        if (! f.hasFileExtension ("els")) return;
        if (auto* s = session())
        {
            s->openFile (f);
            refreshCurrent();
            refreshRecents();
            recents_.list.updateContent();
        }
    }

    void newSession()
    {
        if (auto* s = session())
        {
            s->newSession();
            refreshCurrent();
        }
    }

    void saveSession()
    {
        if (auto* s = session())
        {
            const auto cur = s->getSessionFile();
            if (cur.existsAsFile())
            {
                /* Save to existing file — silent, no chooser. */
                s->saveSession (false, false, true);
            }
            else
            {
                /* No prior file — fall through to Save As path so the
                 * user doesn't quietly create a new session somewhere
                 * unexpected via FileBasedDocument's interactive save. */
                saveAsTypedFilename();
            }
            refreshCurrent();
            refreshRecents();
        }
    }

    void saveAsTypedFilename()
    {
        auto* s = session();
        if (s == nullptr) return;

        const auto dir = DiskOpService::get().getCurrentDirectory();
        if (! dir.isDirectory()) return;

        String typed = DiskOpService::get().getFilename().trim();
        if (typed.isEmpty())
        {
            /* Use the current session's filename if any, else a
             * timestamped fallback so the save never silently dumps
             * into "Untitled.els" overwriting. */
            const auto cur = s->getSessionFile();
            typed = cur.existsAsFile()
                      ? cur.getFileName()
                      : ("session-" + Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S") + ".els");
        }

        if (! typed.endsWithIgnoreCase (".els"))
            typed = typed + ".els";

        File target = dir.getChildFile (typed);
        if (s->saveSessionTo (target))
        {
            refreshCurrent();
            refreshRecents();
            recents_.list.updateContent();
        }
    }

    void configureBtn (TextButton& b, const String& text,
                       std::function<void()> on)
    {
        b.setButtonText (text);
        b.onClick = std::move (on);
        b.setColour (TextButton::buttonColourId, kPanelColour);
        b.setColour (TextButton::textColourOffId, kTextColour);
        addAndMakeVisible (b);
    }

    Services* services_ = nullptr;
    Label title_, currentLabel_, recentsLabel_;
    TextButton newBtn_, openBtn_, saveBtn_, saveAsBtn_;
    Array<File> recentFiles_;
};


/* ===========================================================================
 * RequestPane — overlay that owns the right pane while a one-shot
 * DiskOpService::Request is armed (Open / Save dialog replacement).
 * Shown in place of the SampleBank / Session / PluginPaths pane until
 * the user clicks Confirm or Cancel, or the request is completed via
 * file-browser activation (double-click).
 * ========================================================================*/
class RequestPane : public Component,
                    private ChangeListener
{
public:
    RequestPane()
    {
        title_.setColour (Label::textColourId, kAccentAmber);
        title_.setFont (FontOptions (
                                     kHeaderFontSize, Font::bold));
        title_.setJustificationType (Justification::centredLeft);
        addAndMakeVisible (title_);

        prompt_.setColour (Label::textColourId, kMutedText);
        prompt_.setFont (FontOptions ( kFontSize, Font::plain));
        addAndMakeVisible (prompt_);

        configureBtn (confirmBtn_, "Confirm", [this] { onConfirm(); });
        confirmBtn_.setColour (TextButton::buttonColourId, Colour { 0xff'30'4a'30 });
        confirmBtn_.setColour (TextButton::textColourOffId, kAccentAmber);

        configureBtn (cancelBtn_, "Cancel", [this] { onCancel(); });

        DiskOpService::get().activations.addChangeListener (this);
        sync();
    }

    ~RequestPane() override
    {
        DiskOpService::get().activations.removeChangeListener (this);
    }

    /** Re-sync when the service signals a change (request armed / dirty
     *  filename / selection updated). */
    void sync()
    {
        const auto* req = DiskOpService::get().getPendingRequest();
        if (req == nullptr) return;

        title_.setText (req->title, dontSendNotification);
        prompt_.setText (req->isSave
                           ? String ("Type a filename and press Confirm.")
                           : String ("Pick a file from the browser and press Confirm,\n"
                                     "or double-click the file directly."),
                         dontSendNotification);
    }

    /** Activation broadcaster — for open requests, treat double-click
     *  as Confirm.  Save requests ignore activations (just navigation). */
    void changeListenerCallback (ChangeBroadcaster*) override
    {
        const auto* req = DiskOpService::get().getPendingRequest();
        if (req == nullptr) return;
        if (req->isSave)   return;
        const auto sel = DiskOpService::get().getSelectedFile();
        if (sel.existsAsFile())
            DiskOpService::get().completeRequest (sel);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        title_ .setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        prompt_.setBounds (r.removeFromTop (44));
        r.removeFromTop (10);

        auto btnRow = r.removeFromTop (28);
        confirmBtn_.setBounds (btnRow.removeFromLeft (84)); btnRow.removeFromLeft (6);
        cancelBtn_ .setBounds (btnRow.removeFromLeft (84));
    }

private:
    void onConfirm()
    {
        const auto* req = DiskOpService::get().getPendingRequest();
        if (req == nullptr) return;

        File chosen;
        if (req->isSave)
        {
            const auto dir = DiskOpService::get().getCurrentDirectory();
            String name = DiskOpService::get().getFilename().trim();
            if (! dir.isDirectory()) return;
            if (name.isEmpty()) name = req->initialFilename;
            if (name.isEmpty()) return;

            /* Normalize extension against the request's wildcard
             * (e.g. *.els → ".els").  If the wildcard is "*" or
             * compound, leave the user's input alone. */
            const auto ext = wildcardToExtension (req->wildcard);
            if (ext.isNotEmpty() && ! name.endsWithIgnoreCase (ext))
                name = name + ext;

            chosen = dir.getChildFile (name);
        }
        else
        {
            chosen = DiskOpService::get().getSelectedFile();
            if (! chosen.existsAsFile())
            {
                /* Fall back to typed-filename path under cwd. */
                const auto typed = DiskOpService::get().getFilename().trim();
                if (typed.isNotEmpty())
                    chosen = DiskOpService::get().getCurrentDirectory().getChildFile (typed);
            }
            if (! chosen.existsAsFile()) return;
        }

        DiskOpService::get().completeRequest (chosen);
    }

    void onCancel() { DiskOpService::get().cancelRequest(); }

    static String wildcardToExtension (const String& wildcard)
    {
        if (wildcard.isEmpty()) return {};
        if (wildcard == "*")    return {};
        /* "*.els"  → ".els";  "*.els;*.xml" → "" (ambiguous). */
        if (wildcard.contains (";")) return {};
        const int dot = wildcard.indexOf (".");
        if (dot < 0) return {};
        return wildcard.substring (dot);
    }

    void configureBtn (TextButton& b, const String& text, std::function<void()> on)
    {
        b.setButtonText (text);
        b.onClick = std::move (on);
        b.setColour (TextButton::buttonColourId, kPanelColour);
        b.setColour (TextButton::textColourOffId, kTextColour);
        addAndMakeVisible (b);
    }

    Label title_, prompt_;
    TextButton confirmBtn_, cancelBtn_;
};


/* ===========================================================================
 * DiskOpContentView::Impl
 * ========================================================================*/
class DiskOpContentView::Impl : public Component,
                                public NativeFileListComponent::Listener
{
public:
    explicit Impl (DiskOpContentView& owner) : owner_ (owner),
          browser_ (dirCache_)
    {
        addAndMakeVisible (browser_);
        browser_.addListener (this);
        for (int i = 0; i < (int) DiskOpService::Mode::kNumModes; ++i)
        {
            auto btn = std::make_unique<ToggleButton> (
                DiskOpService::getModeName ((DiskOpService::Mode) i));
            btn->setRadioGroupId (1);
            btn->setColour (ToggleButton::textColourId, kTextColour);
            btn->onClick = [this, i] {
                DiskOpService::get().setMode ((DiskOpService::Mode) i);
            };
            addAndMakeVisible (*btn);
            modeButtons_.add (std::move (btn));
        }

        configureLabel (modeLabel_, "Item:");
        configureLabel (pathLabel_, "/");
        configureLabel (statusLabel_, "");

        pathEdit_.setMultiLine (false);
        pathEdit_.setReturnKeyStartsNewLine (false);
        pathEdit_.setColour (TextEditor::backgroundColourId, kPanelColour);
        pathEdit_.setColour (TextEditor::textColourId, kTextColour);
        pathEdit_.onReturnKey = [this] {
            const File f (pathEdit_.getText().trim());
            if (f.isDirectory())
                DiskOpService::get().setCurrentDirectory (f);
            else if (f.existsAsFile())
                DiskOpService::get().setSelectedFile (f);
        };
        addAndMakeVisible (pathEdit_);

        filenameEdit_.setMultiLine (false);
        filenameEdit_.setTextToShowWhenEmpty ("filename.ext", kMutedText);
        filenameEdit_.setColour (TextEditor::backgroundColourId, kPanelColour);
        filenameEdit_.setColour (TextEditor::textColourId, kTextColour);
        filenameEdit_.onTextChange = [this] {
            DiskOpService::get().setFilename (filenameEdit_.getText());
        };
        addAndMakeVisible (filenameEdit_);

        applyWildcard();
        if (DiskOpService::get().getCurrentDirectory().isDirectory())
            browser_.setRoot (DiskOpService::get().getCurrentDirectory());

        configureActionButton (refreshBtn_, "Refresh", [this] { browser_.refresh(); });
        configureActionButton (setPathBtn_, "Set path", [this] {
            DiskOpService::get().setCurrentDirectory (File (pathEdit_.getText().trim()));
        });
        configureActionButton (homeBtn_, "Home", [this] {
            DiskOpService::get().setCurrentDirectory (
                File::getSpecialLocation (File::userHomeDirectory));
        });
        configureActionButton (rootBtn_, "/", [this] {
            DiskOpService::get().setCurrentDirectory (File ("/"));
        });

        rebuildWineDriveButtons();

        configureLabel (allFilesLabel_, "All files");
        allFilesToggle_.setColour (ToggleButton::textColourId, kTextColour);
        allFilesToggle_.onClick = [this] {
            allFiles_ = allFilesToggle_.getToggleState();
            applyWildcard();
        };
        addAndMakeVisible (allFilesToggle_);

        configureLabel (modeBadge_, "");

        /* Plugin Paths page sections — created upfront, visibility
         * toggled by mode in syncFromService(). */
        for (int i = 0; i < (int) DiskOpService::kNumPluginFormats; ++i)
        {
            auto sec = std::make_unique<PluginPathsSection> (
                (DiskOpService::PluginFormat) i);
            addChildComponent (*sec);   /* hidden by default */
            pluginPathsSections_.add (std::move (sec));
        }

        /* Sample-bank pane — visible in Sample mode. */
        sampleBank_ = std::make_unique<SampleBankPane>();
        addChildComponent (*sampleBank_);

        /* Session pane — visible in Session mode. */
        sessionPane_ = std::make_unique<SessionPane>();
        addChildComponent (*sessionPane_);

        /* Request overlay — visible only when DiskOpService has a
         * one-shot Open/Save request armed. */
        requestPane_ = std::make_unique<RequestPane>();
        addChildComponent (*requestPane_);

        syncFromService();
    }

    /** Connect to real Services-backed APIs (PluginManager paths,
     *  graph traversal for the sample bank). */
    void wireServices (Services& services)
    {
        services_ = &services;
        auto& pm  = services.context().plugins();
        auto* propsFile = services.context().settings().getUserSettings();
        for (auto& s : pluginPathsSections_)
            s->connect (&pm, propsFile);
        if (sampleBank_)  sampleBank_->connect (&services);
        if (sessionPane_) sessionPane_->connect (&services);
    }

    ~Impl() override { browser_.removeListener (this); }

    void paint (Graphics& g) override
    {
        g.fillAll (kBgColour);

        g.setColour (kPanelColour);
        g.fillRect (sidebarBounds_);
        g.setColour (kOutlineColour);
        g.drawRect (sidebarBounds_, 1);

        g.setColour (kPanelColour);
        g.fillRect (toolbarBounds_);
        g.setColour (kOutlineColour);
        g.drawRect (toolbarBounds_, 1);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12);

        /* === Column 1 — Sidebar: mode radio + Wine drive quick-nav. === */
        sidebarBounds_ = r.removeFromLeft (170);
        auto sb = sidebarBounds_.reduced (10);
        modeLabel_.setBounds (sb.removeFromTop (18));
        sb.removeFromTop (2);
        for (auto& b : modeButtons_)
        {
            b->setBounds (sb.removeFromTop (22));
            sb.removeFromTop (2);
        }
        sb.removeFromTop (10);

        modeBadge_.setBounds (sb.removeFromTop (18));
        sb.removeFromTop (6);

        for (auto& b : wineBtns_)
        {
            b->setBounds (sb.removeFromTop (22));
            sb.removeFromTop (2);
        }
        sb.removeFromTop (6);
        homeBtn_.setBounds (sb.removeFromTop (22));  sb.removeFromTop (2);
        rootBtn_.setBounds (sb.removeFromTop (22));

        r.removeFromLeft (8);

        /* === Column 2 — File browser column.  50/50 split with the
         * extras column.  Holds toolbar / path / filename /
         * FileBrowserComponent. === */
        const int browserColW = juce::jmax (0, (r.getWidth() - 10) / 2);
        auto browserCol = r.removeFromLeft (browserColW);

        toolbarBounds_ = browserCol.removeFromTop (36);
        auto tb = toolbarBounds_.reduced (6);
        refreshBtn_.setBounds (tb.removeFromLeft (70)); tb.removeFromLeft (4);
        setPathBtn_.setBounds (tb.removeFromLeft (70)); tb.removeFromLeft (8);
        allFilesToggle_.setBounds (tb.removeFromLeft (90));
        tb.removeFromLeft (8);
        statusLabel_.setBounds (tb);

        browserCol.removeFromTop (6);
        auto pathRow = browserCol.removeFromTop (24);
        pathLabel_.setBounds (pathRow.removeFromLeft (40));
        pathEdit_.setBounds (pathRow);

        browserCol.removeFromTop (4);
        auto fileRow = browserCol.removeFromTop (24);
        filenameLabelArea_ = fileRow.removeFromLeft (40);
        filenameEdit_.setBounds (fileRow);

        browserCol.removeFromTop (6);
        browser_.setBounds (browserCol);

        r.removeFromLeft (10);

        /* === Column 3 — Mode-extras pane.  Mirrors FT2's right-side
         * "extras" (patterns, sample slots).  Content swaps per mode:
         *   Sample        — sampler-slot mirror (placeholder for now)
         *   Session       — recent-sessions placeholder
         *   Plugin Paths  — three stacked CLAP / VST / VST3 sections. === */
        extrasBounds_ = r;

        auto& svc = DiskOpService::get();
        const auto mode = svc.getMode();
        const bool pluginPaths = mode == DiskOpService::Mode::kPluginPaths;
        const bool sampleMode  = mode == DiskOpService::Mode::kSample;
        const bool sessionMode = mode == DiskOpService::Mode::kSession;
        const bool requestActive = svc.hasPendingRequest();

        /* When a one-shot Open/Save request is active, the right pane is
         * owned by RequestPane — every mode pane hides. */
        for (auto& s : pluginPathsSections_) s->setVisible (pluginPaths && ! requestActive);
        if (sampleBank_)  sampleBank_ ->setVisible (sampleMode  && ! requestActive);
        if (sessionPane_) sessionPane_->setVisible (sessionMode && ! requestActive);
        if (requestPane_) requestPane_->setVisible (requestActive);

        if (requestActive && requestPane_)
        {
            requestPane_->setBounds (r);
            requestPane_->sync();
        }
        else if (pluginPaths && ! pluginPathsSections_.isEmpty())
        {
            auto col = r;
            const int h = juce::jmax (60, col.getHeight() / 3);
            pluginPathsSections_[0]->setBounds (col.removeFromTop (h));
            col.removeFromTop (4);
            pluginPathsSections_[1]->setBounds (col.removeFromTop (h));
            col.removeFromTop (4);
            pluginPathsSections_[2]->setBounds (col);
        }
        else if (sampleMode && sampleBank_)
        {
            sampleBank_->setBounds (r);
        }
        else if (sessionMode && sessionPane_)
        {
            sessionPane_->setBounds (r);
        }
    }

    void paintOverChildren (Graphics& g) override
    {
        /* Inline labels that didn't merit a Label component. */
        g.setColour (kTextColour);
        g.setFont (FontOptions ( 12.0f, Font::plain));
        g.drawText ("File:", filenameLabelArea_, Justification::centredLeft);
    }

    /* === NativeFileListComponent::Listener ============================== */
    void selectionChanged (const File& sel) override
    {
        if (sel.existsAsFile())
        {
            DiskOpService::get().setSelectedFile (sel);
            filenameEdit_.setText (sel.getFileName(), dontSendNotification);
        }
    }
    void fileActivated (const File& f) override
    {
        if (! f.existsAsFile()) return;
        DiskOpService::get().setSelectedFile (f);
        statusLabel_.setText ("Selected: " + f.getFileName(), dontSendNotification);
        DiskOpService::get().fireActivation();
    }
    void rootChanged (const File& newRoot) override
    {
        DiskOpService::get().setCurrentDirectory (newRoot);
    }

    /* Called by owner on service change. */
    void syncFromService()
    {
        auto& svc = DiskOpService::get();
        const int mi = (int) svc.getMode();
        if (mi >= 0 && mi < modeButtons_.size())
            modeButtons_[mi]->setToggleState (true, dontSendNotification);
        pathEdit_.setText (svc.getCurrentDirectory().getFullPathName(),
                           dontSendNotification);
        if (filenameEdit_.getText() != svc.getFilename())
            filenameEdit_.setText (svc.getFilename(), dontSendNotification);
        modeBadge_.setText ("Mode: " + DiskOpService::getModeName (svc.getMode()),
                            dontSendNotification);

        /* If the filter changed because the mode changed, push it
         * through (no rebuild needed — the native lister filters in
         * the worker thread). */
        applyWildcard();

        /* If cwd changed externally, push to the lister. */
        if (browser_.getRoot() != svc.getCurrentDirectory()
            && svc.getCurrentDirectory().isDirectory())
        {
            browser_.setRoot (svc.getCurrentDirectory());
        }

        /* Mode changed OR request-armed state flipped → re-layout to
         * swap right-pane content. */
        const bool reqNow = svc.hasPendingRequest();
        if (lastMode_ != svc.getMode() || lastRequestActive_ != reqNow)
        {
            lastMode_ = svc.getMode();
            lastRequestActive_ = reqNow;
            resized();
        }
    }

private:
    void configureLabel (Label& l, const String& text)
    {
        l.setText (text, dontSendNotification);
        l.setJustificationType (Justification::centredLeft);
        l.setColour (Label::textColourId, kTextColour);
        l.setFont (FontOptions ( 12.0f, Font::plain));
        addAndMakeVisible (l);
    }
    void configureActionButton (TextButton& b, const String& text,
                                std::function<void()> on)
    {
        b.setButtonText (text);
        b.onClick = std::move (on);
        b.setColour (TextButton::buttonColourId, kPanelColour);
        b.setColour (TextButton::textColourOffId, kTextColour);
        addAndMakeVisible (b);
    }

    /** Sync the native lister's wildcard with the current mode +
     *  "All files" toggle.  Cheap — the lister re-queries the cache
     *  with the new filter and re-pulls from the cached snapshot
     *  (or queues a worker scan if not cached yet). */
    void applyWildcard()
    {
        auto& svc = DiskOpService::get();
        /* Request-driven wildcard wins over the mode default — e.g.
         * "Import Graph" wants *.elg even though the request mode hint
         * is kSession. */
        juce::String wildcard;
        if (const auto* req = svc.getPendingRequest(); req != nullptr && req->wildcard.isNotEmpty())
            wildcard = req->wildcard;
        else
            wildcard = allFiles_ ? juce::String ("*")
                                 : DiskOpService::getWildcardForMode (svc.getMode());
        if (wildcard == currentWildcard_) return;
        currentWildcard_ = wildcard;
        browser_.setWildcard (wildcard);
    }

    void rebuildWineDriveButtons()
    {
        wineBtns_.clear();
        const auto drives = DiskOpService::enumerateWineDrives();
        for (const auto& d : drives)
        {
            auto btn = std::make_unique<TextButton> ("Wine " + d.letter.toUpperCase());
            const auto target = d.target;
            btn->onClick = [target] {
                if (target.isDirectory())
                    DiskOpService::get().setCurrentDirectory (target);
            };
            btn->setColour (TextButton::buttonColourId, kPanelColour);
            btn->setColour (TextButton::textColourOffId, kAccentAmber);
            addAndMakeVisible (*btn);
            wineBtns_.add (std::move (btn));
        }
    }

    DiskOpContentView& owner_;

    /* Mode radio + sidebar labels. */
    Label modeLabel_, pathLabel_, statusLabel_, modeBadge_, allFilesLabel_;
    OwnedArray<ToggleButton> modeButtons_;
    ToggleButton allFilesToggle_;

    /* Path / filename inputs. */
    TextEditor pathEdit_, filenameEdit_;
    Rectangle<int> filenameLabelArea_;

    /* Action toolbar. */
    TextButton refreshBtn_, setPathBtn_, homeBtn_, rootBtn_;
    OwnedArray<TextButton> wineBtns_;

    /* Embedded file browser — native POSIX-backed lister with a
     * shared worker-thread cache (no juce::FileBrowserComponent). */
    NativeDirCache          dirCache_;
    NativeFileListComponent browser_;
    String currentWildcard_;
    bool   allFiles_ = false;

    /* Mode-extras right pane.  Plugin Paths mode shows 3 PluginPathsSection
     * children; Sample mode shows the SampleBankPane mirroring the active
     * graph's first SamplerNode; Session mode shows the SessionPane
     * (New / Open / Save / Save As + recents).  RequestPane temporarily
     * replaces all of the above while a one-shot DiskOpService request
     * is armed. */
    OwnedArray<PluginPathsSection> pluginPathsSections_;
    std::unique_ptr<SampleBankPane> sampleBank_;
    std::unique_ptr<SessionPane>    sessionPane_;
    std::unique_ptr<RequestPane>    requestPane_;

    Services* services_ = nullptr;

    /* Layout cache. */
    Rectangle<int> sidebarBounds_, toolbarBounds_, extrasBounds_;
    DiskOpService::Mode lastMode_ { (DiskOpService::Mode) -1 };
    bool                lastRequestActive_ { false };
};


/* ===========================================================================
 * DiskOpContentView
 * ========================================================================*/
DiskOpContentView::DiskOpContentView()
    : impl_ (std::make_unique<Impl> (*this))
{
    addAndMakeVisible (impl_.get());
    DiskOpService::get().addChangeListener (this);
}

DiskOpContentView::~DiskOpContentView()
{
    DiskOpService::get().removeChangeListener (this);
}

void DiskOpContentView::initializeView (Services& services)
{
    if (impl_) impl_->wireServices (services);
}

void DiskOpContentView::paint (Graphics& g)
{
    g.fillAll (kBgColour);
}

void DiskOpContentView::resized()
{
    if (impl_) impl_->setBounds (getLocalBounds());
}

void DiskOpContentView::didBecomeActive() { if (impl_) impl_->syncFromService(); }
void DiskOpContentView::willBeRemoved()   {}

void DiskOpContentView::changeListenerCallback (ChangeBroadcaster*)
{
    if (impl_) impl_->syncFromService();
}

/* File-list events handled by Impl (NativeFileListComponent::Listener). */

} // namespace element
