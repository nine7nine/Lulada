// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dsp/quantize_options.hpp"

#include <element/juce/gui_basics.hpp>

#include <functional>
#include <memory>

namespace element {

class PianoRollView;

/** Modal Quantize / Humanize / Scale dialog for the piano-roll editor.
 *
 *  Three tabs, each driving one bulk-edit op against the current
 *  selection on the bound region:
 *
 *    - Quantize : timing-side snap to the chosen note-length grid.
 *                 Controls: NoteLength + NoteType combos, Amount
 *                 slider (0..100%), AdjustStart + AdjustEnd toggles,
 *                 Swing slider (0..100), RandomBeats slider, Reseed
 *                 button.
 *    - Humanize : per-note velocity jitter.  Controls: VelocityRange
 *                 slider, VelocityBias slider, Reseed button.
 *    - Scale    : pitch snap to nearest in-scale tone.  Controls:
 *                 Scale combo (12 entries), Root combo (12 entries
 *                 C..B).  No randomisation -- pitch snap is
 *                 deterministic per (scale, root) pair.
 *
 *  Buttons: Preview (always live), Apply (commit + keep open), OK
 *  (commit + close), Cancel (revert preview + close).  Live preview
 *  is on by default -- every parameter change recomputes the diff
 *  and asks the grid to paint a ghost overlay.
 *
 *  Last-used parameters per tab are stored on PianoRollView so a
 *  fast-apply hotkey (Ctrl+Q) replays the most recent settings
 *  without re-opening the dialog. */
class QuantizeDialog : public juce::Component,
                       private juce::Slider::Listener,
                       private juce::ComboBox::Listener,
                       private juce::Button::Listener
{
public:
    enum class Tab : int { Quantize = 0, Humanize = 1, Scale = 2 };

    /** Construct bound to a piano-roll view.  The dialog reads the
     *  view's last-used parameters for each tab and writes them back
     *  on Apply / OK.  `initialTab` opens the dialog at a specific
     *  tab; the toolbar Q / H / S buttons supply this. */
    QuantizeDialog (PianoRollView& view, Tab initialTab);
    ~QuantizeDialog() override;

    void paint  (juce::Graphics&) override;
    void resized() override;

    /** Run when OK / Apply commits a diff.  Set by the host dialog
     *  window so it can close itself on OK. */
    std::function<void(bool /*closeAfter*/)> onApply;
    std::function<void()>                     onCancel;

    static constexpr int kPreferredW = 480;
    static constexpr int kPreferredH = 340;

private:
    void buttonClicked       (juce::Button*) override;
    void sliderValueChanged  (juce::Slider*) override;
    void comboBoxChanged     (juce::ComboBox*) override;

    /** Recompute the preview diff for the active tab and push it to
     *  the grid.  Cheap (one pass over selection); called on every
     *  parameter change. */
    void refreshPreview();

    /** Apply the active tab's op to the bound region.  Pushes a
     *  MidiNoteDiffCommand through the GuiService UndoManager; if
     *  `closeAfter` is true, also fires onApply(true) which the
     *  dialog window uses to close itself. */
    void applyActive (bool closeAfter);

    /** Discard any live preview overlay -- called on Cancel + on
     *  dtor so the grid never paints stale ghost arrows. */
    void clearPreview();

    /** Switch visible tab.  Hides the off-tab widgets so the layout
     *  stays clean and the user can't tab-key into hidden controls. */
    void setActiveTab (Tab);

    /** Read the active tab's widget state back into the view's
     *  last-used options struct, so the next dialog open / Ctrl+Q
     *  picks up the user's adjustments. */
    void writeBackParameters() const;

    PianoRollView& view_;
    Tab            activeTab_;

    // Tab selector buttons (manual radio).
    juce::TextButton tabQuantize_ { "Quantize" };
    juce::TextButton tabHumanize_ { "Humanize" };
    juce::TextButton tabScale_    { "Scale"    };

    // ---- Quantize tab widgets ----
    juce::Label    qNoteLengthLabel_;
    juce::ComboBox qNoteLengthCombo_;
    juce::Label    qNoteTypeLabel_;
    juce::ComboBox qNoteTypeCombo_;
    juce::Label    qAmountLabel_;
    juce::Slider   qAmountSlider_;
    juce::Label    qSwingLabel_;
    juce::Slider   qSwingSlider_;
    juce::Label    qRandomLabel_;
    juce::Slider   qRandomSlider_;
    juce::ToggleButton qAdjustStart_ { "Adjust note start" };
    juce::ToggleButton qAdjustEnd_   { "Adjust note end"   };

    // ---- Humanize tab widgets ----
    juce::Label    hRangeLabel_;
    juce::Slider   hRangeSlider_;
    juce::Label    hBiasLabel_;
    juce::Slider   hBiasSlider_;
    juce::TextButton hReseedBtn_ { "Reseed" };

    // ---- Scale tab widgets ----
    juce::Label    sScaleLabel_;
    juce::ComboBox sScaleCombo_;
    juce::Label    sRootLabel_;
    juce::ComboBox sRootCombo_;

    // ---- Footer buttons ----
    juce::TextButton previewBtn_ { "Live preview" };
    juce::TextButton applyBtn_   { "Apply" };
    juce::TextButton okBtn_      { "OK" };
    juce::TextButton cancelBtn_  { "Cancel" };

    bool             previewEnabled_ { true };
    bool             initialising_   { true };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (QuantizeDialog)
};

/** DialogWindow wrapper that hosts a QuantizeDialog.  Mirrors the
 *  SessionImportWizardDialog pattern: own a unique_ptr holder, set the
 *  content, centre + show.  Escape closes via Cancel; closing the X
 *  treats it as Cancel. */
class QuantizeDialogWindow : public juce::DialogWindow
{
public:
    QuantizeDialogWindow (std::unique_ptr<juce::Component>& holder,
                          PianoRollView& view,
                          QuantizeDialog::Tab initialTab);
    ~QuantizeDialogWindow() override;

    bool escapeKeyPressed() override;
    void closeButtonPressed() override;

private:
    std::unique_ptr<juce::Component>& holder_;
    QuantizeDialog* content_ { nullptr };
};

} // namespace element
