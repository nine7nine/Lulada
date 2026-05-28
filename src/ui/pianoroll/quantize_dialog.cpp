// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/pianoroll/quantize_dialog.hpp"
#include "ui/pianoroll/pianoroll_view.hpp"
#include "ui/pianoroll/pianoroll_grid.hpp"

#include <element/ui.hpp>

namespace element {

namespace {

constexpr int    kRowH         = 22;
constexpr int    kRowGap       = 4;
constexpr int    kLabelW       = 110;
constexpr int    kSectionGap   = 12;
constexpr int    kFooterH      = 36;
constexpr int    kTabBarH      = 28;
constexpr int    kPad          = 12;
const juce::Colour kAccent     { 0xff'4a'a5'5a };
const juce::Colour kBg         { 0xff'1a'1a'1a };
const juce::Colour kPanel      { 0xff'22'22'22 };

const char* noteLengthLabel (dsp::quantize::NoteLength l) noexcept
{
    using L = dsp::quantize::NoteLength;
    switch (l)
    {
        case L::Whole:        return "1/1";
        case L::Half:         return "1/2";
        case L::Quarter:      return "1/4";
        case L::Eighth:       return "1/8";
        case L::Sixteenth:    return "1/16";
        case L::ThirtySecond: return "1/32";
        case L::SixtyFourth:  return "1/64";
    }
    return "1/16";
}

const char* noteTypeLabel (dsp::quantize::NoteType t) noexcept
{
    using T = dsp::quantize::NoteType;
    switch (t)
    {
        case T::Normal:  return "Normal";
        case T::Dotted:  return "Dotted";
        case T::Triplet: return "Triplet";
    }
    return "Normal";
}

const char* scaleLabel (dsp::quantize::Scale s) noexcept
{
    using S = dsp::quantize::Scale;
    switch (s)
    {
        case S::Major:           return "Major";
        case S::NaturalMinor:    return "Natural Minor";
        case S::HarmonicMinor:   return "Harmonic Minor";
        case S::Dorian:          return "Dorian";
        case S::Phrygian:        return "Phrygian";
        case S::Lydian:          return "Lydian";
        case S::Mixolydian:      return "Mixolydian";
        case S::Locrian:         return "Locrian";
        case S::MajorPentatonic: return "Major Pentatonic";
        case S::MinorPentatonic: return "Minor Pentatonic";
        case S::Chromatic:       return "Chromatic";
        case S::WholeTone:       return "Whole Tone";
    }
    return "Major";
}

const char* rootLabel (int semitone) noexcept
{
    static const char* names[12] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    const int s = ((semitone % 12) + 12) % 12;
    return names[s];
}

/* Style a slider for the dialog -- compact horizontal, value on the
 * right.  Matches the look of audiodeviceselector's sliders. */
void styleSlider (juce::Slider& s, double minV, double maxV, double step)
{
    s.setSliderStyle (juce::Slider::LinearHorizontal);
    s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, kRowH);
    s.setRange (minV, maxV, step);
    s.setColour (juce::Slider::backgroundColourId,        juce::Colour (0xff'30'30'30));
    s.setColour (juce::Slider::trackColourId,             kAccent.withMultipliedBrightness (0.7f));
    s.setColour (juce::Slider::thumbColourId,             kAccent);
    s.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xff'd0'd0'd0));
    s.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff'18'18'18));
    s.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colour (0xff'30'30'30));
}

void styleCombo (juce::ComboBox& c)
{
    c.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff'2c'2c'2c));
    c.setColour (juce::ComboBox::textColourId,       juce::Colour (0xff'd0'd0'd0));
    c.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff'5a'5a'5a));
    c.setColour (juce::ComboBox::arrowColourId,      juce::Colour (0xff'a0'a0'a0));
}

void styleLabel (juce::Label& l, const juce::String& text)
{
    l.setText (text, juce::dontSendNotification);
    l.setFont (juce::Font (juce::FontOptions (12.0f)));
    l.setColour (juce::Label::textColourId, juce::Colour (0xff'b8'b8'b8));
    l.setJustificationType (juce::Justification::centredLeft);
}

void styleTabBtn (juce::TextButton& b, bool active)
{
    b.setColour (juce::TextButton::buttonColourId,
                 active ? kAccent.withMultipliedBrightness (0.6f)
                        : juce::Colour (0xff'2a'2a'2a));
    b.setColour (juce::TextButton::buttonOnColourId,
                 kAccent.withMultipliedBrightness (0.7f));
    b.setColour (juce::TextButton::textColourOnId,  juce::Colour (0xff'ff'ff'ff));
    b.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff'c0'c0'c0));
    b.setToggleState (active, juce::dontSendNotification);
    b.setClickingTogglesState (false);
}

} // namespace

//==============================================================================

QuantizeDialog::QuantizeDialog (PianoRollView& view, Tab initialTab)
    : view_ (view), activeTab_ (initialTab)
{
    /* --- Tab buttons --- */
    auto addTabBtn = [this] (juce::TextButton& b, Tab t)
    {
        b.onClick = [this, t]() { setActiveTab (t); };
        addAndMakeVisible (b);
    };
    addTabBtn (tabQuantize_, Tab::Quantize);
    addTabBtn (tabHumanize_, Tab::Humanize);
    addTabBtn (tabScale_,    Tab::Scale);

    /* --- Quantize tab --- */
    styleLabel (qNoteLengthLabel_, "Note length");
    addAndMakeVisible (qNoteLengthLabel_);
    using L = dsp::quantize::NoteLength;
    using T = dsp::quantize::NoteType;
    int id = 1;
    for (L l : { L::Whole, L::Half, L::Quarter, L::Eighth,
                  L::Sixteenth, L::ThirtySecond, L::SixtyFourth })
        qNoteLengthCombo_.addItem (noteLengthLabel (l), id++);
    styleCombo (qNoteLengthCombo_);
    qNoteLengthCombo_.addListener (this);
    addAndMakeVisible (qNoteLengthCombo_);

    styleLabel (qNoteTypeLabel_, "Note type");
    addAndMakeVisible (qNoteTypeLabel_);
    qNoteTypeCombo_.addItem (noteTypeLabel (T::Normal),  1);
    qNoteTypeCombo_.addItem (noteTypeLabel (T::Dotted),  2);
    qNoteTypeCombo_.addItem (noteTypeLabel (T::Triplet), 3);
    styleCombo (qNoteTypeCombo_);
    qNoteTypeCombo_.addListener (this);
    addAndMakeVisible (qNoteTypeCombo_);

    styleLabel (qAmountLabel_, "Amount %");
    addAndMakeVisible (qAmountLabel_);
    styleSlider (qAmountSlider_, 0.0, 100.0, 1.0);
    qAmountSlider_.addListener (this);
    addAndMakeVisible (qAmountSlider_);

    styleLabel (qSwingLabel_, "Swing %");
    addAndMakeVisible (qSwingLabel_);
    styleSlider (qSwingSlider_, 0.0, 100.0, 1.0);
    qSwingSlider_.addListener (this);
    addAndMakeVisible (qSwingSlider_);

    styleLabel (qRandomLabel_, "Random beats");
    addAndMakeVisible (qRandomLabel_);
    styleSlider (qRandomSlider_, 0.0, 0.5, 0.001);
    qRandomSlider_.addListener (this);
    addAndMakeVisible (qRandomSlider_);

    qAdjustStart_.setColour (juce::ToggleButton::textColourId, juce::Colour (0xff'd0'd0'd0));
    qAdjustStart_.setColour (juce::ToggleButton::tickColourId, kAccent);
    qAdjustStart_.addListener (this);
    addAndMakeVisible (qAdjustStart_);

    qAdjustEnd_.setColour (juce::ToggleButton::textColourId, juce::Colour (0xff'd0'd0'd0));
    qAdjustEnd_.setColour (juce::ToggleButton::tickColourId, kAccent);
    qAdjustEnd_.addListener (this);
    addAndMakeVisible (qAdjustEnd_);

    /* --- Humanize tab --- */
    styleLabel (hRangeLabel_, "Velocity range");
    addAndMakeVisible (hRangeLabel_);
    styleSlider (hRangeSlider_, 0.0, 64.0, 1.0);
    hRangeSlider_.addListener (this);
    addAndMakeVisible (hRangeSlider_);

    styleLabel (hBiasLabel_, "Velocity bias");
    addAndMakeVisible (hBiasLabel_);
    styleSlider (hBiasSlider_, -32.0, 32.0, 1.0);
    hBiasSlider_.addListener (this);
    addAndMakeVisible (hBiasSlider_);

    hReseedBtn_.setTooltip ("Re-roll humanize jitter with a fresh seed");
    hReseedBtn_.addListener (this);
    addAndMakeVisible (hReseedBtn_);

    /* --- Scale tab --- */
    styleLabel (sScaleLabel_, "Scale");
    addAndMakeVisible (sScaleLabel_);
    {
        int sid = 1;
        for (auto s : { dsp::quantize::Scale::Major,
                         dsp::quantize::Scale::NaturalMinor,
                         dsp::quantize::Scale::HarmonicMinor,
                         dsp::quantize::Scale::Dorian,
                         dsp::quantize::Scale::Phrygian,
                         dsp::quantize::Scale::Lydian,
                         dsp::quantize::Scale::Mixolydian,
                         dsp::quantize::Scale::Locrian,
                         dsp::quantize::Scale::MajorPentatonic,
                         dsp::quantize::Scale::MinorPentatonic,
                         dsp::quantize::Scale::Chromatic,
                         dsp::quantize::Scale::WholeTone })
            sScaleCombo_.addItem (scaleLabel (s), sid++);
    }
    styleCombo (sScaleCombo_);
    sScaleCombo_.addListener (this);
    addAndMakeVisible (sScaleCombo_);

    styleLabel (sRootLabel_, "Root");
    addAndMakeVisible (sRootLabel_);
    for (int i = 0; i < 12; ++i)
        sRootCombo_.addItem (rootLabel (i), i + 1);
    styleCombo (sRootCombo_);
    sRootCombo_.addListener (this);
    addAndMakeVisible (sRootCombo_);

    /* --- Footer buttons --- */
    previewBtn_.setClickingTogglesState (true);
    previewBtn_.setToggleState (true, juce::dontSendNotification);
    previewBtn_.setColour (juce::TextButton::buttonOnColourId,
                            kAccent.withMultipliedBrightness (0.65f));
    previewBtn_.addListener (this);
    addAndMakeVisible (previewBtn_);

    applyBtn_.addListener (this);
    okBtn_.addListener (this);
    cancelBtn_.addListener (this);
    addAndMakeVisible (applyBtn_);
    addAndMakeVisible (okBtn_);
    addAndMakeVisible (cancelBtn_);

    /* Seed widget values from the view's last-used parameters.
     * initialising_ suppresses preview recomputes during this seeding
     * phase so we don't N-times-paint the grid before the user sees
     * anything. */
    initialising_ = true;
    {
        const auto& q = view_.getLastQuantizeOptions();
        qNoteLengthCombo_.setSelectedItemIndex (static_cast<int> (q.noteLength),
                                                  juce::dontSendNotification);
        qNoteTypeCombo_  .setSelectedItemIndex (static_cast<int> (q.noteType),
                                                  juce::dontSendNotification);
        qAmountSlider_.setValue (q.amount * 100.0,                 juce::dontSendNotification);
        qSwingSlider_ .setValue (q.swing  * 100.0,                 juce::dontSendNotification);
        qRandomSlider_.setValue (q.randomBeats,                    juce::dontSendNotification);
        qAdjustStart_ .setToggleState (q.adjustStart,              juce::dontSendNotification);
        qAdjustEnd_   .setToggleState (q.adjustEnd,                juce::dontSendNotification);

        const auto& h = view_.getLastHumanizeOptions();
        hRangeSlider_.setValue ((double) h.velocityRange, juce::dontSendNotification);
        hBiasSlider_ .setValue ((double) h.velocityBias,  juce::dontSendNotification);

        sScaleCombo_.setSelectedItemIndex (static_cast<int> (view_.getLastScale()),
                                            juce::dontSendNotification);
        sRootCombo_.setSelectedId (view_.getLastScaleRoot() + 1,
                                    juce::dontSendNotification);
    }
    initialising_ = false;

    setActiveTab (activeTab_);
    refreshPreview();
    setSize (kPreferredW, kPreferredH);
}

QuantizeDialog::~QuantizeDialog()
{
    clearPreview();
}

void QuantizeDialog::paint (juce::Graphics& g)
{
    g.fillAll (kBg);

    /* Section panel behind the active tab's widgets. */
    auto r = getLocalBounds().reduced (kPad);
    r.removeFromTop (kTabBarH + kPad / 2);
    r.removeFromBottom (kFooterH + kPad / 2);
    g.setColour (kPanel);
    g.fillRoundedRectangle (r.toFloat(), 4.0f);

    /* Tab-bar underline -- thin accent line under the active tab so the
     * user has a strong visual anchor for "what am I editing". */
    juce::Rectangle<int> tabBar = getLocalBounds().reduced (kPad, 0)
                                                  .withTrimmedTop (kPad)
                                                  .withHeight (kTabBarH);
    g.setColour (kAccent.withAlpha (0.5f));
    g.drawHorizontalLine (tabBar.getBottom() - 1,
                           (float) tabBar.getX(), (float) tabBar.getRight());
}

void QuantizeDialog::resized()
{
    auto r = getLocalBounds().reduced (kPad);

    /* Tab bar. */
    auto tabRow = r.removeFromTop (kTabBarH);
    const int tabW = tabRow.getWidth() / 3;
    tabQuantize_.setBounds (tabRow.removeFromLeft (tabW).reduced (1));
    tabHumanize_.setBounds (tabRow.removeFromLeft (tabW).reduced (1));
    tabScale_   .setBounds (tabRow.reduced (1));

    r.removeFromTop (kPad / 2);

    /* Footer. */
    auto footer = r.removeFromBottom (kFooterH).reduced (0, 4);
    const int btnW = 80;
    cancelBtn_ .setBounds (footer.removeFromRight (btnW).reduced (2));
    okBtn_     .setBounds (footer.removeFromRight (btnW).reduced (2));
    applyBtn_  .setBounds (footer.removeFromRight (btnW).reduced (2));
    previewBtn_.setBounds (footer.removeFromLeft  (btnW + 18).reduced (2));

    r.removeFromBottom (kPad / 2);

    /* Tab body -- single column of (label, control) rows.  Each
     * row is kRowH tall + kRowGap below.  Layout is independent of
     * which tab is active; we just hide off-tab widgets. */
    auto body = r.reduced (kPad);

    auto rowOf = [&] (juce::Component& label, juce::Component& control,
                       int controlW = -1)
    {
        if (! label.isVisible() && ! control.isVisible()) return;
        auto row = body.removeFromTop (kRowH);
        label.setBounds (row.removeFromLeft (kLabelW));
        if (controlW > 0)
            control.setBounds (row.removeFromLeft (controlW));
        else
            control.setBounds (row);
        body.removeFromTop (kRowGap);
    };

    if (activeTab_ == Tab::Quantize)
    {
        rowOf (qNoteLengthLabel_, qNoteLengthCombo_, 120);
        rowOf (qNoteTypeLabel_,   qNoteTypeCombo_,   120);
        body.removeFromTop (kSectionGap / 2);
        rowOf (qAmountLabel_, qAmountSlider_);
        rowOf (qSwingLabel_,  qSwingSlider_);
        rowOf (qRandomLabel_, qRandomSlider_);
        body.removeFromTop (kSectionGap / 2);
        auto toggleRow = body.removeFromTop (kRowH);
        qAdjustStart_.setBounds (toggleRow.removeFromLeft (toggleRow.getWidth() / 2));
        qAdjustEnd_  .setBounds (toggleRow);
    }
    else if (activeTab_ == Tab::Humanize)
    {
        rowOf (hRangeLabel_, hRangeSlider_);
        rowOf (hBiasLabel_,  hBiasSlider_);
        body.removeFromTop (kSectionGap);
        auto reseed = body.removeFromTop (kRowH);
        hReseedBtn_.setBounds (reseed.removeFromLeft (96));
    }
    else /* Scale */
    {
        rowOf (sScaleLabel_, sScaleCombo_, 200);
        rowOf (sRootLabel_,  sRootCombo_,   80);
    }
}

void QuantizeDialog::setActiveTab (Tab t)
{
    activeTab_ = t;

    styleTabBtn (tabQuantize_, t == Tab::Quantize);
    styleTabBtn (tabHumanize_, t == Tab::Humanize);
    styleTabBtn (tabScale_,    t == Tab::Scale);

    const bool q = (t == Tab::Quantize);
    qNoteLengthLabel_.setVisible (q);
    qNoteLengthCombo_.setVisible (q);
    qNoteTypeLabel_  .setVisible (q);
    qNoteTypeCombo_  .setVisible (q);
    qAmountLabel_    .setVisible (q);
    qAmountSlider_   .setVisible (q);
    qSwingLabel_     .setVisible (q);
    qSwingSlider_    .setVisible (q);
    qRandomLabel_    .setVisible (q);
    qRandomSlider_   .setVisible (q);
    qAdjustStart_    .setVisible (q);
    qAdjustEnd_      .setVisible (q);

    const bool h = (t == Tab::Humanize);
    hRangeLabel_   .setVisible (h);
    hRangeSlider_  .setVisible (h);
    hBiasLabel_    .setVisible (h);
    hBiasSlider_   .setVisible (h);
    hReseedBtn_    .setVisible (h);

    const bool s = (t == Tab::Scale);
    sScaleLabel_.setVisible (s);
    sScaleCombo_.setVisible (s);
    sRootLabel_ .setVisible (s);
    sRootCombo_ .setVisible (s);

    resized();
    refreshPreview();
}

void QuantizeDialog::buttonClicked (juce::Button* b)
{
    if (b == &tabQuantize_) { setActiveTab (Tab::Quantize); return; }
    if (b == &tabHumanize_) { setActiveTab (Tab::Humanize); return; }
    if (b == &tabScale_)    { setActiveTab (Tab::Scale);    return; }

    if (b == &previewBtn_)
    {
        previewEnabled_ = previewBtn_.getToggleState();
        if (! previewEnabled_) clearPreview();
        else                   refreshPreview();
        return;
    }

    if (b == &qAdjustStart_ || b == &qAdjustEnd_)
    {
        if (! initialising_) refreshPreview();
        return;
    }

    if (b == &hReseedBtn_)
    {
        /* Bump the humanize seed to a fresh value so the next preview /
         * apply re-rolls the jitter pattern.  juce::Random gives us a
         * non-zero 64-bit number deterministically per-call. */
        juce::Random r;
        std::uint64_t seed = static_cast<std::uint64_t> (r.nextInt64());
        if (seed == 0) seed = 1;
        auto h = view_.getLastHumanizeOptions();
        h.seed = seed;
        view_.setLastHumanizeOptions (h);
        refreshPreview();
        return;
    }

    if (b == &applyBtn_)  { applyActive (false); return; }
    if (b == &okBtn_)     { applyActive (true);  return; }
    if (b == &cancelBtn_)
    {
        clearPreview();
        /* Defer the close: onCloseRequested may hide / destroy us.
         * Run via callAsync so the button callback can unwind first. */
        juce::MessageManager::callAsync ([cb = onCloseRequested]() {
            if (cb) cb();
        });
        return;
    }
}

void QuantizeDialog::switchTab (Tab t)
{
    if (t == activeTab_) return;
    setActiveTab (t);
}

void QuantizeDialog::sliderValueChanged (juce::Slider*)
{
    if (initialising_) return;
    refreshPreview();
}

void QuantizeDialog::comboBoxChanged (juce::ComboBox*)
{
    if (initialising_) return;
    refreshPreview();
}

void QuantizeDialog::writeBackParameters() const
{
    /* Mutable view_ access OK: writeBack is invoked from non-const
     * paths (apply / OK / refreshPreview).  const here just keeps the
     * helper out of the listener-API const-warning soup. */
    auto& view = const_cast<PianoRollView&> (view_);

    if (activeTab_ == Tab::Quantize)
    {
        dsp::quantize::QuantizeOptions q;
        q.noteLength  = static_cast<dsp::quantize::NoteLength> (
                            juce::jmax (0, qNoteLengthCombo_.getSelectedItemIndex()));
        q.noteType    = static_cast<dsp::quantize::NoteType> (
                            juce::jmax (0, qNoteTypeCombo_.getSelectedItemIndex()));
        q.amount      = qAmountSlider_.getValue() / 100.0;
        q.swing       = qSwingSlider_ .getValue() / 100.0;
        q.randomBeats = qRandomSlider_.getValue();
        q.adjustStart = qAdjustStart_.getToggleState();
        q.adjustEnd   = qAdjustEnd_  .getToggleState();
        /* Preserve any previously set seed so reseed survives a
         * preview-recompute that comes from a slider tweak. */
        q.seed        = view_.getLastQuantizeOptions().seed;
        view.setLastQuantizeOptions (q);
    }
    else if (activeTab_ == Tab::Humanize)
    {
        auto h = view_.getLastHumanizeOptions();
        h.velocityRange = (int) hRangeSlider_.getValue();
        h.velocityBias  = (int) hBiasSlider_ .getValue();
        view.setLastHumanizeOptions (h);
    }
    else /* Scale */
    {
        const auto s = static_cast<dsp::quantize::Scale> (
                            juce::jmax (0, sScaleCombo_.getSelectedItemIndex()));
        const int  root = juce::jmax (1, sRootCombo_.getSelectedId()) - 1;
        view.setLastScale (s, root);
    }
}

void QuantizeDialog::refreshPreview()
{
    if (! previewEnabled_) return;

    auto* grid = view_.getGrid();
    if (grid == nullptr) return;

    writeBackParameters();

    std::vector<std::uint64_t> ids;
    if (activeTab_ == Tab::Quantize)
        ids = grid->previewQuantizeIds (view_.getLastQuantizeOptions());
    else if (activeTab_ == Tab::Humanize)
        ids = grid->previewHumanizeIds (view_.getLastHumanizeOptions());
    else
        ids = grid->previewScaleIds (view_.getLastScale(),
                                      view_.getLastScaleRoot());

    grid->setPreviewAffectedNotes (std::move (ids));
}

void QuantizeDialog::applyActive (bool closeAfter)
{
    auto* grid = view_.getGrid();
    if (grid == nullptr)
    {
        if (closeAfter)
            juce::MessageManager::callAsync ([cb = onCloseRequested]() {
                if (cb) cb();
            });
        return;
    }

    writeBackParameters();

    std::size_t touched = 0;
    if (activeTab_ == Tab::Quantize)
        touched = grid->applyQuantize (view_.getLastQuantizeOptions());
    else if (activeTab_ == Tab::Humanize)
        touched = grid->applyHumanize (view_.getLastHumanizeOptions());
    else
        touched = grid->applyScale (view_.getLastScale(), view_.getLastScaleRoot());

    if (onApplied) onApplied (touched);

    if (closeAfter)
    {
        clearPreview();
        /* Defer the close hook so we don't return into a freed panel. */
        juce::MessageManager::callAsync ([cb = onCloseRequested]() {
            if (cb) cb();
        });
    }
    else
    {
        /* Re-snapshot the preview now that the diff has been committed
         * (most ids will no longer trigger -- they're already on-grid). */
        refreshPreview();
    }
}

void QuantizeDialog::clearPreview()
{
    if (auto* grid = view_.getGrid())
        grid->clearPreviewAffectedNotes();
}

} // namespace element
