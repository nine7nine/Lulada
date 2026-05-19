// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce.hpp>
#include <deque>
#include <map>

namespace element {

/** A directory entry — name + type + mtime.  Populated by the worker
 *  thread inside NativeDirCache via direct POSIX opendir / readdir,
 *  bypassing JUCE's juce::File enumeration to avoid any wineserver
 *  round-trips when Element runs as a winelib build. */
struct NativeDirEntry
{
    juce::String name;     /* basename */
    bool         isDir = false;
    juce::int64  size = 0;
    juce::int64  mtimeMs = 0;
};


/** Background-thread cache of directory listings.  Snapshot per
 *  directory keyed by absolute path; invalidation by stat-ing the
 *  directory's own mtime (one syscall, not one per file).  Worker
 *  thread services a queue of scan requests; UI thread reads the
 *  cache synchronously and gets either stale or fresh data, with a
 *  ChangeBroadcaster signal when fresh data arrives.
 *
 *  Wildcard filtering is done on the worker so the UI thread paints
 *  a fully-filtered array. */
class NativeDirCache : public juce::ChangeBroadcaster,
                      private juce::Thread
{
public:
    NativeDirCache();
    ~NativeDirCache() override;

    /** Result returned from get(): the cached entries + a freshness
     *  flag (true = scan completed since last call, false = stale or
     *  scan still in flight). */
    struct Snapshot
    {
        juce::Array<NativeDirEntry> entries;
        juce::int64                 dirMtimeMs = 0;
        bool                        fresh = false;
    };

    /** Return the cached snapshot for `dir` (filtered by `wildcard`).
     *  If no cache yet, queues an async scan and returns an empty
     *  snapshot.  When the scan completes, sendChangeMessage() fires
     *  on the message thread — UI listeners then re-call get() to
     *  pick up the new data. */
    Snapshot get (const juce::File& dir, const juce::String& wildcard);

    /** Force a re-scan on next get(), regardless of mtime check. */
    void invalidate (const juce::File& dir);

    /** Drop all cached entries (e.g. memory pressure). */
    void clear();

private:
    struct Pending {
        juce::String absPath;
        juce::String wildcard;
    };

    /** Cache key includes the active wildcard so different filters
     *  don't fight over the same slot.  The cost is one stored vector
     *  per (dir, filter) tuple. */
    struct CacheKey
    {
        juce::String absPath;
        juce::String wildcard;
        bool operator< (const CacheKey& o) const
        {
            if (absPath  != o.absPath)  return absPath  < o.absPath;
            return wildcard < o.wildcard;
        }
    };

    void run() override;
    void enqueue (const Pending& p);
    void scanDir (const Pending& p);
    static bool wildcardMatch (const juce::String& name, const juce::String& filter);
    static juce::int64 statMtimeMs (const char* path);

    juce::CriticalSection lock_;
    std::map<CacheKey, Snapshot> cache_;
    std::deque<Pending>           queue_;
    juce::WaitableEvent           wakeup_;
};


/** Component that lists files from a NativeDirCache.  Mirrors the
 *  parts of juce::FileBrowserComponent we actually use:
 *
 *    - Path bar at the top (Up button + current dir label / editable)
 *    - Scrollable file list
 *    - Selection + double-click → activation
 *    - Wildcard filter
 *
 *  Disk activity happens on the cache's worker thread — UI thread
 *  only paints from the cached snapshot. */
class NativeFileListComponent : public juce::Component,
                                public juce::ChangeListener,
                                private juce::ListBoxModel
{
public:
    /** Listener interface — mirrors juce::FileBrowserListener subset. */
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void selectionChanged (const juce::File& selected) = 0;
        virtual void fileActivated    (const juce::File& activated) = 0;   /* dblclick */
        virtual void rootChanged      (const juce::File& newRoot)   {}
    };

    NativeFileListComponent (NativeDirCache& cache);
    ~NativeFileListComponent() override;

    void setRoot (const juce::File& dir);
    juce::File getRoot() const { return root_; }

    void setWildcard (const juce::String& w);

    /** Manually refresh (re-stat the dir + re-list). */
    void refresh();

    /** Currently-highlighted file (or invalid File if none / dir). */
    juce::File getSelectedFile() const;

    void addListener (Listener* l)    { listeners_.add (l); }
    void removeListener (Listener* l) { listeners_.remove (l); }

    /* === Component ============================================== */
    void paint (juce::Graphics&) override;
    void resized() override;

    /* === ListBoxModel =========================================== */
    int getNumRows() override { return entries_.size(); }
    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;

    /* === ChangeListener (cache → UI) ============================ */
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

private:
    void pullSnapshot();
    void navigateUp();

    NativeDirCache& cache_;
    juce::File      root_;
    juce::String    wildcard_ { "*" };
    juce::Array<NativeDirEntry> entries_;

    juce::Label      pathLabel_;
    juce::TextButton upBtn_;
    juce::TextButton refreshBtn_;
    juce::ListBox    list_;

    juce::ListenerList<Listener> listeners_;
};

} // namespace element
