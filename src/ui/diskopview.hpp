// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce.hpp>
#include <element/ui/content.hpp>

#define EL_VIEW_DISK_OP "DiskOpView"

namespace element {

/** Embedded main-window file-ops page.  Mirrors FT2's Disk Op idea
 *  (single nav target for all file IO across the app) and replaces
 *  JUCE FileChooser at the affected sites — JUCE's native chooser is
 *  unreliable under winelib, and even non-native loses chrome.
 *
 *  Three modes (radio at top-left):
 *    - Sample        → load WAVs into the sampler's active slot
 *    - Session       → load/save .els session files [Stage 2]
 *    - Plugin Paths  → manage CLAP/VST/VST3 scan paths  [Stage 2]
 *
 *  Path bar handles Linux + Win32 forms transparently via juce::File.
 *  Quick-Nav row enumerates ~/.wine/dosdevices/ so the user can jump
 *  between Linux paths and Wine drive letters with one click. */
class DiskOpContentView : public ContentView,
                          public juce::ChangeListener
{
public:
    DiskOpContentView();
    ~DiskOpContentView() override;

    void initializeView (Services&) override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void didBecomeActive() override;
    void willBeRemoved() override;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace element
