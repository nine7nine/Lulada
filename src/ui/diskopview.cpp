// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/diskopview.hpp"
#include "services/diskopservice.hpp"

#include <element/services.hpp>
#include <element/context.hpp>
#include <element/plugins.hpp>
#include <element/nodefactory.hpp>
#include <element/settings.hpp>
#include <element/session.hpp>
#include <element/processor.hpp>
#include <element/node.h>

using namespace juce;

#include "ui/pluginmanagercomponent.hpp"  /* uses unqualified juce types */
#include "nodes/sampler.hpp"

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
        title_.setFont (FontOptions (Font::getDefaultMonospacedFontName(),
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
        g.setFont (FontOptions (Font::getDefaultMonospacedFontName(),
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
 * SampleBankPane — Sample mode's right-side content.  Enumerates
 * SamplerNodes from the active graph, picks one (combo at top), shows
 * its currently-active instrument's 16 slots as a list.  Click a slot
 * → load DiskOpService's selected file into it.  Refresh button
 * re-walks the graph when the user adds/removes sampler nodes.
 * ========================================================================*/
class SampleBankPane : public Component,
                       private ListBoxModel,
                       private Timer
{
public:
    SampleBankPane()
    {
        title_.setText ("Sample Bank", dontSendNotification);
        title_.setColour (Label::textColourId, kTextColour);
        title_.setFont (FontOptions (Font::getDefaultMonospacedFontName(),
                                     kHeaderFontSize, Font::bold));
        addAndMakeVisible (title_);

        samplerCombo_.setColour (ComboBox::backgroundColourId, kPanelColour);
        samplerCombo_.setColour (ComboBox::textColourId, kTextColour);
        samplerCombo_.onChange = [this] {
            activeSampler_ = samplerCombo_.getSelectedId() - 1;
            refresh();
        };
        addAndMakeVisible (samplerCombo_);

        configureBtn (refreshBtn_, "Refresh", [this] { rebuildSamplerList(); });
        configureBtn (loadBtn_, "Load → slot", [this] { loadIntoSelectedSlot(); });

        slotList_.setModel (this);
        slotList_.setRowHeight (22);
        slotList_.setColour (ListBox::backgroundColourId, kBgColour);
        slotList_.setColour (ListBox::outlineColourId,    kOutlineColour);
        slotList_.setOutlineThickness (1);
        addAndMakeVisible (slotList_);

        startTimerHz (2);   /* graph changes are rare; cheap poll. */
    }

    ~SampleBankPane() override { stopTimer(); }

    /** Pull the active graph from Services so we can enumerate
     *  SamplerNodes.  Called once from DiskOpContentView. */
    void connect (Services* services)
    {
        services_ = services;
        rebuildSamplerList();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        title_.setBounds (r.removeFromTop (18));
        r.removeFromTop (4);

        auto topRow = r.removeFromTop (24);
        refreshBtn_.setBounds (topRow.removeFromRight (70)); topRow.removeFromRight (4);
        loadBtn_   .setBounds (topRow.removeFromRight (90)); topRow.removeFromRight (4);
        samplerCombo_.setBounds (topRow);
        r.removeFromTop (4);

        slotList_.setBounds (r);
    }

    /* === ListBoxModel ============================================== */
    int getNumRows() override
    {
        auto inst = activeInstrument();
        if (inst == nullptr) return 0;
        return SamplerInstrument::kNumSlots;
    }
    void paintListBoxItem (int row, Graphics& g, int width, int height,
                           bool rowIsSelected) override
    {
        if (rowIsSelected)
        {
            g.setColour (kAccentBlue.withAlpha (0.3f));
            g.fillRect (0, 0, width, height);
        }
        auto inst = activeInstrument();
        const auto* slot = inst ? inst->getSlot (row) : nullptr;

        g.setFont (FontOptions (Font::getDefaultMonospacedFontName(),
                                kFontSize, Font::plain));

        g.setColour (slot ? kTextColour : kEmptyText);
        g.drawText (String::formatted ("%02d", row + 1),
                    6, 0, 28, height, Justification::centredLeft);

        g.setColour (slot ? kAccentAmber : kEmptyText);
        g.drawText (slot ? slot->name
                         : String (CharPointer_UTF8 ("\xe2\x80\x94")),
                    38, 0, width - 44, height, Justification::centredLeft);
    }
    void listBoxItemDoubleClicked (int row, const MouseEvent&) override
    {
        slotList_.selectRow (row);
        loadIntoSelectedSlot();
    }

private:
    void timerCallback() override
    {
        /* Cheap diff-poll for sampler node count / instrument count
         * change.  No graph traversal if neither moved. */
        const int curCount = countSamplerNodes();
        if (curCount != lastSamplerCount_) rebuildSamplerList();
        slotList_.repaint();
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
                    return sp;
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
        {
            const Node n = graph.getNode (i);
            if (n.getFileOrIdentifier().toString() == EL_NODE_ID_SAMPLER) ++c;
        }
        return c;
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
            samplerCombo_.setEnabled (false);
        }
        else
        {
            samplerCombo_.setEnabled (true);
            if (activeSampler_ >= n) activeSampler_ = 0;
            samplerCombo_.setSelectedId (activeSampler_ + 1, dontSendNotification);
        }
        refresh();
    }

    SamplerInstrument::Ptr activeInstrument() const
    {
        auto* sn = getSamplerProcessor (activeSampler_);
        if (sn == nullptr) return nullptr;
        return sn->getInstrument (0);   /* first instrument for now */
    }

    void loadIntoSelectedSlot()
    {
        const int row = slotList_.getSelectedRow();
        if (row < 0) return;
        auto* sn = getSamplerProcessor (activeSampler_);
        if (sn == nullptr) return;
        const auto sel = DiskOpService::get().getSelectedFile();
        if (! sel.existsAsFile()) return;
        sn->loadSampleToSlot (0, row, sel);
        slotList_.repaint();
    }

    void refresh()
    {
        slotList_.updateContent();
        slotList_.repaint();
    }

    Services* services_ = nullptr;
    int activeSampler_ = 0;
    int lastSamplerCount_ = -1;

    Label title_;
    ComboBox samplerCombo_;
    TextButton refreshBtn_, loadBtn_;
    ListBox  slotList_;
};


/* ===========================================================================
 * DiskOpContentView::Impl
 * ========================================================================*/
class DiskOpContentView::Impl : public Component,
                                public FileBrowserListener
{
public:
    explicit Impl (DiskOpContentView& owner) : owner_ (owner)
    {
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

        rebuildBrowser();

        configureActionButton (refreshBtn_, "Refresh", [this] { refreshBrowser(); });
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
            rebuildBrowser();
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

        /* Mode-extras placeholder for Session mode (until that wires in). */
        extrasPlaceholder_.setJustificationType (Justification::centred);
        extrasPlaceholder_.setColour (Label::textColourId, kMutedText);
        extrasPlaceholder_.setFont (FontOptions (
            Font::getDefaultMonospacedFontName(), kFontSize, Font::plain));
        addChildComponent (extrasPlaceholder_);

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
        if (sampleBank_) sampleBank_->connect (&services);
    }

    ~Impl() override { detachBrowser(); }

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
        if (browser_ != nullptr)
            browser_->setBounds (browserCol);

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

        for (auto& s : pluginPathsSections_) s->setVisible (pluginPaths);
        if (sampleBank_) sampleBank_->setVisible (sampleMode);
        extrasPlaceholder_.setVisible (! pluginPaths && ! sampleMode);

        if (pluginPaths && ! pluginPathsSections_.isEmpty())
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
        else
        {
            extrasPlaceholder_.setBounds (r);
        }
    }

    void paintOverChildren (Graphics& g) override
    {
        /* Inline labels that didn't merit a Label component. */
        g.setColour (kTextColour);
        g.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 12.0f, Font::plain));
        g.drawText ("File:", filenameLabelArea_, Justification::centredLeft);
    }

    /* === FileBrowserListener (relays into the service) ================== */
    void selectionChanged() override
    {
        if (browser_ == nullptr) return;
        const auto sel = browser_->getSelectedFile (0);
        if (sel.existsAsFile())
        {
            DiskOpService::get().setSelectedFile (sel);
            filenameEdit_.setText (sel.getFileName(), dontSendNotification);
        }
        else if (sel.isDirectory())
        {
            DiskOpService::get().setCurrentDirectory (sel);
        }
    }
    void fileClicked (const File&, const MouseEvent&) override {}
    void fileDoubleClicked (const File& f) override
    {
        if (f.isDirectory())
            DiskOpService::get().setCurrentDirectory (f);
        else if (f.existsAsFile())
        {
            DiskOpService::get().setSelectedFile (f);
            statusLabel_.setText ("Selected: " + f.getFileName(), dontSendNotification);
        }
    }
    void browserRootChanged (const File& newRoot) override
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

        /* If the filter changed because the mode changed, rebuild. */
        const auto wildcard = DiskOpService::getWildcardForMode (svc.getMode());
        if (wildcard != currentWildcard_)
            rebuildBrowser();

        /* If cwd changed externally, push to the browser. */
        if (browser_ != nullptr
            && browser_->getRoot() != svc.getCurrentDirectory())
        {
            if (svc.getCurrentDirectory().isDirectory())
                browser_->setRoot (svc.getCurrentDirectory());
        }

        /* Mode changed → re-layout to swap right-pane content. */
        if (lastMode_ != svc.getMode())
        {
            lastMode_ = svc.getMode();
            resized();
        }
    }

private:
    void configureLabel (Label& l, const String& text)
    {
        l.setText (text, dontSendNotification);
        l.setJustificationType (Justification::centredLeft);
        l.setColour (Label::textColourId, kTextColour);
        l.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 12.0f, Font::plain));
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

    void rebuildBrowser()
    {
        detachBrowser();

        auto& svc = DiskOpService::get();
        currentWildcard_ = allFiles_
                               ? "*"
                               : DiskOpService::getWildcardForMode (svc.getMode());
        filter_ = std::make_unique<WildcardFileFilter> (
            currentWildcard_, "*",
            "Files for " + DiskOpService::getModeName (svc.getMode()) + " mode");

        const auto start = svc.getCurrentDirectory().isDirectory()
                              ? svc.getCurrentDirectory()
                              : File::getSpecialLocation (File::userHomeDirectory);

        browser_ = std::make_unique<FileBrowserComponent> (
            FileBrowserComponent::openMode
                | FileBrowserComponent::canSelectFiles
                | FileBrowserComponent::filenameBoxIsReadOnly,
            start, filter_.get(), nullptr);
        browser_->addListener (this);
        addAndMakeVisible (*browser_);
        resized();
    }

    void detachBrowser()
    {
        if (browser_)
        {
            browser_->removeListener (this);
            removeChildComponent (browser_.get());
            browser_.reset();
        }
        filter_.reset();
    }

    void refreshBrowser()
    {
        if (browser_ != nullptr)
            browser_->refresh();
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

    /* Embedded file browser. */
    std::unique_ptr<WildcardFileFilter>   filter_;
    std::unique_ptr<FileBrowserComponent> browser_;
    String currentWildcard_;
    bool   allFiles_ = false;

    /* Mode-extras right pane.  Plugin Paths mode shows 3 PluginPathsSection
     * children; Sample mode shows the SampleBankPane mirroring the active
     * graph's first SamplerNode; Session mode currently shows a
     * placeholder Label until session save/load wires up. */
    OwnedArray<PluginPathsSection> pluginPathsSections_;
    std::unique_ptr<SampleBankPane> sampleBank_;
    Label extrasPlaceholder_ { {}, "Session recents — coming next iteration." };

    Services* services_ = nullptr;

    /* Layout cache. */
    Rectangle<int> sidebarBounds_, toolbarBounds_, extrasBounds_;
    DiskOpService::Mode lastMode_ { (DiskOpService::Mode) -1 };
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

/* Stub overrides — Impl owns the real FileBrowserComponent + listener. */
void DiskOpContentView::selectionChanged() {}
void DiskOpContentView::fileClicked (const File&, const MouseEvent&) {}
void DiskOpContentView::fileDoubleClicked (const File&) {}
void DiskOpContentView::browserRootChanged (const File&) {}

} // namespace element
