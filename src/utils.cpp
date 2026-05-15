// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "utils.hpp"

#if JUCE_WINDOWS
#include <windows.h>
#endif

namespace element {
namespace Util {

StringArray compiledAudioPluginFormats()
{
    StringArray fmts;

#if JUCE_MAC && JUCE_PLUGINHOST_AU
    fmts.add ("AudioUnit");
#endif
#if JUCE_PLUGINHOST_VST
    fmts.add ("VST");
#endif
#if JUCE_PLUGINHOST_VST3
    fmts.add ("VST3");
#endif
    /* LADSPA and LV2 are Linux-native plugin formats — they don't help
     * a winelib host load Windows plugins, and Carla's LV2 bundles in
     * particular hang the scanner (Element ends up trying to load
     * kxstudio.sf.net plugin URIs that pull in Carla's massive JUCE-
     * based plugin host as a side effect, leaving the UI near-frozen).
     * The addDefaultFormats gate in pluginmanager.cpp already excludes
     * them under __WINE__; this list has to match or LV2 / LADSPA leak
     * back in via paths-by-name lookups in the scanner thread + the
     * options-menu UI. */
#if JUCE_PLUGINHOST_LADSPA && ! defined (__WINE__)
    fmts.add ("LADSPA");
#endif
#if JUCE_PLUGINHOST_LV2 && ! defined (__WINE__)
    fmts.add ("LV2");
#endif
    fmts.add ("CLAP");
    fmts.sort (false);
    return fmts;
}

bool isRunningInWine()
{
#if JUCE_WINDOWS
    HMODULE ntdll = GetModuleHandleA ("ntdll");
    return ntdll != nullptr && GetProcAddress (ntdll, "wine_get_version") != nullptr;
#else
    return false;
#endif
}

/* Augment a plugin search path with the standard Wine prefix locations
 * for the given format.  Idempotent (uses addIfNotAlreadyThere via the
 * caller — we call .add directly which is a no-op for duplicates by
 * String, plus the format-level dedupe in PluginManager strips repeats).
 *
 * Under winelib (compiled with `__WINE__`), JUCE's
 * VSTPluginFormat::getDefaultLocationsToSearch returns Linux defaults
 * only — `~/.vst`, `/usr/lib/vst`, `/usr/local/lib/vst`.  None of those
 * point at the Wine prefix's plugin directories where actual Windows
 * VST2/VST3 plugins live, so an unconfigured Element scan finds zero
 * Windows plugins and the user has to add paths manually.  This helper
 * is called from the UI's getLastSearchPath default and from the
 * background scanner's path-stitch so a fresh winelib Element finds
 * Windows plugins out of the box.
 *
 * Conservative path list — Common Files\VST2 and Common Files\VST3
 * are the standard installer targets; Steinberg/VstPlugins covers
 * older or non-standard installs.  Both Program Files (64-bit) and
 * Program Files (x86) (32-bit) variants are included since plugins
 * install into the matching arch's hive.
 */
void addWinePluginPaths (juce::FileSearchPath& path, const juce::String& formatName)
{
#if defined (__WINE__)
    const juce::String prefix = juce::SystemStats::getEnvironmentVariable ("WINEPREFIX", {});
    if (prefix.isEmpty())
        return;

    auto addIfExists = [&path] (const juce::String& p)
    {
        const juce::File dir (p);
        if (dir.isDirectory())
            path.addIfNotAlreadyThere (dir);
    };

    if (formatName == "VST")
    {
        addIfExists (prefix + "/drive_c/Program Files/Common Files/VST2");
        addIfExists (prefix + "/drive_c/Program Files (x86)/Common Files/VST2");
        addIfExists (prefix + "/drive_c/Program Files/Steinberg/VstPlugins");
        addIfExists (prefix + "/drive_c/Program Files (x86)/Steinberg/VstPlugins");
        addIfExists (prefix + "/drive_c/Program Files/Common Files/VST");
        addIfExists (prefix + "/drive_c/Program Files (x86)/Common Files/VST");
    }
    else if (formatName == "VST3")
    {
        addIfExists (prefix + "/drive_c/Program Files/Common Files/VST3");
        addIfExists (prefix + "/drive_c/Program Files (x86)/Common Files/VST3");
    }
#else
    juce::ignoreUnused (path, formatName);
#endif
}

} // namespace Util
} // namespace element
