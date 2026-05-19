// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/ui/nodeeditor.hpp>

namespace element {

class TrackerNode;

/** Pattern grid editor for the Tracker node.  Renoise-style:
 *  - Dark monospace grid + hex row numbers
 *  - Coloured track header bars w/ MIDI channel + click-to-mute
 *  - Playhead row highlight while transport plays
 *  - Edit cursor (sub-column-aware: note / vel-hi / vel-lo)
 *  - QWERTY note input + hex velocity input
 *  - Multi-pattern: Ctrl+N adds, Ctrl+PageUp/Dn switches
 *  - Top toolbar exposes the keyboard ops as visible buttons
 *
 *  ESC toggles edit mode.  F1 shows full keybindings. */
class TrackerEditor : public NodeEditor,
                      private juce::Timer
{
public:
    explicit TrackerEditor (const Node& node);
    ~TrackerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /* Bridge methods used by the toolbar.  Delegate to PatternView /
     * TrackerNode under engineLock_. */
    bool  isEditMode() const;
    void  toggleEditMode();
    int   getOctave() const;
    void  changeOctave (int delta);
    int   getEditStep() const;
    void  changeEditStep (int delta);
    int   getPatternLength() const;
    void  changePatternLength (int delta);
    int   getTrackCount() const;
    void  addTrack();
    void  deleteCurrentTrack();
    int   getPatternIndex() const;
    int   getPatternCount() const;
    void  newPattern();
    void  switchPattern (int delta);
    float getBPM() const;
    void  toggleHelp();

private:
    void timerCallback() override;
    void refreshToolbar();

    class PatternView;
    class Toolbar;

    std::unique_ptr<PatternView> patternView;
    std::unique_ptr<juce::Viewport> viewport;
    std::unique_ptr<Toolbar> toolbar;
};

} // namespace element
