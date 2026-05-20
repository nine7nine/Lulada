// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/nativefilelist.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <fnmatch.h>
#include <cstring>
#include <algorithm>

using namespace juce;

namespace element {

namespace {

const Colour kBgColour       { 0xff'18'18'18 };
const Colour kPanelColour    { 0xff'1c'1c'1c };
const Colour kOutlineColour  { 0xff'2a'2a'2a };
const Colour kTextColour     { 0xff'd4'd4'd4 };
const Colour kMutedText      { 0xff'7a'7a'7a };
const Colour kDirColour      { 0xff'5a'a5'd0 };
const Colour kSelTint        { 0x44'40'a0'ff };

} // namespace


/* ===========================================================================
 * NativeDirCache
 * ========================================================================*/
NativeDirCache::NativeDirCache()
    : Thread ("native-dir-cache")
{
    startThread (Thread::Priority::low);
}

NativeDirCache::~NativeDirCache()
{
    /* Worker thread holds references to cache_ / queue_ / lock_ which
     * are about to be destroyed after this dtor returns.  Give the
     * thread enough budget to wake from wakeup_.wait, drain whatever
     * scan it was on (each readdir iteration checks threadShouldExit),
     * and exit before any member is freed.  2 s was tight on slow
     * media — 4 s removes the post-stopThread-timeout dangling-thread
     * risk. */
    signalThreadShouldExit();
    wakeup_.signal();
    stopThread (4000);
}

NativeDirCache::Snapshot NativeDirCache::get (const File& dir, const String& wildcard)
{
    if (! dir.isDirectory())
        return {};

    const CacheKey key { dir.getFullPathName(), wildcard };

    Snapshot result;
    bool needsScan = true;

    {
        const ScopedLock sl (lock_);
        const auto it = cache_.find (key);
        if (it != cache_.end())
        {
            result = it->second;
            result.fresh = false;
            /* Compare cached mtime against the current dir mtime — one
             * stat() call.  If unchanged we serve cached. */
            const auto cur = statMtimeMs (dir.getFullPathName().toRawUTF8());
            if (cur != 0 && cur == result.dirMtimeMs)
                needsScan = false;
        }
    }

    if (needsScan)
        enqueue ({ dir.getFullPathName(), wildcard });

    return result;
}

void NativeDirCache::invalidate (const File& dir)
{
    const auto key = dir.getFullPathName();
    const ScopedLock sl (lock_);
    for (auto it = cache_.begin(); it != cache_.end(); )
    {
        if (it->first.absPath == key) it = cache_.erase (it);
        else                          ++it;
    }
}

void NativeDirCache::clear()
{
    const ScopedLock sl (lock_);
    cache_.clear();
}

void NativeDirCache::enqueue (const Pending& p)
{
    {
        const ScopedLock sl (lock_);
        /* Coalesce duplicate scans — keep only one pending request
         * per (dir, wildcard). */
        for (const auto& q : queue_)
            if (q.absPath == p.absPath && q.wildcard == p.wildcard)
                return;
        queue_.push_back (p);
    }
    wakeup_.signal();
}

void NativeDirCache::run()
{
    while (! threadShouldExit())
    {
        Pending p;
        bool gotOne = false;
        {
            const ScopedLock sl (lock_);
            if (! queue_.empty())
            {
                p = queue_.front();
                queue_.pop_front();
                gotOne = true;
            }
        }
        if (gotOne)
        {
            scanDir (p);
            /* Send change notification on the message thread.  ALL
             * waiting UI components re-read get() and pick up new
             * data.  ChangeBroadcaster::sendChangeMessage already
             * marshalls to the message thread. */
            sendChangeMessage();
            continue;
        }
        wakeup_.wait (1000);
    }
}

bool NativeDirCache::wildcardMatch (const String& name, const String& filter)
{
    if (filter.isEmpty() || filter == "*") return true;
    /* Filter can be semicolon-separated patterns. */
    auto patterns = StringArray::fromTokens (filter, ";", "");
    for (const auto& pat : patterns)
    {
        const auto p = pat.trim();
        if (p.isEmpty()) continue;
        if (fnmatch (p.toRawUTF8(), name.toRawUTF8(), FNM_CASEFOLD) == 0)
            return true;
    }
    return false;
}

int64 NativeDirCache::statMtimeMs (const char* path)
{
    struct stat st;
    if (::stat (path, &st) != 0) return 0;
    return (int64) st.st_mtime * 1000;
}

void NativeDirCache::scanDir (const Pending& p)
{
    Snapshot snap;
    snap.fresh = true;

    const auto absPath = p.absPath.toRawUTF8();
    snap.dirMtimeMs = statMtimeMs (absPath);

    DIR* dirp = ::opendir (absPath);
    if (dirp == nullptr)
    {
        const ScopedLock sl (lock_);
        cache_[CacheKey { p.absPath, p.wildcard }] = std::move (snap);
        return;
    }

    Array<NativeDirEntry> dirs, files;
    while (true)
    {
        if (threadShouldExit()) { ::closedir (dirp); return; }
        struct dirent* ent = ::readdir (dirp);
        if (ent == nullptr) break;

        const char* name = ent->d_name;
        if (name[0] == '.' && (name[1] == '\0'
            || (name[1] == '.' && name[2] == '\0')))
            continue;  /* skip . and .. */
        if (name[0] == '.') continue;  /* skip hidden */

        NativeDirEntry e;
        e.name = String (CharPointer_UTF8 (name));

        bool isDir = false;
        bool isFile = false;
        if (ent->d_type == DT_DIR) isDir = true;
        else if (ent->d_type == DT_REG) isFile = true;
        else
        {
            /* DT_UNKNOWN / DT_LNK — fall back to stat. */
            String fullPath = p.absPath;
            fullPath << '/' << e.name;
            struct stat st;
            if (::stat (fullPath.toRawUTF8(), &st) == 0)
            {
                isDir  = S_ISDIR (st.st_mode);
                isFile = S_ISREG (st.st_mode);
                e.size = (int64) st.st_size;
                e.mtimeMs = (int64) st.st_mtime * 1000;
            }
        }
        e.isDir = isDir;
        if (! isDir && ! isFile) continue;

        if (! isDir && ! wildcardMatch (e.name, p.wildcard))
            continue;

        if (isDir) dirs.add (std::move (e));
        else       files.add (std::move (e));
    }
    ::closedir (dirp);

    auto byName = [] (const NativeDirEntry& a, const NativeDirEntry& b) {
        return a.name.compareIgnoreCase (b.name) < 0;
    };
    std::sort (dirs.begin(),  dirs.end(),  byName);
    std::sort (files.begin(), files.end(), byName);

    snap.entries.ensureStorageAllocated (dirs.size() + files.size());
    snap.entries.addArray (dirs);
    snap.entries.addArray (files);

    {
        const ScopedLock sl (lock_);
        cache_[CacheKey { p.absPath, p.wildcard }] = std::move (snap);
    }
}


/* ===========================================================================
 * NativeFileListComponent
 * ========================================================================*/
NativeFileListComponent::NativeFileListComponent (NativeDirCache& cache)
    : cache_ (cache)
{
    cache_.addChangeListener (this);

    upBtn_.setButtonText ("Up");
    upBtn_.onClick = [this] { navigateUp(); };
    upBtn_.setColour (TextButton::buttonColourId,    kPanelColour);
    upBtn_.setColour (TextButton::textColourOffId,   kTextColour);
    addAndMakeVisible (upBtn_);

    refreshBtn_.setButtonText ("Refresh");
    refreshBtn_.onClick = [this] { refresh(); };
    refreshBtn_.setColour (TextButton::buttonColourId,    kPanelColour);
    refreshBtn_.setColour (TextButton::textColourOffId,   kTextColour);
    addAndMakeVisible (refreshBtn_);

    pathLabel_.setColour (Label::backgroundColourId, kPanelColour);
    pathLabel_.setColour (Label::textColourId,        kTextColour);
    pathLabel_.setFont (FontOptions (13.0f, Font::plain));
    addAndMakeVisible (pathLabel_);

    list_.setModel (this);
    list_.setRowHeight (22);
    list_.setColour (ListBox::backgroundColourId, kBgColour);
    list_.setColour (ListBox::outlineColourId,    kOutlineColour);
    list_.setOutlineThickness (1);
    addAndMakeVisible (list_);
}

NativeFileListComponent::~NativeFileListComponent()
{
    cache_.removeChangeListener (this);
}

void NativeFileListComponent::setRoot (const File& dir)
{
    if (! dir.isDirectory()) return;
    if (root_ == dir) return;
    root_ = dir;
    pathLabel_.setText (root_.getFullPathName(), dontSendNotification);
    pullSnapshot();
    listeners_.call ([this] (Listener& l) { l.rootChanged (root_); });
}

void NativeFileListComponent::setWildcard (const String& w)
{
    if (wildcard_ == w) return;
    wildcard_ = w;
    pullSnapshot();
}

void NativeFileListComponent::refresh()
{
    cache_.invalidate (root_);
    pullSnapshot();
}

File NativeFileListComponent::getSelectedFile() const
{
    const int row = list_.getSelectedRow();
    if (row < 0 || row >= entries_.size()) return {};
    const auto& e = entries_[row];
    return root_.getChildFile (e.name);
}

void NativeFileListComponent::paint (Graphics& g)
{
    g.fillAll (kBgColour);
}

void NativeFileListComponent::resized()
{
    auto r = getLocalBounds();

    auto top = r.removeFromTop (22);
    upBtn_     .setBounds (top.removeFromLeft (40));
    top.removeFromLeft (4);
    refreshBtn_.setBounds (top.removeFromRight (60));
    top.removeFromRight (4);
    pathLabel_ .setBounds (top);
    r.removeFromTop (2);

    list_.setBounds (r);
}

void NativeFileListComponent::paintListBoxItem (int row, Graphics& g, int w, int h, bool sel)
{
    if (row < 0 || row >= entries_.size()) return;
    const auto& e = entries_[row];

    if (sel) { g.setColour (kSelTint); g.fillRect (0, 0, w, h); }

    /* Default sans-serif font for readability (matches the Plugin
     * Manager / VST scanner list).  Monospaced reserved for the
     * sampler keymap + tracker grid where character alignment matters. */
    g.setFont (FontOptions (13.0f, Font::plain));

    /* Icon glyph — distinct UTF-8 marks for dir vs file.  Both string
     * literals MUST go through CharPointer_UTF8 — JUCE's String(const
     * char*) treats raw bytes as the platform default, not UTF-8, so
     * a bare "\xe2\x80\xa2" comes out as garbled "âç". */
    g.setColour (e.isDir ? kDirColour : kMutedText);
    g.drawText (e.isDir ? String (CharPointer_UTF8 ("\xe2\x96\xb8"))   /* ▸ small triangle */
                        : String (CharPointer_UTF8 ("\xe2\x80\xa2")),  /* • bullet */
                6, 0, 18, h, Justification::centred);

    /* Name in white for both dirs + files — icon alone tells you the
     * type, no need to repeat that as a colour code. */
    g.setColour (kTextColour);
    g.drawText (e.name, 28, 0, w - 110, h,
                Justification::centredLeft);

    if (! e.isDir && e.size > 0)
    {
        const String sz = e.size > 1024 * 1024
                            ? String::formatted ("%.1f MB", e.size / 1048576.0)
                            : (e.size > 1024
                                ? String::formatted ("%.1f KB", e.size / 1024.0)
                                : String::formatted ("%lld B", (long long) e.size));
        g.setColour (kMutedText);
        g.drawText (sz, w - 92, 0, 86, h, Justification::centredRight);
    }
}

void NativeFileListComponent::listBoxItemClicked (int row, const MouseEvent&)
{
    if (row < 0 || row >= entries_.size()) return;
    const auto f = root_.getChildFile (entries_[row].name);
    listeners_.call ([&] (Listener& l) { l.selectionChanged (f); });
}

void NativeFileListComponent::listBoxItemDoubleClicked (int row, const MouseEvent&)
{
    if (row < 0 || row >= entries_.size()) return;
    const auto f = root_.getChildFile (entries_[row].name);
    if (entries_[row].isDir)
        setRoot (f);
    else
        listeners_.call ([&] (Listener& l) { l.fileActivated (f); });
}

void NativeFileListComponent::returnKeyPressed (int row)
{
    /* Enter activates the highlighted row exactly like a double-click:
     * directory → descend, file → fileActivated (which in the Disk Op
     * page broadcasts on DiskOpService::activations → load-to-slot or
     * load-session, depending on mode). */
    if (row < 0 || row >= entries_.size()) return;
    const auto f = root_.getChildFile (entries_[row].name);
    if (entries_[row].isDir)
        setRoot (f);
    else
        listeners_.call ([&] (Listener& l) { l.fileActivated (f); });
}

void NativeFileListComponent::changeListenerCallback (ChangeBroadcaster*)
{
    /* Cache reported new data for some directory.  Cheaply re-pull;
     * if the snapshot for our current root has different fingerprint
     * we update the list view.  Otherwise no-op. */
    pullSnapshot();
}

void NativeFileListComponent::pullSnapshot()
{
    auto snap = cache_.get (root_, wildcard_);
    if (snap.entries.size() == entries_.size())
    {
        bool same = true;
        for (int i = 0; i < entries_.size(); ++i)
        {
            if (entries_[i].name != snap.entries[i].name
                || entries_[i].isDir != snap.entries[i].isDir)
            {
                same = false; break;
            }
        }
        if (same && ! snap.fresh) return;
    }
    entries_ = std::move (snap.entries);
    list_.updateContent();
    list_.repaint();
}

void NativeFileListComponent::navigateUp()
{
    const auto parent = root_.getParentDirectory();
    if (parent.isDirectory() && parent != root_)
        setRoot (parent);
}

} // namespace element
