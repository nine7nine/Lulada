// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <element/juce.hpp>

namespace element {

/** App-wide "Disk Op" service — single source of truth for the embedded
 *  file-ops nav page.  Other components (sampler editor, session save
 *  flow, plugin path management) read the current selection per mode
 *  and listen for change broadcasts instead of opening native dialogs.
 *
 *  Stage 1: Sample mode functional; Session + Plugin Paths modes are
 *  defined in the enum but their wiring lands in later stages.
 *
 *  Path handling: juce::File accepts both POSIX (`/home/x`) and Windows
 *  (`C:\Users\x`) forms transparently — under winelib both reach the
 *  same FS via Wine's drive-letter remapping (~/.wine/dosdevices/).
 *  DiskOpView exposes a Quick-Nav row that enumerates ~/.wine/dosdevices/
 *  entries so the user can jump between Linux roots and Wine drive
 *  letters without typing.
 *
 *  Singleton — instantiated on first access.  Lifetime tied to the
 *  process; not coupled to Services because the file-ops state needs
 *  to survive session reloads.
 */
class DiskOpService : public juce::ChangeBroadcaster
{
public:
    enum class Mode {
        kSample = 0,         ///< loading/saving audio samples
        kSession,            ///< loading/saving .els session files
        kPluginPaths,        ///< CLAP / VST / VST3 search paths
        kNumModes
    };

    static DiskOpService& get()
    {
        static DiskOpService inst;
        return inst;
    }

    /* --- current mode ------------------------------------------------- */
    Mode getMode() const noexcept { return mode_; }
    void setMode (Mode m)
    {
        if (m == mode_) return;
        mode_ = m;
        sendChangeMessage();
    }

    /* --- current path / selection per mode ---------------------------- */

    /** Currently-focused directory.  Persisted across mode switches.
     *  Defaults to user's home. */
    juce::File getCurrentDirectory() const { return cwd_; }
    void setCurrentDirectory (const juce::File& f)
    {
        if (f == cwd_) return;
        cwd_ = f;
        sendChangeMessage();
    }

    /** Selected file for the active mode.  Sampler reads this when the
     *  user clicks Load. */
    juce::File getSelectedFile() const { return selection_; }
    void setSelectedFile (const juce::File& f)
    {
        if (f == selection_) return;
        selection_ = f;
        sendChangeMessage();
    }

    /** Filename input — what the user has typed for save/rename.
     *  Mode-specific extension applied by callers if blank. */
    juce::String getFilename() const { return filename_; }
    void setFilename (const juce::String& s)
    {
        if (s == filename_) return;
        filename_ = s;
        sendChangeMessage();
    }

    /* --- plugin scan paths (Plugin Paths mode) ------------------------ *
     * One FileSearchPath per format.  Edits live in this service until
     * the user clicks Save in the Plugin Paths page — at which point a
     * registered persist callback writes to PluginManager's props (Stage
     * 2 wiring). */
    enum PluginFormat { kCLAP = 0, kVST2, kVST3, kNumPluginFormats };

    static juce::String getPluginFormatName (PluginFormat f)
    {
        switch (f)
        {
            case kCLAP: return "CLAP";
            case kVST2: return "VST";
            case kVST3: return "VST3";
            default: return "?";
        }
    }

    juce::FileSearchPath getPluginPaths (PluginFormat f) const
    {
        if (f < 0 || f >= kNumPluginFormats) return {};
        return pluginPaths_[(size_t) f];
    }
    void setPluginPaths (PluginFormat f, const juce::FileSearchPath& p)
    {
        if (f < 0 || f >= kNumPluginFormats) return;
        pluginPaths_[(size_t) f] = p;
        sendChangeMessage();
    }

    void addPluginPath (PluginFormat f, const juce::File& dir)
    {
        if (f < 0 || f >= kNumPluginFormats || ! dir.isDirectory()) return;
        pluginPaths_[(size_t) f].add (dir);
        sendChangeMessage();
    }
    void removePluginPath (PluginFormat f, int index)
    {
        if (f < 0 || f >= kNumPluginFormats) return;
        if (index < 0 || index >= pluginPaths_[(size_t) f].getNumPaths()) return;
        pluginPaths_[(size_t) f].remove (index);
        sendChangeMessage();
    }
    void movePluginPath (PluginFormat f, int from, int delta)
    {
        if (f < 0 || f >= kNumPluginFormats) return;
        auto& p = pluginPaths_[(size_t) f];
        const int to = from + delta;
        if (from < 0 || from >= p.getNumPaths()) return;
        if (to   < 0 || to   >= p.getNumPaths()) return;
        const juce::File a = p[from];
        const juce::File b = p[to];
        p.remove (from);
        p.add (b, from);
        p.remove (to);
        p.add (a, to);
        sendChangeMessage();
    }

    /* --- file-type filter strings ------------------------------------- */
    static juce::String getWildcardForMode (Mode m)
    {
        switch (m)
        {
            case Mode::kSample:  return "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg";
            case Mode::kSession: return "*.els";
            case Mode::kPluginPaths: return "*";   // directory mode
            default: return "*";
        }
    }

    static juce::String getModeName (Mode m)
    {
        switch (m)
        {
            case Mode::kSample:      return "Sample";
            case Mode::kSession:     return "Session";
            case Mode::kPluginPaths: return "Plugin Paths";
            default: return "?";
        }
    }

    /* --- Wine drive enumeration --------------------------------------- *
     * Lists ~/.wine/dosdevices/ entries to surface Wine drive letters
     * (Z:, C:, etc.) as quick-nav targets.  Each entry resolves to the
     * Linux symlink target.  Empty when Wine isn't installed or the
     * dosdevices directory is absent. */
    struct WineDrive {
        juce::String letter;       ///< "c:", "z:", etc.
        juce::File   target;       ///< resolved Linux path
    };
    static juce::Array<WineDrive> enumerateWineDrives()
    {
        juce::Array<WineDrive> out;
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
        const auto dosdev = home.getChildFile (".wine/dosdevices");
        if (! dosdev.isDirectory()) return out;

        for (const auto& entry : juce::RangedDirectoryIterator (
                 dosdev, false, "*", juce::File::findFilesAndDirectories))
        {
            const auto child = entry.getFile();
            const auto name  = child.getFileName().toLowerCase();
            // Wine drives look like "c:", "z:", "d::" etc; skip symlinks
            // that aren't drive shape.
            if (name.length() < 2 || name[1] != ':') continue;
            WineDrive d;
            d.letter = name;
            d.target = child.getLinkedTarget();
            if (! d.target.exists()) d.target = child;
            out.add (d);
        }
        return out;
    }

private:
    DiskOpService()
        : cwd_ (juce::File::getSpecialLocation (juce::File::userHomeDirectory)) {}

    Mode         mode_ { Mode::kSample };
    juce::File   cwd_;
    juce::File   selection_;
    juce::String filename_;
    std::array<juce::FileSearchPath, kNumPluginFormats> pluginPaths_;
};

} // namespace element
