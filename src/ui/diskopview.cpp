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

        /* Sidebar (left): mode selector + Wine drive quick-nav. */
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

        /* Wine-drive quick-nav. */
        for (auto& b : wineBtns_)
        {
            b->setBounds (sb.removeFromTop (22));
            sb.removeFromTop (2);
        }
        sb.removeFromTop (6);
        homeBtn_.setBounds (sb.removeFromTop (22));  sb.removeFromTop (2);
        rootBtn_.setBounds (sb.removeFromTop (22));

        r.removeFromLeft (10);

        /* Right column: toolbar + path/filename + browser. */
        toolbarBounds_ = r.removeFromTop (36);
        auto tb = toolbarBounds_.reduced (6);
        refreshBtn_.setBounds (tb.removeFromLeft (70)); tb.removeFromLeft (4);
        setPathBtn_.setBounds (tb.removeFromLeft (70)); tb.removeFromLeft (8);
        allFilesToggle_.setBounds (tb.removeFromLeft (90));
        tb.removeFromLeft (8);
        statusLabel_.setBounds (tb);

        r.removeFromTop (6);
        auto pathRow = r.removeFromTop (24);
        pathLabel_.setBounds (pathRow.removeFromLeft (40));
        pathEdit_.setBounds (pathRow);

        r.removeFromTop (4);
        auto fileRow = r.removeFromTop (24);
        Label dummy; dummy.setText ("File:", dontSendNotification);
        // (Filename label inline, drawn next to the edit — no allocation needed.)
        // We draw label text in paint? simpler: use a static area for the label.
        const auto labelArea = fileRow.removeFromLeft (40);
        filenameLabelArea_ = labelArea;
        filenameEdit_.setBounds (fileRow);

        r.removeFromTop (6);
        if (browser_ != nullptr)
            browser_->setBounds (r);
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

    /* Layout cache. */
    Rectangle<int> sidebarBounds_, toolbarBounds_;
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
