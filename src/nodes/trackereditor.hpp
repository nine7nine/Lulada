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
    int   getRpb() const;
    void  changeRpb (int delta);
    int   getTrackCount() const;
    void  addTrack();
    void  deleteCurrentTrack();
    int   getPatternIndex() const;
    int   getPatternCount() const;
    void  newPattern();
    void  duplicatePattern();
    void  deletePattern();
    void  switchPattern (int delta);
    float getBPM() const;
    void  toggleHelp();
    bool  getFollowPlayhead() const;
    void  toggleFollowPlayhead();
    void  undoOp();
    void  redoOp();
    bool  canUndo() const;
    bool  canRedo() const;

private:
    void timerCallback() override;
    void refreshToolbar();

    class PatternView;
    class Toolbar;

    std::unique_ptr<PatternView> patternView;
    std::unique_ptr<juce::Viewport> viewport;
    std::unique_ptr<Toolbar> toolbar;

    /* Toolbar diff state — gates refreshToolbar() inside the 30Hz
     * timerCallback so we don't keep rebuilding label strings + button
     * states on idle frames.  Sentinels force first-tick refresh. */
    int   lastToolbarPatternIndex_ = -1;
    int   lastToolbarPatternCount_ = -1;
    float lastToolbarBpm_          = -1.0f;
    bool  lastToolbarEditMode_     = false;
    int   lastToolbarOctave_       = -1;
    int   lastToolbarEditStep_     = -1;
    int   lastToolbarPatternLength_= -1;
    int   lastToolbarRpb_          = -1;
    int   lastToolbarTrackCount_   = -1;
    bool  lastToolbarFollow_       = false;
    bool  lastToolbarCanUndo_      = false;
    bool  lastToolbarCanRedo_      = false;
};

} // namespace element
