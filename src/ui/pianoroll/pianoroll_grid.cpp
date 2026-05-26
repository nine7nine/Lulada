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

    /* Tick subdivision scales with zoom -- ArrangementView mirrors
     * the same thresholds (>=32 px/beat -> 4 sub-ticks, >=16 -> 2,
     * else 1).  Sub-ticks render as the dim LCD tier so they don't
     * compete with the beat + bar ticks. */
    const int subdiv = (pxPerBeat_ >= 32) ? 4
                     : (pxPerBeat_ >= 16) ? 2 : 1;
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

    /* Base colours derived from the region's colour.  Brighter top
     * edge gives a subtle "lit from above" depth cue; selected notes
     * use a saturated wash + accent edge.  Velocity scales the body
     * brightness so harder-hit notes stand out (Ableton convention). */
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
        /* Body brightness: velocity 1 reads at ~70% saturation, velocity 127 at full.
         * Selection overrides the colour entirely for unambiguous read. */
        const juce::Colour bodyMid = selected
            ? selWash
            : baseBody.withMultipliedSaturation (0.70f + 0.30f * velNorm)
                       .withMultipliedBrightness (0.85f + 0.30f * velNorm);
        const juce::Colour bodyTop = bodyMid.brighter (0.30f);
        const juce::Colour bodyBot = bodyMid.darker   (0.20f);
        const juce::Colour edge    = selected
            ? selEdge
            : bodyMid.brighter (0.45f);

        const auto rf = rect.toFloat();
        const float corner = juce::jmin (3.0f, rf.getHeight() * 0.35f);

        /* Body: vertical gradient so the note reads as solid + lit
         * from above.  Single setGradientFill + fillRoundedRectangle
         * call -- no overdraw, no per-pixel work. */
        juce::ColourGradient grad (bodyTop, rf.getX(), rf.getY(),
                                     bodyBot, rf.getX(), rf.getBottom(),
                                     false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (rf, corner);

        /* Outer outline + a brighter top hairline for the depth cue
         * (avoids relying on a perceptible gradient at small row
         * heights where the gradient flattens visually). */
        g.setColour (edge);
        g.drawRoundedRectangle (rf, corner, selected ? 1.6f : 1.0f);
        if (rf.getHeight() >= 6.0f)
        {
            g.setColour (juce::Colours::white.withAlpha (selected ? 0.30f : 0.18f));
            g.drawLine (rf.getX() + 1.5f, rf.getY() + 1.0f,
                        rf.getRight() - 1.5f, rf.getY() + 1.0f, 1.0f);
        }

        /* Pitch label inside wide-enough notes.  Skip for tiny rows
         * (would clip vertically) and short notes.  The text colour
         * picks black or white depending on body brightness so it
         * stays readable across the velocity range. */
        if (rf.getWidth() >= 40.0f && rf.getHeight() >= 12.0f)
        {
            const bool useBlack = bodyMid.getPerceivedBrightness() > 0.62f;
            g.setColour (useBlack ? juce::Colours::black.withAlpha (0.78f)
                                  : juce::Colours::white.withAlpha (0.80f));
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
            parent_.notifyRegionEdited();
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
    /* Alt + wheel = VERTICAL zoom (visible pitch span shrinks/grows
     * around centre).  Shift + wheel = vertical PAN (shift the
     * visible band up/down without changing the span).  Cmd/Ctrl +
     * wheel = horizontal zoom (handled below). */
    if (e.mods.isAltDown())
    {
        if (auto* kb = parent_.getKeyboard())
        {
            const double factor = (wheel.deltaY > 0.0f) ? (1.0 / 1.15) : 1.15;
            kb->zoomVertically (factor);
            /* Keyboard repaint already triggered by setVisibleNoteRange;
             * the grid's own paint reads kb->getLowestVisibleNoteNumber
             * + cachedHighest_ live, so a repaint here picks up the
             * new pitch span. */
            repaint();
        }
        return;
    }
    if (e.mods.isShiftDown())
    {
        if (auto* kb = parent_.getKeyboard())
        {
            const int delta = (wheel.deltaY > 0.0f) ? 2 : -2;
            kb->shiftVisibleRange (delta);
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
        parent_.notifyRegionEdited();
        repaint();
        return true;
    }
    return false;
}

} // namespace element
