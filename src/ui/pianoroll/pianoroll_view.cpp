// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/pianoroll/pianoroll_view.hpp"
#include "ui/pianoroll/pianoroll_keyboard.hpp"
#include "ui/fontcache.hpp"

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
        /* Faint horizontal line + 3 inline dots -- same visual idiom
         * as the tracker dock's vertical drag bar, rotated. */
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
        /* Positive delta = user dragging DOWN (dock shrinks).
         * Negative delta = user dragging UP (dock grows).
         * StandardContent inverts this for its height state. */
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

    /* Header: region label on the left, close X on the right.  The
     * label reads "MIDI -- <name>" once a region is bound; falls back
     * to a static hint when no region is bound. */
    regionLabel_.setJustificationType (juce::Justification::centredLeft);
    regionLabel_.setColour (juce::Label::textColourId,
                              juce::Colours::white.withAlpha (0.85f));
    regionLabel_.setFont (monoFont (11.0f, juce::Font::plain));
    refreshLabel();
    addAndMakeVisible (regionLabel_);

    addAndMakeVisible (closeBtn_);
    closeBtn_.setTooltip ("Hide piano-roll dock");
    closeBtn_.onClick = [this]() {
        if (onCloseClicked) onCloseClicked();
    };

    keyboard_ = std::make_unique<PianoRollKeyboard>();
    addAndMakeVisible (*keyboard_);
}

PianoRollView::~PianoRollView() = default;

void PianoRollView::setRegion (const juce::Uuid& regionId)
{
    if (regionId == boundRegionId_) return;
    boundRegionId_ = regionId;
    refreshLabel();
    repaint();
}

void PianoRollView::refreshLabel()
{
    if (boundRegionId_.isNull())
        regionLabel_.setText ("Piano Roll -- no region bound",
                                juce::dontSendNotification);
    else
        regionLabel_.setText ("MIDI -- " + boundRegionId_.toString().substring (0, 8),
                                juce::dontSendNotification);
}

void PianoRollView::paint (juce::Graphics& g)
{
    /* Body background -- matte black faceplate beneath the eventual
     * keyboard column + grid.  Header strip is drawn one step lighter
     * so the affordances read against the body. */
    g.fillAll (juce::Colour (0xff'0a'0a'0a));

    g.setColour (juce::Colour (0xff'0c'0c'0c));
    g.fillRect (0, kDragHandleH, getWidth(), kHeaderH);

    /* Session 1 commits B/C: keyboard column paints itself on the
     * LEFT (juce::MidiKeyboardComponent subclass); the empty-state
     * hint paints in the GRID slot to the right of the keyboard.
     * Commit C replaces the hint with the PianoRollGrid child
     * component (which paints its own empty state when no region
     * resolver / region is bound). */
    const int bodyY = kDragHandleH + kHeaderH;
    const int bodyH = juce::jmax (0, getHeight() - bodyY);
    const int gridX = kKeyboardW;
    const int gridW = juce::jmax (0, getWidth() - gridX);
    if (bodyH > 0 && gridW > 0)
    {
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.setFont (monoFont (12.0f, juce::Font::plain));
        g.drawText ("Double-click a MIDI region to edit.",
                    gridX, bodyY, gridW, bodyH,
                    juce::Justification::centred, false);
    }
}

void PianoRollView::resized()
{
    auto r = getLocalBounds();
    dragHandle_->setBounds (r.removeFromTop (kDragHandleH));

    auto header = r.removeFromTop (kHeaderH);
    closeBtn_   .setBounds (header.removeFromRight (kHeaderH).reduced (3));
    header.removeFromRight (4);
    regionLabel_.setBounds (header.reduced (4, 2));

    /* Body: keyboard column on the left, grid slot on the right.
     * Commit C wires the grid child component into the right slot;
     * for now paint() draws the empty-state hint in that area. */
    if (keyboard_ != nullptr)
        keyboard_->setBounds (r.removeFromLeft (kKeyboardW));
}

} // namespace element
