// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/ui/nodeeditor.hpp>

namespace element {

class TrackerNode;

/** Pattern grid editor for the Tracker node. Renoise-style:
 *  - Dark monospace grid
 *  - Hex row numbers in left gutter
 *  - Colored track header bars
 *  - Beat-highlight every 4 rows
 *  - Cells render as `C-5 64` (note + velocity); empty = `--- --`
 *  - Playhead row highlighted while transport is playing
 *  - Edit cursor with arrow-key navigation
 *  - QWERTY note input (tracker piano layout) when edit mode is on
 *
 *  Caps Lock toggles edit mode. Arrows move the cursor; PageUp/PageDown
 *  jump 16 rows. 1-9 set octave. Delete clears the cell at cursor. */
class TrackerEditor : public NodeEditor,
                      private juce::Timer
{
public:
    explicit TrackerEditor (const Node& node);
    ~TrackerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    class PatternView;
    std::unique_ptr<PatternView> patternView;
};

} // namespace element
