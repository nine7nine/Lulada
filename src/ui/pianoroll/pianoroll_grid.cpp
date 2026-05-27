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
    const auto body = bodyBounds();
    const float rowH = (float) body.getHeight() / (float) span;
    if (rowH <= 0.0f) return lo;
    const int yInBody = y - body.getY();
    const int row = (int) ((float) yInBody / rowH);
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
    const auto body = bodyBounds();
    const float rowH = (float) body.getHeight() / (float) span;
    return body.getY() + (int) ((float) (hi - pitch) * rowH);
}

int PianoRollGrid::rowHeight() const noexcept
{
    auto* kb = parent_.getKeyboard();
    if (kb == nullptr) return 1;
    const int hi = kb->getHighestVisibleNoteNumber();
    const int lo = kb->getLowestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    return juce::jmax (1, bodyBounds().getHeight() / span);
}

double PianoRollGrid::snapBeat (double localBeat) const noexcept
{
    if (! snapEnabled_ || snapDivision_ <= 0.0)
        return localBeat;
    return std::round (localBeat / snapDivision_) * snapDivision_;
}

void PianoRollGrid::setSnapDivision (double beats) noexcept
{
    if (beats < 0.0) beats = 0.0;
    if (juce::approximatelyEqual (beats, snapDivision_)) return;
    snapDivision_ = beats;
    repaint();   /* affects sub-grid line density */
}

void PianoRollGrid::setSnapEnabled (bool b) noexcept
{
    if (b == snapEnabled_) return;
    snapEnabled_ = b;
    repaint();
}

//==============================================================================
// Zoom helpers used by the toolbar +/-/Fit buttons.

void PianoRollGrid::zoomBy (double factor)
{
    if (factor <= 0.0) return;
    /* Anchor: viewport centre in beat coords.  Symmetric with the
     * cmd+wheel anchored zoom in mouseWheelMove. */
    int anchorBeatPx = getWidth() / 2;
    if (auto* vp = findParentComponentOfClass<juce::Viewport>())
        anchorBeatPx = vp->getViewPositionX() + vp->getMaximumVisibleWidth() / 2;

    const double anchorBeat = (pxPerBeat_ > 0)
        ? (double) anchorBeatPx / (double) pxPerBeat_
        : 0.0;

    const int next = juce::jlimit (kPxPerBeatMin, kPxPerBeatMax,
                                     (int) std::round ((double) pxPerBeat_ * factor));
    if (next == pxPerBeat_) return;
    pxPerBeat_ = next;

    const int spanW = (int) std::round (regionLenBeats_ * pxPerBeat_);
    setSize (juce::jmax (spanW, 1), getHeight());

    if (auto* vp = findParentComponentOfClass<juce::Viewport>())
    {
        const int newAnchorPx = (int) std::round (anchorBeat * pxPerBeat_);
        const int viewX = juce::jmax (0, newAnchorPx - vp->getMaximumVisibleWidth() / 2);
        vp->setViewPosition (viewX, vp->getViewPositionY());
    }
    repaint();
}

void PianoRollGrid::zoomToFit()
{
    if (auto* vp = findParentComponentOfClass<juce::Viewport>())
    {
        const int visibleW = vp->getMaximumVisibleWidth();
        if (visibleW > 0 && regionLenBeats_ > 0.0)
        {
            const int fit = (int) std::floor ((double) visibleW / regionLenBeats_);
            pxPerBeat_ = juce::jlimit (kPxPerBeatMin, kPxPerBeatMax, fit);
            const int spanW = (int) std::round (regionLenBeats_ * pxPerBeat_);
            setSize (juce::jmax (spanW, 1), getHeight());
            vp->setViewPosition (0, vp->getViewPositionY());
        }
    }
    repaint();
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
    /* Body background: mid-dark grey lifted from near-black so the
     * grid + notes have visual breathing room.  Matches Element's
     * contentBackgroundColor (0xff141414) shifted a hair to keep
     * the dock distinct from the arrangement view above. */
    g.setColour (juce::Colour (0xff'16'16'16));
    g.fillRect (bodyBounds());

    /* Paint the "beyond region" zone darker so the user can see the
     * span they're actually editing.  Grid component width may exceed
     * regionLen * pxPerBeat when the viewport is wider than the
     * region (auto-fit on bind), and we want the user to know clicks
     * past the right edge are no-ops. */
    const int regionEndX = (int) std::round (regionLenBeats_ * pxPerBeat_);
    if (getWidth() > regionEndX)
    {
        const auto body = bodyBounds();
        const juce::Rectangle<int> beyond (regionEndX,
                                            body.getY(),
                                            getWidth() - regionEndX,
                                            body.getHeight());
        g.setColour (juce::Colour (0xff'08'08'08));
        g.fillRect (beyond);
        /* 1 px brighter divider line at the region end. */
        g.setColour (juce::Colour (0xff'40'40'40));
        g.drawVerticalLine (regionEndX,
                              (float) body.getY(),
                              (float) body.getBottom());
    }

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
    paintRuler   (g, beatsPerBar);

    if (region == nullptr)
    {
        paintEmptyState (g);
        return;
    }

    paintNotes (g, *region);
    paintPlayhead (g);
    paintActiveDragOverlay (g);
}

void PianoRollGrid::paintEmptyState (juce::Graphics& g)
{
    g.setColour (juce::Colours::white.withAlpha (0.35f));
    g.setFont (monoFont (12.0f, juce::Font::plain));
    g.drawText ("Double-click a MIDI region to edit.",
                bodyBounds().getIntersection (visibleRect()),
                juce::Justification::centred,
                false);
}

void PianoRollGrid::paintBarGrid (juce::Graphics& g, int beatsPerBar)
{
    const auto vr   = visibleRect();
    const auto body = bodyBounds();
    if (vr.getWidth() <= 0 || body.getHeight() <= 0) return;
    if (pxPerBeat_ <= 0) return;

    auto* kb = parent_.getKeyboard();
    if (kb == nullptr) return;

    const int lo   = kb->getLowestVisibleNoteNumber();
    const int hi   = kb->getHighestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    const float rowH = (float) body.getHeight() / (float) span;

    /* Pitch row backdrop -- black-key rows tinted slightly darker so
     * the user reads octaves at a glance.  We paint each visible row
     * as a thin strip; one fillRect per pitch is cheap and avoids
     * blending artefacts at the row edges. */
    const float bodyL = (float) vr.getX();
    const float bodyR = (float) vr.getRight();
    const juce::Colour rowBlack { 0xff'11'12'14 };  /* faintly cooler than bg */
    const juce::Colour rowEdge  { 0xff'1d'1d'1d };  /* row separator */
    const juce::Colour rowOctaveEdge { 0xff'30'30'30 };

    for (int p = lo; p <= hi; ++p)
    {
        const float y = (float) body.getY() + (float) (hi - p) * rowH;
        /* Black-key pitches: C# D# F# G# A#  (mod 12 in {1,3,6,8,10}). */
        const int m = ((p % 12) + 12) % 12;
        const bool isBlack = (m == 1 || m == 3 || m == 6 || m == 8 || m == 10);
        if (isBlack)
        {
            g.setColour (rowBlack);
            g.fillRect (juce::Rectangle<float> (bodyL, y, bodyR - bodyL, rowH));
        }
        /* Row separator -- brighter on octave (every C). */
        g.setColour ((p % 12) == 0 ? rowOctaveEdge : rowEdge);
        g.drawHorizontalLine ((int) y, bodyL, bodyR);
    }

    /* Three-tier vertical grid: sub-snap (faintest) -> beat (medium)
     * -> bar (brightest).  Sub-snap lines drawn first so beat/bar
     * lines overpaint them at coincident X. */
    const int startBeat = juce::jmax (0, vr.getX() / pxPerBeat_);
    const int endBeat   = (vr.getRight() + pxPerBeat_ - 1) / pxPerBeat_;
    const float yT = (float) body.getY();
    const float yB = (float) body.getBottom();

    /* Sub-snap: only paint when snap is enabled, snapDivision_ subdivides
     * the beat (i.e. < 1.0), and at least 6 px between lines (avoids
     * visual noise when zoomed out). */
    if (snapEnabled_ && snapDivision_ > 0.0 && snapDivision_ < 1.0)
    {
        const double subPx = snapDivision_ * pxPerBeat_;
        if (subPx >= 6.0)
        {
            g.setColour (juce::Colour (0xff'1c'1c'1c));
            for (int beat = startBeat; beat <= endBeat; ++beat)
            {
                const double base = (double) beat;
                /* Step through subdivisions strictly between integer
                 * beats so the per-beat line doesn't get overdrawn by
                 * the sub-snap colour. */
                for (double sub = snapDivision_; sub < 1.0 - 1e-9; sub += snapDivision_)
                {
                    const int x = (int) std::round ((base + sub) * pxPerBeat_);
                    g.drawVerticalLine (x, yT, yB);
                }
            }
        }
    }

    /* Per-beat lines -- medium brightness. */
    g.setColour (juce::Colour (0xff'26'26'26));
    for (int beat = startBeat; beat <= endBeat; ++beat)
    {
        if (beat % beatsPerBar == 0) continue;
        const int x = beat * pxPerBeat_;
        g.drawVerticalLine (x, yT, yB);
    }

    /* Per-bar lines -- brightest. */
    g.setColour (juce::Colour (0xff'42'42'42));
    const int firstBar = (startBeat / beatsPerBar) * beatsPerBar;
    for (int beat = firstBar; beat <= endBeat; beat += beatsPerBar)
    {
        const int x = beat * pxPerBeat_;
        g.drawVerticalLine (x, yT, yB);
    }
}

void PianoRollGrid::paintRuler (juce::Graphics& g, int beatsPerBar)
{
    const auto vr = visibleRect();
    if (vr.getWidth() <= 0 || pxPerBeat_ <= 0) return;

    /* Palette mirrors ArrangementView::paintRuler -- bezel + cool-grey
     * vertical gradient inside + LCD-blue ticks at three brightness
     * tiers.  Single source of truth for the look so the piano-roll
     * + arrangement rulers read as siblings. */
    const juce::Colour kBezel       { 0xff'08'08'08 };
    const juce::Colour kBezelEdge   { 0xff'3a'3a'3a };
    const juce::Colour kLcdTop      { 0xff'14'19'1e };
    const juce::Colour kLcdBot      { 0xff'0c'0f'12 };
    const juce::Colour kLcdBlue     { 0xff'9e'dc'ff };
    const juce::Colour kLcdBlueMid  { 0xff'6f'b0'e0 };
    const juce::Colour kLcdBlueDim  { 0xff'4a'7c'a0 };

    const juce::Rectangle<int> ruler { 0, 0, getWidth(), kRulerH };

    /* Bezel + inset gradient + hairline divider against the body. */
    g.setColour (kBezel);
    g.fillRect (ruler);
    const auto inner = ruler.reduced (0, 2).toFloat();
    juce::ColourGradient lcdGrad (kLcdTop, inner.getX(), inner.getY(),
                                    kLcdBot, inner.getX(), inner.getBottom(),
                                    false);
    g.setGradientFill (lcdGrad);
    g.fillRect (inner);
    g.setColour (kBezelEdge);
    g.drawHorizontalLine (kRulerH - 1,
                            (float) vr.getX(),
                            (float) vr.getRight());

    /* Tick subdivision tracks the active snap setting -- the user
     * should always SEE the grid they're snapping to.  Snap off (or
     * snap >= 1 beat) falls back to a zoom-driven 4/2/1 ladder so the
     * ruler still has some sub-resolution at high zoom.  Triplet
     * snaps emit 3, 6 or 12 sub-ticks/beat (rounding to the nearest
     * supported subdivision count).  Final subdiv is capped so no
     * two adjacent sub-ticks land closer than ~5 px -- below that
     * they blur into a solid bar. */
    int subdiv = 1;
    if (snapEnabled_ && snapDivision_ > 0.0 && snapDivision_ <= 1.0)
    {
        const double subsPerBeat = 1.0 / snapDivision_;
        /* Round to nearest integer; covers duple (2/4/8/16/32) +
         * triplet (3/6/12) families since 1/0.125 = 8, 1/(1/6) = 6,
         * etc. */
        subdiv = juce::jlimit (1, 32, (int) std::round (subsPerBeat));
    }
    else
    {
        /* No snap -- use the prior zoom ladder so the ruler still
         * communicates beat resolution at high zoom. */
        subdiv = (pxPerBeat_ >= 32) ? 4
              : (pxPerBeat_ >= 16) ? 2 : 1;
    }
    while (subdiv > 1 && (double) pxPerBeat_ / (double) subdiv < 5.0)
        subdiv = juce::jmax (1, subdiv / 2);
    const int subStepPx = juce::jmax (1, pxPerBeat_ / subdiv);

    /* Loop bounds via clip rect intersected with visible -- caps the
     * work to what's actually drawn even on long regions. */
    const int x0 = juce::jmax (vr.getX(), g.getClipBounds().getX());
    const int x1 = juce::jmin (vr.getRight(), g.getClipBounds().getRight());
    const int subStart = juce::jmax (0, x0 / subStepPx - 1);
    const int subEnd   =                 x1 / subStepPx + 1;

    g.setFont (monoFont (10.0f, juce::Font::bold));

    for (int sub = subStart; sub <= subEnd; ++sub)
    {
        const int x = sub * subStepPx;
        if (x > vr.getRight()) break;
        const int beat   = sub / subdiv;
        const int phase  = sub % subdiv;
        const bool atBeat = (phase == 0);
        const bool atBar  = atBeat && (beat % beatsPerBar) == 0;

        int tickTop;
        juce::Colour tickCol;
        if      (atBar)  { tickTop = 3;            tickCol = kLcdBlue;    }
        else if (atBeat) { tickTop = kRulerH - 11; tickCol = kLcdBlueMid; }
        else             { tickTop = kRulerH - 6;  tickCol = kLcdBlueDim; }

        g.setColour (tickCol);
        g.drawVerticalLine (x, (float) tickTop, (float) (kRulerH - 2));

        if (atBar)
        {
            const int barNum = (beat / beatsPerBar) + 1;
            g.setColour (kLcdBlue);
            g.drawText (juce::String (barNum),
                        x + 3, 1, 32, kRulerH - 4,
                        juce::Justification::topLeft);
        }
    }
}

void PianoRollGrid::paintPlayhead (juce::Graphics& g)
{
    if (monitor_ == nullptr) return;
    /* Only paint when transport is actually rolling -- a parked
     * playhead inside the region's middle is visual noise. */
    if (! monitor_->playing.get()) return;

    /* Region-local beat = transport beat - region.positionBeats. */
    auto* region = resolveBoundRegion();
    if (region == nullptr) return;

    const double transportBeat = (double) monitor_->getPositionBeats();
    const double localBeat = transportBeat - region->positionBeats;
    if (localBeat < 0.0 || localBeat > regionLenBeats_) return;

    const int x = (int) std::round (localBeat * pxPerBeat_);
    g.setColour (juce::Colour (0xff'40'ff'80).withAlpha (0.85f));
    g.drawVerticalLine (x, (float) kRulerH, (float) getHeight());
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
    const auto body = bodyBounds();
    const float rowH = (float) body.getHeight() / (float) span;

    const auto vr = visibleRect();
    /* Cull rect: only paint notes whose pixel rect intersects what
     * the user can actually see, AND intersects the body (notes that
     * scrolled up under the ruler are clipped). */
    const auto cullRect = vr.getIntersection (body);

    /* Note style mirrors ArrangementView region paint: a darker
     * shaded interior tinted from the region's colour, with the full
     * saturated colour applied as the outer outline.  White note-name
     * label reads against the dim body across the velocity range
     * without the black-on-bright fallback the gradient style needed.
     * Velocity scales body brightness so harder-hit notes still stand
     * out -- but never bright enough to fight the label or the
     * saturated outline. */
    const juce::Colour baseBody = region.colour;
    const juce::Colour selWash  { 0xff'40'ff'80 };   /* kAccentGreen */
    const juce::Colour selEdge  { 0xff'cf'ff'd6 };

    for (const auto& n : *snap)
    {
        if (n.pitch < lo || n.pitch > hi) continue;

        const int xRaw = (int) std::round (n.onBeat * pxPerBeat_);
        const int wRaw = juce::jmax (2, (int) std::round (n.lengthBeats * pxPerBeat_));
        const int yRaw = body.getY() + (int) ((float) (hi - n.pitch) * rowH);
        const int hRaw = juce::jmax (2, (int) rowH - 1);

        const juce::Rectangle<int> rect (xRaw, yRaw, wRaw, hRaw);
        if (! cullRect.intersects (rect)) continue;

        const bool selected = isSelected (n.id);
        const float velNorm = juce::jlimit (0.0f, 1.0f, (float) n.velocity / 127.0f);

        /* Body fill (shaded/tinted) and outline (saturated tint) --
         * timeline-region pattern.  ArrangementView's MIDI region uses
         * sat x 0.55 / brightness x 0.45; piano-roll notes match that
         * cap exactly but scale brightness down with velocity so soft
         * notes still read as darker.  Selection uses the accent-green
         * wash + bright accent edge for unambiguous read. */
        const juce::Colour fill = selected
            ? selWash.withMultipliedSaturation (0.85f)
                     .withMultipliedBrightness (0.45f)
            : baseBody.withMultipliedSaturation (0.55f)
                       .withMultipliedBrightness (0.30f + 0.15f * velNorm);
        const juce::Colour edge = selected ? selEdge : baseBody;

        const auto rf = rect.toFloat();
        const float corner = juce::jmin (3.0f, rf.getHeight() * 0.35f);

        g.setColour (fill);
        g.fillRoundedRectangle (rf, corner);

        g.setColour (edge);
        g.drawRoundedRectangle (rf, corner, selected ? 1.6f : 1.0f);

        /* Pitch label inside wide-enough notes.  Skip for tiny rows
         * (would clip vertically) and short notes.  Always-white text
         * pairs cleanly with the dim shaded body across the velocity
         * range -- no per-note brightness branch required. */
        if (rf.getWidth() >= 40.0f && rf.getHeight() >= 12.0f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.90f));
            g.setFont (monoFont (juce::jmin (10.0f, rf.getHeight() * 0.55f),
                                  juce::Font::plain));
            static const char* const pcs[12] = {
                "C", "C#", "D", "D#", "E", "F",
                "F#", "G", "G#", "A", "A#", "B" };
            const int oct = (n.pitch / 12) - 1;
            juce::String label = juce::String (pcs[n.pitch % 12]) + juce::String (oct);
            g.drawText (label,
                        rect.reduced (4, 1),
                        juce::Justification::centredLeft, false);
        }
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
    if (y < kRulerH) return 0;   /* ruler clicks aren't note hits */
    const auto* snap = region.loadSnapshot();
    if (snap == nullptr || snap->empty()) return 0;

    auto* kb = parent_.getKeyboard();
    if (kb == nullptr) return 0;

    const int lo = kb->getLowestVisibleNoteNumber();
    const int hi = kb->getHighestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    const auto body = bodyBounds();
    const float rowH = (float) body.getHeight() / (float) span;

    /* Iterate in reverse so notes painted on top (later in the
     * sorted snapshot if any overlap) win the hit. */
    for (auto it = snap->rbegin(); it != snap->rend(); ++it)
    {
        const auto& n = *it;
        if (n.pitch < lo || n.pitch > hi) continue;
        const int nx = (int) std::round (n.onBeat * pxPerBeat_);
        const int nw = juce::jmax (2, (int) std::round (n.lengthBeats * pxPerBeat_));
        const int ny = body.getY() + (int) ((float) (hi - n.pitch) * rowH);
        const int nh = juce::jmax (2, (int) rowH - 1);
        if (x >= nx && x < nx + nw && y >= ny && y < ny + nh)
            return n.id;
    }
    return 0;
}

std::uint64_t PianoRollGrid::hitTestResizeHandle (int x, int y,
                                                    const MidiNoteRegion& region) const noexcept
{
    if (y < kRulerH) return 0;
    const auto* snap = region.loadSnapshot();
    if (snap == nullptr || snap->empty()) return 0;

    auto* kb = parent_.getKeyboard();
    if (kb == nullptr) return 0;

    const int lo = kb->getLowestVisibleNoteNumber();
    const int hi = kb->getHighestVisibleNoteNumber();
    const int span = juce::jmax (1, hi - lo + 1);
    const auto body = bodyBounds();
    const float rowH = (float) body.getHeight() / (float) span;

    for (auto it = snap->rbegin(); it != snap->rend(); ++it)
    {
        const auto& n = *it;
        if (n.pitch < lo || n.pitch > hi) continue;
        const int nx = (int) std::round (n.onBeat * pxPerBeat_);
        const int nw = juce::jmax (2, (int) std::round (n.lengthBeats * pxPerBeat_));
        const int ny = body.getY() + (int) ((float) (hi - n.pitch) * rowH);
        const int nh = juce::jmax (2, (int) rowH - 1);
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

    /* Ruler band absorbs clicks -- no marquee/pencil-create above the
     * note body.  Transport-seek via ruler click is a Session 4 item. */
    if (e.y < kRulerH) return;

    auto* region = resolveBoundRegion();
    if (region == nullptr) return;

    /* "Beyond region" zone -- if the grid component is wider than the
     * bound region's beat span (auto-fit on bind, or user shrank the
     * region after a zoom), clicks past the region end are no-ops.
     * Prevents pencil-create from placing notes outside the region. */
    const int regionEndX = (int) std::round (regionLenBeats_ * pxPerBeat_);
    if (e.x >= regionEndX)
    {
        /* Click in the void = clear selection, same as empty-body
         * click inside the region. */
        if (! e.mods.isCommandDown() && ! e.mods.isCtrlDown())
            selectClear();
        return;
    }

    /* Brush tool short-circuits hit-test + selection so it always
     * paints fresh notes regardless of what's underneath the cursor.
     * No selection update either -- the brush is purely additive. */
    if (parent_.getActiveTool() == PianoRollView::Tool::Brush)
    {
        activeDrag_ = NoteDrag::makeBrush (*this, *region, e);
        return;
    }

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
                juce::Component::SafePointer<PianoRollView> safeView (&parent_);
                cmd->onApplied = [safeView]() {
                    if (auto* v = safeView.getComponent())
                        v->notifyRegionEdited();
                };
                gui->getUndoManager().perform (cmd.release(), "Erase MIDI note");
            }
            else
            {
                region->removeNoteById (hitId);
                parent_.notifyRegionEdited();
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
    const auto tool = parent_.getActiveTool();
    setMouseCursor ((tool == PianoRollView::Tool::Pencil
                     || tool == PianoRollView::Tool::Brush)
                      ? juce::MouseCursor::CrosshairCursor
                      : juce::MouseCursor::NormalCursor);
}

void PianoRollGrid::mouseWheelMove (const juce::MouseEvent& e,
                                      const juce::MouseWheelDetails& wheel)
{
    /* Wheel conventions (piano-roll standard):
     *   no mod      -> vertical PAN through pitches
     *   shift+wheel -> horizontal scroll (defer to viewport)
     *   alt+wheel   -> vertical zoom around centre
     *   cmd/ctrl+wheel -> horizontal zoom anchored at cursor X */
    if (e.mods.isAltDown())
    {
        if (auto* kb = parent_.getKeyboard())
        {
            const double factor = (wheel.deltaY > 0.0f) ? (1.0 / 1.15) : 1.15;
            kb->zoomVertically (factor);
            /* Keyboard repaint already triggered by setVisibleNoteRange;
             * the grid's own paint reads kb->getLowestVisibleNoteNumber
             * + getHighestVisibleNoteNumber live, so a repaint here
             * picks up the new pitch span. */
            repaint();
        }
        return;
    }

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

    /* Plain wheel: vertical pan through pitches.  Scroll AMOUNT is
     * derived from wheel.deltaY so a high-resolution trackpad gets
     * sub-row precision while a notched mouse wheel still moves a
     * meaningful chunk per click.  Negative sign because wheel.deltaY
     * positive == finger moving up (= scroll upward in content =
     * raise visible-pitch lower-bound).
     *
     * Shift+wheel forwards to the parent component, letting the host
     * viewport handle horizontal scroll the same way a non-modified
     * wheel did before this change. */
    if (e.mods.isShiftDown())
    {
        juce::Component::mouseWheelMove (e, wheel);
        return;
    }

    if (auto* kb = parent_.getKeyboard())
    {
        /* Convert wheel.deltaY (typically -1.0..+1.0 per notch) into a
         * semitone delta.  ~3 semitones per full wheel tick feels
         * close to JUCE's default scroll speed in other vertical
         * surfaces without being so coarse that an octave click skips
         * the target row entirely. */
        const float k = wheel.isReversed ? -3.0f : 3.0f;
        int delta = (int) std::lround (-wheel.deltaY * k);
        if (delta == 0)
            delta = (wheel.deltaY > 0.0f) ? -1 : (wheel.deltaY < 0.0f ? 1 : 0);
        if (delta != 0)
        {
            kb->shiftVisibleRange (delta);
            repaint();
        }
        return;
    }

    juce::Component::mouseWheelMove (e, wheel);
}

bool PianoRollGrid::keyPressed (const juce::KeyPress& key)
{
    auto* region = resolveBoundRegion();

    /* Delete / Backspace -- remove selected notes via undoable diff. */
    if (key == juce::KeyPress::deleteKey
        || key == juce::KeyPress::backspaceKey)
    {
        if (selectedNoteIds_.empty()) return false;
        if (region == nullptr) return false;

        if (auto* gui = services_.find<GuiService>())
        {
            auto cmd = std::make_unique<MidiNoteDiffCommand> (parent_.getBoundRegionId(),
                                                                parent_.getRegionResolver());
            for (auto id : selectedNoteIds_)
                cmd->recordRemove (*region, id);
            juce::Component::SafePointer<PianoRollView> safeView (&parent_);
            cmd->onApplied = [safeView]() {
                if (auto* v = safeView.getComponent())
                    v->notifyRegionEdited();
            };
            gui->getUndoManager().perform (cmd.release(), "Delete MIDI notes");
        }
        else
        {
            for (auto id : selectedNoteIds_)
                region->removeNoteById (id);
            parent_.notifyRegionEdited();
        }
        selectedNoteIds_.clear();
        repaint();
        return true;
    }

    /* Ctrl+A -- select every note in the bound region. */
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'A')
    {
        if (region == nullptr) return false;
        if (const auto* snap = region->loadSnapshot())
        {
            selectedNoteIds_.clear();
            for (const auto& n : *snap)
                selectedNoteIds_.insert (n.id);
            repaint();
        }
        return true;
    }

    /* Ctrl+D -- duplicate selection one snap-division to the right.
     * Each duplicate gets a fresh id; final selection contains the
     * duplicates so subsequent edits operate on the new notes. */
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'D')
    {
        if (region == nullptr || selectedNoteIds_.empty()) return false;
        const auto* snap = region->loadSnapshot();
        if (snap == nullptr) return false;

        const double step = isSnapEnabled() && getSnapDivision() > 0.0
                             ? getSnapDivision()
                             : 1.0;
        std::vector<MidiNote> dupes;
        dupes.reserve (selectedNoteIds_.size());
        for (const auto& n : *snap)
            if (isSelected (n.id))
            {
                MidiNote copy = n;
                copy.id     = 0;   /* fresh id stamped on add */
                copy.onBeat = n.onBeat + step;
                if (copy.onBeat < region->lengthBeats)
                    dupes.push_back (copy);
            }
        if (dupes.empty()) return true;

        if (auto* gui = services_.find<GuiService>())
        {
            auto cmd = std::make_unique<MidiNoteDiffCommand> (parent_.getBoundRegionId(),
                                                                parent_.getRegionResolver());
            for (auto& d : dupes)
                cmd->recordAdd (*region, d);
            juce::Component::SafePointer<PianoRollView> safeView (&parent_);
            cmd->onApplied = [safeView]() {
                if (auto* v = safeView.getComponent())
                    v->notifyRegionEdited();
            };
            gui->getUndoManager().perform (cmd.release(), "Duplicate MIDI notes");
        }
        else
        {
            for (auto& d : dupes)
                region->addNote (d);
        }
        /* Re-select the duplicates -- ids assigned by recordAdd are
         * stable post-perform; the snapshot has new ids in the same
         * order as `dupes`, so iterating again finds them. */
        selectedNoteIds_.clear();
        if (const auto* snap2 = region->loadSnapshot())
        {
            for (const auto& n : *snap2)
                for (const auto& d : dupes)
                    if (n.pitch == d.pitch
                        && std::abs (n.onBeat - d.onBeat) < 1e-9
                        && n.channel == d.channel
                        && ! isSelected (n.id))
                    {
                        selectedNoteIds_.insert (n.id);
                        break;
                    }
        }
        parent_.notifyRegionEdited();
        repaint();
        return true;
    }

    /* Arrow keys -- nudge selection.  Left/Right by snap division (or
     * 1 beat if snap off); Up/Down by 1 semitone; Shift+Up/Down by 12
     * (octave).  All routed through the undo path so each press is a
     * step the user can rewind. */
    auto nudge = [&] (double beatDelta, int pitchDelta, const juce::String& label) -> bool
    {
        if (region == nullptr || selectedNoteIds_.empty()) return false;
        const auto* snap = region->loadSnapshot();
        if (snap == nullptr) return false;

        std::vector<std::pair<MidiNote, MidiNote>> moves;
        moves.reserve (selectedNoteIds_.size());
        for (const auto& n : *snap)
            if (isSelected (n.id))
            {
                MidiNote after = n;
                after.onBeat = juce::jmax (0.0, n.onBeat + beatDelta);
                if (after.onBeat + after.lengthBeats > region->lengthBeats)
                    after.onBeat = juce::jmax (0.0,
                        region->lengthBeats - after.lengthBeats);
                after.pitch  = juce::jlimit (0, 127, n.pitch + pitchDelta);
                if (after.pitch == n.pitch
                    && std::abs (after.onBeat - n.onBeat) < 1e-9) continue;
                moves.emplace_back (n, after);
            }
        if (moves.empty()) return true;

        if (auto* gui = services_.find<GuiService>())
        {
            auto cmd = std::make_unique<MidiNoteDiffCommand> (parent_.getBoundRegionId(),
                                                                parent_.getRegionResolver());
            for (auto& mv : moves)
                cmd->recordUpdate (mv.first.id, mv.first, mv.second);
            juce::Component::SafePointer<PianoRollView> safeView (&parent_);
            cmd->onApplied = [safeView]() {
                if (auto* v = safeView.getComponent())
                    v->notifyRegionEdited();
            };
            gui->getUndoManager().perform (cmd.release(), label);
        }
        else
        {
            for (auto& mv : moves)
                region->updateNoteById (mv.first.id, mv.second);
        }
        parent_.notifyRegionEdited();
        repaint();
        return true;
    };

    if (key == juce::KeyPress::leftKey
        || key == juce::KeyPress::rightKey
        || key == juce::KeyPress::upKey
        || key == juce::KeyPress::downKey)
    {
        const double step = isSnapEnabled() && getSnapDivision() > 0.0
                             ? getSnapDivision()
                             : 1.0;
        if (key == juce::KeyPress::leftKey)  return nudge (-step, 0,  "Nudge left");
        if (key == juce::KeyPress::rightKey) return nudge ( step, 0,  "Nudge right");
        const int p = key.getModifiers().isShiftDown() ? 12 : 1;
        if (key == juce::KeyPress::upKey)    return nudge (0,  p, "Transpose up");
        if (key == juce::KeyPress::downKey)  return nudge (0, -p, "Transpose down");
    }

    return false;
}

} // namespace element
