// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/ui/content.hpp>

namespace element {

class VirtualKeyboardComponent : public MidiKeyboardComponent
{
public:
    VirtualKeyboardComponent (MidiKeyboardState& s, Orientation o);
    ~VirtualKeyboardComponent() {}

    void setKeypressOctaveOffset (int offset);
    int getKeypressOctaveOffset() const { return keypressOctaveOffset; }

    bool keyPressed (const KeyPress&) override;

    /** Override JUCE's default drawBlackNote which paints a
     *  `c.brighter()` highlight cap across the upper 7/8 of every
     *  black key.  That cap reads as grey-on-grey under the dark
     *  palette (since brightening pure black still yields a dark
     *  grey).  We just fill the whole black-key rect flat instead. */
    void drawBlackNote (int midiNoteNumber, Graphics&,
                         Rectangle<float> area,
                         bool isDown, bool isOver,
                         Colour noteFillColour) override;

private:
    int keypressOctaveOffset = 6;
};

class VirtualKeyboardView : public ContentView
{
public:
    VirtualKeyboardView();
    virtual ~VirtualKeyboardView();

    void didBecomeActive() override;
    void stabilizeContent() override
    {
        didBecomeActive();
        resized();
    }

    void saveState (PropertiesFile*) override;
    void restoreState (PropertiesFile*) override;

    void paint (Graphics&) override;
    void resized() override;
    bool keyPressed (const KeyPress&) override;
    bool keyStateChanged (bool) override;
    void parentHierarchyChanged() override;
    void visibilityChanged() override;

private:
    std::unique_ptr<VirtualKeyboardComponent> keyboard;
    bool keyboardInitialized = false;
    MidiKeyboardState internalState;
    int keyWidth = 16;

    Label midiChannelLabel;
    Slider midiChannel;
    Label midiProgramLabel;
    Slider midiProgram;

    TextButton sustain;
    TextButton hold;

    Label widthLabel;
    TextButton widthDown;
    TextButton widthUp;
    void setupKeyboard (VirtualKeyboardComponent&);
    void stabilizeWidthControls();
};

} // namespace element
