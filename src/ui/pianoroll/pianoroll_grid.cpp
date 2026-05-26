// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/pianoroll/pianoroll_grid.hpp"
#include "ui/pianoroll/pianoroll_view.hpp"
#include "ui/pianoroll/pianoroll_keyboard.hpp"
#include "ui/pianoroll/note_drag.hpp"
#include "ui/pianoroll/midi_note_diff_command.hpp"
#include "ui/fontcache.hpp"
#include "services/timeline/midi_note_region.hpp"
#include "services/timeline/midi_note.hpp"

#include <element/audioengine.hpp>
#include <element/context.hpp>
#include <element/ui.hpp>

#include <cmath>

namespace element {

namespace {

constexpr int kResizeHandlePx = 5;   /* width of the right-edge resize hot zone */

} // namespace

//==============================================================================

PianoRollGrid::PianoRollGrid (PianoRollView& parent, Services& services)
    : parent_ (parent), services_ (services)
{
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::NormalCursor);

    if (auto* eng = services.context().audio().get())
        monitor_ = eng->getTransportMonitor();

    startTimerHz (30);
}

PianoRollGrid::~PianoRollGrid()
{
    stopTimer();
}

//==============================================================================
// Bound-region helpers.

MidiNoteRegion* PianoRollGrid::resolveBoundRegion() const noexcept
{
    const auto& resolver = parent_.getRegionResolver();
    const auto regionId  = parent_.getBoundRegionId();
    if (! resolver || regionId.isNull()) return nullptr;
    return resolver (regionId);
}

void PianoRollGrid::boundRegionChanged()
{
    /* Clear selection -- a fresh region binding shouldn't carry over
     * note ids from the previous one (different regions have
     * disjoint id namespaces). */
    selectedNoteIds_.clear();

    /* Re-sync the cached region length + recompute auto-fit zoom. */
    regionLenBeats_ = 16.0;
    if (auto* region = resolveBoundRegion())
        regionLenBeats_ = juce::jmax (1.0, region->lengthBeats);

    /* Auto-fit on bind: pick pxPerBeat so the whole region fits in
     * the viewport.  If the user later zooms, that overrides
     * auto-fit. */
    if (auto* vp = findParentComponentOfClass<juce::Viewport>())
    {
        const int visibleW = vp->getMaximumVisibleWidth();
        if (visibleW > 0 && regionLenBeats_ > 0.0)
        {
            const int fit = (int) std::floor ((double) visibleW / regionLenBeats_);
            pxPerBeat_ = juce::jlimit (kPxPerBeatMin, kPxPerBeatMax, fit);
        }
    }

    setSize ((int) std::round (regionLenBeats_ * pxPerBeat_), getHeight());
    repaint();
}

void PianoRollGrid::activeToolChanged()
{
    /* No active gesture state survives a tool change -- be defensive
     * and drop any in-flight drag.  Repaint to update cursor hints. */
    activeDrag_.reset();
    repaint();
}

void PianoRollGrid::updateSizeForViewport (int visibleW, int visibleH)
{
    visibleH_ = juce::jmax (1, visibleH);
    /* Width: max(visibleW, region span at current zoom).  visibleW
     * ensures the grid fills the viewport even if the region is
     * shorter than the visible area. */
    const int spanW = (int) std::round (regionLenBeats_ * pxPerBeat_);
    setSize (juce::jmax (visibleW, spanW), visibleH_);
}

void PianoRollGrid::setPxPerBeat (int newPxPerBeat)
{
    newPxPerBeat = juce::jlimit (kPxPerBeatMin, kPxPerBeatMax, newPxPerBeat);
    if (newPxPerBeat == pxPerBeat_) return;
    pxPerBeat_ = newPxPerBeat;
    const int spanW = (int) std::round (regionLenBeats_ * pxPerBeat_);
    setSize (juce::jmax (spanW, 1), getHeight());
    repaint();
}

void PianoRollGrid::resized()
{
    /* No child layout -- the grid paints directly into its bounds. */
}

void PianoRollGrid::timerCallback()
{
    if (! isShowing()) return;
    repaint();
}

//==============================================================================
// Coordinate helpers.

double PianoRollGrid::beatForX (int x, double lengthBeatsClamp) const noexcept
{
    if (pxPerBeat_ <= 0) return 0.0;
    const double b = (double) x / (double) pxPerBeat_;
    if (lengthBeatsClamp <= 0.0) return juce::jmax (0.0, b);
    return juce::jlimit (0.0, lengthBeatsClamp, b);
}

int PianoRollGrid::pitchForY (int y) const noexcept
{
    auto* kb = parent_.getKeyboard();
    if (kb == nullptr) return 60;
    const int lo = kb->getLowestVisibleNoteNumber();
    const int hi = kb->getHighestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    const float rowH = (float) getHeight() / (float) span;
    if (rowH <= 0.0f) return lo;
    const int row = (int) ((float) y / rowH);
    const int pitch = hi - row;
    return juce::jlimit (lo, hi, pitch);
}

int PianoRollGrid::xForBeat (double localBeat) const noexcept
{
    return (int) std::round (localBeat * pxPerBeat_);
}

int PianoRollGrid::yForPitch (int pitch) const noexcept
{
    auto* kb = parent_.getKeyboard();
    if (kb == nullptr) return 0;
    const int hi = kb->getHighestVisibleNoteNumber();
    const int lo = kb->getLowestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    const float rowH = (float) getHeight() / (float) span;
    return (int) ((float) (hi - pitch) * rowH);
}

int PianoRollGrid::rowHeight() const noexcept
{
    auto* kb = parent_.getKeyboard();
    if (kb == nullptr) return 1;
    const int hi = kb->getHighestVisibleNoteNumber();
    const int lo = kb->getLowestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    return juce::jmax (1, getHeight() / span);
}

double PianoRollGrid::snapBeat (double localBeat) const noexcept
{
    /* v1: snap to nearest beat (quarter note).  Future enhancement
     * reads a per-view snap setting (1/8, 1/16, ...). */
    return std::round (localBeat);
}

juce::Rectangle<int> PianoRollGrid::visibleRect() const noexcept
{
    /* If we're inside a Viewport, use its view-area; otherwise the
     * grid's full bounds. */
    if (auto* vp = findParentComponentOfClass<juce::Viewport>())
    {
        const int x = vp->getViewPositionX();
        const int y = vp->getViewPositionY();
        const int w = vp->getMaximumVisibleWidth();
        const int h = vp->getMaximumVisibleHeight();
        return { x, y, w, h };
    }
    return getLocalBounds();
}

//==============================================================================
// Selection.

void PianoRollGrid::selectOnly (std::uint64_t noteId)
{
    selectedNoteIds_.clear();
    if (noteId != 0) selectedNoteIds_.insert (noteId);
    repaint();
}

void PianoRollGrid::selectToggle (std::uint64_t noteId)
{
    if (noteId == 0) return;
    auto it = selectedNoteIds_.find (noteId);
    if (it == selectedNoteIds_.end())
        selectedNoteIds_.insert (noteId);
    else
        selectedNoteIds_.erase (it);
    repaint();
}

void PianoRollGrid::selectAdd (std::uint64_t noteId)
{
    if (noteId == 0) return;
    selectedNoteIds_.insert (noteId);
    repaint();
}

void PianoRollGrid::selectClear()
{
    if (selectedNoteIds_.empty()) return;
    selectedNoteIds_.clear();
    repaint();
}

//==============================================================================
// Paint.

void PianoRollGrid::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff'0a'0a'0a));

    auto* region = resolveBoundRegion();
    if (region != nullptr && juce::jmax (1.0, region->lengthBeats) != regionLenBeats_)
    {
        /* Region length changed since last paint (e.g. user resized
         * a region in the arrangement view).  Re-sync component width
         * so the horizontal scrollbar tracks correctly.  Done from
         * paint() rather than a separate observer because the
         * resolver lookup happens here anyway. */
        regionLenBeats_ = juce::jmax (1.0, region->lengthBeats);
        if (auto* vp = findParentComponentOfClass<juce::Viewport>())
            updateSizeForViewport (vp->getMaximumVisibleWidth(),
                                    vp->getMaximumVisibleHeight());
    }

    const int beatsPerBar = monitor_ != nullptr
        ? juce::jmax (1, (int) monitor_->beatsPerBar.get())
        : 4;
    paintBarGrid (g, beatsPerBar);

    if (region == nullptr)
    {
        paintEmptyState (g);
        return;
    }

    paintNotes (g, *region);
    paintActiveDragOverlay (g);
}

void PianoRollGrid::paintEmptyState (juce::Graphics& g)
{
    g.setColour (juce::Colours::white.withAlpha (0.35f));
    g.setFont (monoFont (12.0f, juce::Font::plain));
    g.drawText ("Double-click a MIDI region to edit.",
                visibleRect(),
                juce::Justification::centred,
                false);
}

void PianoRollGrid::paintBarGrid (juce::Graphics& g, int beatsPerBar)
{
    const auto vr = visibleRect();
    const int h = getHeight();
    if (vr.getWidth() <= 0 || h <= 0) return;

    const int startBeat = juce::jmax (0, vr.getX() / juce::jmax (1, pxPerBeat_));
    const int endBeat   = (vr.getRight() + pxPerBeat_ - 1) / juce::jmax (1, pxPerBeat_);

    g.setColour (juce::Colour (0xff'18'18'18));
    for (int beat = startBeat; beat <= endBeat; ++beat)
    {
        if (beat % beatsPerBar == 0) continue;
        const int x = beat * pxPerBeat_;
        g.drawVerticalLine (x, 0.0f, (float) h);
    }

    g.setColour (juce::Colour (0xff'2a'2a'2a));
    const int firstBar = (startBeat / beatsPerBar) * beatsPerBar;
    for (int beat = firstBar; beat <= endBeat; beat += beatsPerBar)
    {
        const int x = beat * pxPerBeat_;
        g.drawVerticalLine (x, 0.0f, (float) h);
    }

    /* Pitch row separators -- octave boundaries brighter. */
    auto* kb = parent_.getKeyboard();
    if (kb != nullptr)
    {
        const int lo = kb->getLowestVisibleNoteNumber();
        const int hi = kb->getHighestVisibleNoteNumber();
        const int span = juce::jmax (1, hi - lo + 1);
        const float rowH = (float) h / (float) span;

        for (int p = lo; p <= hi; ++p)
        {
            const float y = (float) (hi - p) * rowH;
            g.setColour ((p % 12) == 0
                            ? juce::Colour (0xff'22'22'22)
                            : juce::Colour (0xff'14'14'14));
            g.drawHorizontalLine ((int) y,
                                    (float) vr.getX(),
                                    (float) vr.getRight());
        }
    }
}

void PianoRollGrid::paintNotes (juce::Graphics& g, const MidiNoteRegion& region)
{
    const auto* snap = region.loadSnapshot();
    if (snap == nullptr || snap->empty()) return;

    auto* kb = parent_.getKeyboard();
    if (kb == nullptr) return;

    const int lo = kb->getLowestVisibleNoteNumber();
    const int hi = kb->getHighestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    const float rowH = (float) getHeight() / (float) span;

    const auto vr = visibleRect();

    const juce::Colour noteFill = region.colour
                                          .withMultipliedBrightness (1.2f);
    const juce::Colour noteEdge = region.colour
                                          .withMultipliedBrightness (1.6f);
    const juce::Colour selFill  = noteFill.brighter (0.5f);
    const juce::Colour selEdge  = juce::Colours::white;

    for (const auto& n : *snap)
    {
        if (n.pitch < lo || n.pitch > hi) continue;

        const int x  = (int) std::round (n.onBeat * pxPerBeat_);
        const int w  = juce::jmax (2, (int) std::round (n.lengthBeats * pxPerBeat_));
        const int y  = (int) ((float) (hi - n.pitch) * rowH);
        const int hh = juce::jmax (1, (int) rowH - 1);

        const juce::Rectangle<int> rect (x, y, w, hh);
        if (! vr.intersects (rect)) continue;

        const bool selected = isSelected (n.id);
        g.setColour (selected ? selFill : noteFill);
        g.fillRect (rect);
        g.setColour (selected ? selEdge : noteEdge);
        g.drawRect (rect, selected ? 2 : 1);
    }
}

void PianoRollGrid::paintActiveDragOverlay (juce::Graphics& g)
{
    if (activeDrag_ == nullptr) return;
    activeDrag_->paintOverlay (g, *this);
}

//==============================================================================
// Hit test.

std::uint64_t PianoRollGrid::hitTestNote (int x, int y,
                                            const MidiNoteRegion& region) const noexcept
{
    const auto* snap = region.loadSnapshot();
    if (snap == nullptr || snap->empty()) return 0;

    auto* kb = parent_.getKeyboard();
    if (kb == nullptr) return 0;

    const int lo = kb->getLowestVisibleNoteNumber();
    const int hi = kb->getHighestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    const float rowH = (float) getHeight() / (float) span;

    /* Iterate in reverse so notes painted on top (later in the
     * sorted snapshot if any overlap) win the hit. */
    for (auto it = snap->rbegin(); it != snap->rend(); ++it)
    {
        const auto& n = *it;
        if (n.pitch < lo || n.pitch > hi) continue;
        const int nx = (int) std::round (n.onBeat * pxPerBeat_);
        const int nw = juce::jmax (2, (int) std::round (n.lengthBeats * pxPerBeat_));
        const int ny = (int) ((float) (hi - n.pitch) * rowH);
        const int nh = juce::jmax (1, (int) rowH - 1);
        if (x >= nx && x < nx + nw && y >= ny && y < ny + nh)
            return n.id;
    }
    return 0;
}

std::uint64_t PianoRollGrid::hitTestResizeHandle (int x, int y,
                                                    const MidiNoteRegion& region) const noexcept
{
    const auto* snap = region.loadSnapshot();
    if (snap == nullptr || snap->empty()) return 0;

    auto* kb = parent_.getKeyboard();
    if (kb == nullptr) return 0;

    const int lo = kb->getLowestVisibleNoteNumber();
    const int hi = kb->getHighestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    const float rowH = (float) getHeight() / (float) span;

    for (auto it = snap->rbegin(); it != snap->rend(); ++it)
    {
        const auto& n = *it;
        if (n.pitch < lo || n.pitch > hi) continue;
        const int nx = (int) std::round (n.onBeat * pxPerBeat_);
        const int nw = juce::jmax (2, (int) std::round (n.lengthBeats * pxPerBeat_));
        const int ny = (int) ((float) (hi - n.pitch) * rowH);
        const int nh = juce::jmax (1, (int) rowH - 1);
        /* Right-edge handle: within kResizeHandlePx of nx+nw, in
         * the note's Y band. */
        if (x >= nx + nw - kResizeHandlePx && x < nx + nw + 1
            && y >= ny && y < ny + nh)
            return n.id;
    }
    return 0;
}

//==============================================================================
// Mouse handlers.

void PianoRollGrid::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();

    auto* region = resolveBoundRegion();
    if (region == nullptr) return;

    /* First try the resize handle hit (5 px right edge of any note)
     * regardless of tool -- consistent affordance like all DAWs.
     * Then fall back to body hit + branch on active tool. */
    if (parent_.getActiveTool() == PianoRollView::Tool::Select)
    {
        if (auto hitResize = hitTestResizeHandle (e.x, e.y, *region))
        {
            if (! isSelected (hitResize))
                selectOnly (hitResize);
            activeDrag_ = NoteDrag::makeResize (*this, *region, hitResize, e);
            return;
        }
    }

    const auto hitId = hitTestNote (e.x, e.y, *region);

    if (hitId != 0)
    {
        /* Selection update before drag setup so the drag captures the
         * full set in motion. */
        if (e.mods.isCommandDown() || e.mods.isCtrlDown())
            selectToggle (hitId);
        else if (! isSelected (hitId))
            selectOnly (hitId);

        if (parent_.getActiveTool() == PianoRollView::Tool::Erase)
        {
            /* Erase tool on a hit -> remove + push undo. */
            if (auto* gui = services_.find<GuiService>())
            {
                auto cmd = std::make_unique<MidiNoteDiffCommand> (parent_.getBoundRegionId(),
                                                                   parent_.getRegionResolver());
                cmd->recordRemove (*region, hitId);
                gui->getUndoManager().perform (cmd.release(), "Erase MIDI note");
            }
            else
            {
                region->removeNoteById (hitId);
            }
            selectedNoteIds_.erase (hitId);
            repaint();
            return;
        }

        /* Select / Pencil on a body hit -> move drag. */
        activeDrag_ = NoteDrag::makeMove (*this, *region, hitId, e);
        return;
    }

    /* Empty area click. */
    if (! e.mods.isCommandDown() && ! e.mods.isCtrlDown())
        selectClear();

    if (parent_.getActiveTool() == PianoRollView::Tool::Pencil)
    {
        activeDrag_ = NoteDrag::makeCreate (*this, *region, e);
        return;
    }

    /* Select + Erase on empty area -> marquee rubber-band. */
    activeDrag_ = NoteDrag::makeMarquee (*this, e,
                                           parent_.getActiveTool() == PianoRollView::Tool::Erase);
}

void PianoRollGrid::mouseDrag (const juce::MouseEvent& e)
{
    if (activeDrag_ != nullptr)
        activeDrag_->mouseDrag (e, *this);
}

void PianoRollGrid::mouseUp (const juce::MouseEvent& e)
{
    if (activeDrag_ == nullptr) return;
    auto drag = std::move (activeDrag_);
    drag->mouseUp (e, *this);
    repaint();
}

void PianoRollGrid::mouseMove (const juce::MouseEvent& e)
{
    /* Update cursor: resize handle = horizontal-resize cursor; over a
     * note body = grab cursor (Select/Pencil) or x cursor (Erase);
     * empty area = pencil cursor (Pencil) or normal (Select/Erase). */
    if (auto* region = resolveBoundRegion())
    {
        if (parent_.getActiveTool() == PianoRollView::Tool::Select
            && hitTestResizeHandle (e.x, e.y, *region) != 0)
        {
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            return;
        }
        if (hitTestNote (e.x, e.y, *region) != 0)
        {
            setMouseCursor (parent_.getActiveTool() == PianoRollView::Tool::Erase
                              ? juce::MouseCursor::PointingHandCursor
                              : juce::MouseCursor::DraggingHandCursor);
            return;
        }
    }
    setMouseCursor (parent_.getActiveTool() == PianoRollView::Tool::Pencil
                      ? juce::MouseCursor::CrosshairCursor
                      : juce::MouseCursor::NormalCursor);
}

void PianoRollGrid::mouseWheelMove (const juce::MouseEvent& e,
                                      const juce::MouseWheelDetails& wheel)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        /* Zoom: cmd / ctrl + scroll.  Anchor zoom around the mouse X
         * so the beat under the cursor stays put.  We resize in the
         * grid's own coords; the surrounding Viewport will adjust
         * its scrollPos to keep the visible area sensible. */
        const double mouseBeat = (pxPerBeat_ > 0)
            ? (double) e.x / (double) pxPerBeat_
            : 0.0;

        const int delta = (wheel.deltaY > 0.0f) ? 4 : -4;
        const int next  = juce::jlimit (kPxPerBeatMin,
                                          kPxPerBeatMax,
                                          pxPerBeat_ + delta);
        if (next == pxPerBeat_) return;
        pxPerBeat_ = next;

        /* Resize self so the viewport scrollbar tracks. */
        const int spanW = (int) std::round (regionLenBeats_ * pxPerBeat_);
        setSize (juce::jmax (spanW, 1), getHeight());

        if (auto* vp = findParentComponentOfClass<juce::Viewport>())
        {
            const int newMouseX = (int) std::round (mouseBeat * pxPerBeat_);
            const int dx        = newMouseX - e.x;
            const int viewX     = juce::jmax (0, vp->getViewPositionX() + dx);
            vp->setViewPosition (viewX, vp->getViewPositionY());
        }

        repaint();
        return;
    }

    /* No mod: let the viewport handle horizontal scroll naturally. */
    juce::Component::mouseWheelMove (e, wheel);
}

bool PianoRollGrid::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::deleteKey
        || key == juce::KeyPress::backspaceKey)
    {
        if (selectedNoteIds_.empty()) return false;
        auto* region = resolveBoundRegion();
        if (region == nullptr) return false;

        if (auto* gui = services_.find<GuiService>())
        {
            auto cmd = std::make_unique<MidiNoteDiffCommand> (parent_.getBoundRegionId(),
                                                                parent_.getRegionResolver());
            for (auto id : selectedNoteIds_)
                cmd->recordRemove (*region, id);
            gui->getUndoManager().perform (cmd.release(), "Delete MIDI notes");
        }
        else
        {
            for (auto id : selectedNoteIds_)
                region->removeNoteById (id);
        }
        selectedNoteIds_.clear();
        repaint();
        return true;
    }
    return false;
}

} // namespace element
