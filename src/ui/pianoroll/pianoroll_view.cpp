// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/pianoroll/pianoroll_view.hpp"
#include "ui/pianoroll/pianoroll_keyboard.hpp"
#include "ui/pianoroll/pianoroll_grid.hpp"
#include "ui/fontcache.hpp"

#include "services/timeline/midi_note_region.hpp"

namespace element {

/* Top-edge drag handle.  Vertical-only resize -- forwards the pixel
 * delta to the dock's onResizeDrag callback so StandardContent owns
 * the height field + layout invalidation.  Mirror of
 * TrackerSideDock::DragHandle rotated 90 degrees. */
class PianoRollView::DragHandle : public juce::Component
{
public:
    DragHandle (PianoRollView& d) : dock_ (d)
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff'1a'1a'1a));
        g.setColour (juce::Colour (0xff'3a'3a'3a));
        g.drawHorizontalLine (getHeight() / 2,
                               0.0f, (float) getWidth());
        g.setColour (juce::Colour (0xff'5a'5a'5a));
        const int cx = getWidth() / 2;
        const int cy = getHeight() / 2;
        for (int i = -1; i <= 1; ++i)
            g.fillEllipse ((float) (cx + i * 8) - 1.0f,
                            (float) cy - 1.0f, 2.0f, 2.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStartY_ = e.getScreenY();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        const int dy = e.getScreenY() - dragStartY_;
        dragStartY_ = e.getScreenY();
        if (dock_.onResizeDrag) dock_.onResizeDrag (dy);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dock_.onResizeDragEnd) dock_.onResizeDragEnd();
    }

private:
    PianoRollView& dock_;
    int dragStartY_ { 0 };
};

PianoRollView::PianoRollView (Services& services)
    : services_ (services)
{
    dragHandle_ = std::make_unique<DragHandle> (*this);
    addAndMakeVisible (*dragHandle_);

    regionLabel_.setJustificationType (juce::Justification::centredLeft);
    regionLabel_.setColour (juce::Label::textColourId,
                              juce::Colours::white.withAlpha (0.85f));
    regionLabel_.setFont (monoFont (11.0f, juce::Font::plain));
    refreshLabel();
    addAndMakeVisible (regionLabel_);

    /* Tool palette: radio toggle.  Buttons are toggleable; clicking
     * one sets the active tool and refreshes the visual toggle state
     * on all three.  Default Select. */
    auto initToolBtn = [this] (juce::TextButton& btn, Tool t) {
        btn.setClickingTogglesState (true);
        btn.setRadioGroupId (0); /* manual radio handling below */
        btn.onClick = [this, t]() { setActiveTool (t); };
        addAndMakeVisible (btn);
    };
    initToolBtn (selectBtn_, Tool::Select);
    initToolBtn (pencilBtn_, Tool::Pencil);
    initToolBtn (eraseBtn_,  Tool::Erase);
    syncToolToggles();

    addAndMakeVisible (closeBtn_);
    closeBtn_.setTooltip ("Hide piano-roll dock");
    closeBtn_.onClick = [this]() {
        if (onCloseClicked) onCloseClicked();
    };

    keyboard_ = std::make_unique<PianoRollKeyboard>();
    addAndMakeVisible (*keyboard_);

    grid_ = std::make_unique<PianoRollGrid> (*this, services_);

    /* Wrap the grid in a juce::Viewport so the user can horizontally
     * scroll through long regions (e.g. a 424-beat .mid import that
     * doesn't fit in the dock's visible width at the default zoom).
     * Vertical scrollbar is disabled because the pitch range is fixed
     * to the keyboard column's [36, 96] span, which is laid out to
     * fill the dock height. */
    gridViewport_ = std::make_unique<juce::Viewport>();
    gridViewport_->setScrollBarsShown (false /*vertical*/, true /*horizontal*/);
    gridViewport_->setViewedComponent (grid_.get(), false /*deleteOnDelete*/);
    addAndMakeVisible (*gridViewport_);
}

PianoRollView::~PianoRollView() = default;

void PianoRollView::setRegion (const juce::Uuid& regionId)
{
    if (regionId == boundRegionId_) return;
    boundRegionId_ = regionId;
    refreshLabel();
    if (grid_ != nullptr)
        grid_->boundRegionChanged();
    repaint();
}

void PianoRollView::setActiveTool (Tool t)
{
    if (t == activeTool_) return;
    activeTool_ = t;
    syncToolToggles();
    if (grid_ != nullptr)
        grid_->activeToolChanged();
}

void PianoRollView::syncToolToggles()
{
    selectBtn_.setToggleState (activeTool_ == Tool::Select, juce::dontSendNotification);
    pencilBtn_.setToggleState (activeTool_ == Tool::Pencil, juce::dontSendNotification);
    eraseBtn_ .setToggleState (activeTool_ == Tool::Erase,  juce::dontSendNotification);
}

void PianoRollView::refreshLabel()
{
    juce::String text ("Piano Roll -- no region bound");
    if (! boundRegionId_.isNull())
    {
        /* Prefer the region's name (.mid file basename, set by
         * importMidiFileToLane).  Falls back to the uuid prefix if
         * the region can't be resolved or is unnamed. */
        juce::String name;
        if (regionResolver_)
        {
            if (auto* region = regionResolver_ (boundRegionId_))
            {
                if (region->name.isNotEmpty())
                    name = region->name;
            }
        }
        if (name.isEmpty())
            name = boundRegionId_.toString().substring (0, 8);
        text = "MIDI -- " + name;
    }
    regionLabel_.setText (text, juce::dontSendNotification);
}

void PianoRollView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff'0a'0a'0a));

    g.setColour (juce::Colour (0xff'0c'0c'0c));
    g.fillRect (0, kDragHandleH, getWidth(), kHeaderH);
}

void PianoRollView::resized()
{
    auto r = getLocalBounds();
    dragHandle_->setBounds (r.removeFromTop (kDragHandleH));

    auto header = r.removeFromTop (kHeaderH);
    closeBtn_  .setBounds (header.removeFromRight (kHeaderH).reduced (3));
    header.removeFromRight (4);

    /* Tool palette occupies the right portion of the header next to
     * the close button.  Three equal-width slots; the rest of the
     * header is the region label on the left. */
    auto toolArea = header.removeFromRight (kToolBtnW * 3 + 8).reduced (2);
    eraseBtn_  .setBounds (toolArea.removeFromRight (kToolBtnW));
    toolArea.removeFromRight (2);
    pencilBtn_ .setBounds (toolArea.removeFromRight (kToolBtnW));
    toolArea.removeFromRight (2);
    selectBtn_ .setBounds (toolArea.removeFromRight (kToolBtnW));

    regionLabel_.setBounds (header.reduced (4, 2));

    /* Body: keyboard column on the left, grid viewport fills the
     * rest.  PianoRollGrid sets its own actual width (region span *
     * kPxPerBeat) so the viewport's horizontal scrollbar engages
     * when the content extends past the visible area. */
    if (keyboard_ != nullptr)
        keyboard_->setBounds (r.removeFromLeft (kKeyboardW));
    if (gridViewport_ != nullptr)
        gridViewport_->setBounds (r);

    /* Grid height tracks the viewport's inner height so each pitch
     * row stays aligned with the keyboard.  Width is updated by the
     * grid itself in updateSizeForRegion (called from
     * boundRegionChanged + when zoom changes). */
    if (grid_ != nullptr && gridViewport_ != nullptr)
        grid_->updateSizeForViewport (gridViewport_->getMaximumVisibleWidth(),
                                       gridViewport_->getMaximumVisibleHeight());
}

} // namespace element
