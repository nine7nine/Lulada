// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ElementApp.h"
#include "ui/buttons.hpp"
#include "ui/lcdsublabel.hpp"
#include <element/audioengine.hpp>
#include <element/session.hpp>

namespace element {

class BarLabel;
class TransportBar : public Component,
                     private Button::Listener,
                     private Timer
{
public:
    TransportBar();
    ~TransportBar();

    void setBeatTime (const float t);
    void updateWidth();
    void stabilize();

    /** Hide the bar / beat / sub labels and lay out as buttons-only.
     *  Used by the redesigned Content::Toolbar where the position
     *  read-out moved into MainDisplayPanel; TransportBar then reads
     *  as a tight transport-button cluster on the left. */
    void setShowPositionLabels (bool show);

    void paint (Graphics& g) override;
    void resized() override;

private:
    SessionPtr session;
    AudioEnginePtr engine;
    Transport::MonitorPtr monitor;

    /* Transport buttons use the BlockToolButton family so the
     * Play/Stop/Record/SeekZero icons render in the same vector-glyph
     * style as the view-selector buttons (consistent silhouette,
     * padding, gradient). */
    std::unique_ptr<juce::Button> play, stop, record, toZero;
    std::unique_ptr<DragableIntLabel> barLabel, beatLabel, subLabel;
    bool showPositionLabels_ { true };

    /* LCD-style sub-labels under each transport button.  Match the
     * left-cluster pattern in Content::Toolbar -- decorative for now,
     * will become spill-over menu triggers when the legacy menubar
     * is removed. */
    LcdSublabel playLbl_   { "PLAY" };
    LcdSublabel stopLbl_   { "STOP" };
    LcdSublabel recordLbl_ { "REC"  };
    LcdSublabel toZeroLbl_ { "REW"  };

    friend class BarLabel;
    friend class Timer;

    void buttonClicked (Button* buttonThatWasClicked) override;
    void timerCallback() override;

    bool checkForMonitor();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TransportBar)
};

} // namespace element
