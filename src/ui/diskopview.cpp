// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/diskopview.hpp"
#include "services/diskopservice.hpp"

using namespace juce;

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
 * PluginPathsSection — one ListBox + Add/Remove/Up/Down per plugin format.
 * Add appends the currently-browsed Disk Op directory.
 * ========================================================================*/
class PluginPathsSection : public Component,
                           private ListBoxModel,
                           private ChangeListener
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
            DiskOpService::get().addPluginPath (format_, cwd);
        });
        configureBtn (removeBtn_, "Remove", [this] {
            const int sel = list_.getSelectedRow();
            if (sel >= 0) DiskOpService::get().removePluginPath (format_, sel);
        });
        configureBtn (upBtn_, "Up", [this] {
            DiskOpService::get().movePluginPath (format_, list_.getSelectedRow(), -1);
        });
        configureBtn (downBtn_, "Down", [this] {
            DiskOpService::get().movePluginPath (format_, list_.getSelectedRow(), +1);
        });

        DiskOpService::get().addChangeListener (this);
        refresh();
    }

    ~PluginPathsSection() override
    {
        DiskOpService::get().removeChangeListener (this);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (4);
        title_.setBounds (r.removeFromTop (18));
        r.removeFromTop (2);

        auto btnRow = r.removeFromBottom (22);
        addBtn_   .setBounds (btnRow.removeFromLeft (74)); btnRow.removeFromLeft (4);
        removeBtn_.setBounds (btnRow.removeFromLeft (74)); btnRow.removeFromLeft (4);
        upBtn_    .setBounds (btnRow.removeFromLeft (40)); btnRow.removeFromLeft (4);
        downBtn_  .setBounds (btnRow.removeFromLeft (40));
        r.removeFromBottom (4);

        list_.setBounds (r);
    }

    /* === ListBoxModel ============================================== */
    int getNumRows() override
    {
        return DiskOpService::get().getPluginPaths (format_).getNumPaths();
    }
    void paintListBoxItem (int row, Graphics& g, int width, int height,
                           bool rowIsSelected) override
    {
        if (rowIsSelected)
        {
            g.setColour (kAccentBlue.withAlpha (0.3f));
            g.fillRect (0, 0, width, height);
        }
        const auto path = DiskOpService::get().getPluginPaths (format_);
        if (row < 0 || row >= path.getNumPaths()) return;
        g.setColour (kTextColour);
        g.setFont (FontOptions (Font::getDefaultMonospacedFontName(),
                                kFontSize, Font::plain));
        g.drawText (path[row].getFullPathName(),
                    6, 0, width - 12, height, Justification::centredLeft);
    }

private:
    void configureBtn (TextButton& b, const String& text, std::function<void()> on)
    {
        b.setButtonText (text);
        b.onClick = std::move (on);
        b.setColour (TextButton::buttonColourId, kPanelColour);
        b.setColour (TextButton::textColourOffId, kTextColour);
        addAndMakeVisible (b);
    }
    void changeListenerCallback (ChangeBroadcaster*) override { refresh(); }
    void refresh()
    {
        list_.updateContent();
        list_.repaint();
    }

    DiskOpService::PluginFormat format_;
    Label  title_;
    ListBox list_;
    TextButton addBtn_, removeBtn_, upBtn_, downBtn_;
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

        /* Mode-extras placeholder shown in non-Plugin-Paths modes. */
        extrasPlaceholder_.setJustificationType (Justification::centred);
        extrasPlaceholder_.setColour (Label::textColourId, kMutedText);
        extrasPlaceholder_.setFont (FontOptions (
            Font::getDefaultMonospacedFontName(), kFontSize, Font::plain));
        addAndMakeVisible (extrasPlaceholder_);

        syncFromService();
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

        /* === Column 2 — File browser column.  Narrow per design note:
         * "file navigation is never going to require full horizontal."
         * Holds toolbar / path / filename / FileBrowserComponent. === */
        const int browserColW = juce::jmin (480, r.getWidth() * 5 / 12);
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
        const bool pluginPaths = svc.getMode() == DiskOpService::Mode::kPluginPaths;
        for (int i = 0; i < pluginPathsSections_.size(); ++i)
        {
            pluginPathsSections_[i]->setVisible (pluginPaths);
        }
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

        extrasPlaceholder_.setVisible (! pluginPaths);
        if (! pluginPaths) extrasPlaceholder_.setBounds (extrasBounds_);
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
     * children; other modes show a single placeholder Label for now (Sample
     * mode's sample-bank mirror, Session mode's recent-sessions list lands
     * in a follow-up). */
    OwnedArray<PluginPathsSection> pluginPathsSections_;
    Label extrasPlaceholder_ { {}, "Sample bank mirror — coming next iteration." };

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
