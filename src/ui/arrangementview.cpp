// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/arrangementview.hpp"

#include <element/audioengine.hpp>
#include <element/context.hpp>
#include <element/engine.hpp>
#include <element/node.hpp>
#include <element/session.hpp>
#include <element/services.hpp>
#include <element/tags.hpp>
#include <element/ui.hpp>
#include <element/ui/style.hpp>

#include "nodes/audioclip.hpp"
#include "nodes/tracker.hpp"
#include "services/arrangementtracksservice.hpp"
#include "services/sources/sourceregistry.hpp"
#include "services/timeline/audiolaneadapter.hpp"
#include "ui/fontcache.hpp"
#include "ui/lanepalette.hpp"

#include <unordered_map>

namespace element {

using juce::Colour;
using juce::Graphics;
using juce::MouseEvent;
using juce::Rectangle;
using juce::String;
namespace Colours = juce::Colours;

namespace {

/** Acceptable audio extensions for external file drop.  Lowercase
 *  comparison via String::endsWithIgnoreCase. */
const char* const kAudioDropExtensions[] = {
    ".wav", ".aiff", ".aif", ".flac", ".ogg", ".mp3", ".w64", ".au"
};

/* Lane tint palette lives in ui/lanepalette.hpp (shared with the
 * tracker editor + future session-view clip cells).  Local helper
 * kept so call sites read naturally. */
inline juce::Colour laneTintForIndex (int idx) noexcept
{
    return element::ui::laneTint (idx);
}

bool isAcceptableAudioFile (const juce::String& path) noexcept
{
    for (const char* ext : kAudioDropExtensions)
        if (path.endsWithIgnoreCase (ext))
            return true;
    return false;
}

bool anyAudioFileIn (const juce::StringArray& files) noexcept
{
    for (const auto& f : files)
        if (isAcceptableAudioFile (f))
            return true;
    return false;
}

} // namespace

/* ===================================================================== */
/* Body: inline-paint component drawing every lane in one paint() pass.  */
/* Also the FileDragAndDropTarget so external file drops hit the lanes.  */
/* ===================================================================== */

class ArrangementView::Body : public juce::Component,
                              public juce::FileDragAndDropTarget,
                              private juce::ChangeListener
{
public:
    explicit Body (ArrangementView& o)
        : owner (o),
          thumbnailCache_ (kThumbnailCacheEntries)
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);   // Delete key handling

        /* Register stock audio formats once.  Same shape Sampler /
         * AudioFilePlayerNode use; libsndfile is the underlying
         * reader for WAV/AIF/FLAC under JUCE_LINUX (no wineserver). */
        formatManager_.registerBasicFormats();
    }

    ~Body() override
    {
        /* Detach as a listener from any thumbnails we built so their
         * background-thread completion doesn't fire callbacks into a
         * dying Body. */
        for (auto& kv : thumbnails_)
            if (kv.second)
                kv.second->removeChangeListener (this);
    }

    /** Active editing tool.  Toolbar above the ruler swaps between
     *  these; mouseDown / mouseDrag branch on the value.
     *   - Select   : drag = move, edge-drag = resize, click = launch,
     *                right-click = context menu (existing behaviour)
     *   - Range    : drag in strip = set time-range overlay
     *                (loops between rangeStart/rangeEnd when loop
     *                button is on; transport wraps in timerCallback)
     *   - Split    : click on region = split at click beat
     *   - Trim     : drag on region = resize only (no move)
     *   - Audition : click on region = launch immediately, no drag */
    enum class Tool { Select, Range, Split, Trim, Audition };

    void setActiveTool (Tool t)
    {
        if (activeTool_ == t) return;
        activeTool_ = t;
        /* Cancel any in-flight gesture if it doesn't suit the new
         * tool; harmless to wipe even when it does. */
        gesture_ = Gesture {};
        switch (t)
        {
            case Tool::Select:   setMouseCursor (juce::MouseCursor::NormalCursor);          break;
            case Tool::Range:    setMouseCursor (juce::MouseCursor::CrosshairCursor);        break;
            case Tool::Split:    setMouseCursor (juce::MouseCursor::IBeamCursor);             break;
            case Tool::Trim:     setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);   break;
            case Tool::Audition: setMouseCursor (juce::MouseCursor::PointingHandCursor);      break;
        }
        repaint();
    }
    Tool getActiveTool() const noexcept { return activeTool_; }

    /** Loop-range state.  rangeActive_ is "user has drawn a range";
     *  loopActive_ is "and the loop button is on".  Loop only takes
     *  effect when both are true.  Range bounds are in beats and
     *  always normalised so rangeStart_ <= rangeEnd_. */
    bool   hasRange()       const noexcept { return rangeActive_; }
    double rangeStart()     const noexcept { return rangeStart_; }
    double rangeEnd()       const noexcept { return rangeEnd_;   }
    bool   isLooping()      const noexcept { return loopActive_ && rangeActive_
                                                     && rangeEnd_ > rangeStart_; }
    void   setLoopActive (bool on) { loopActive_ = on; repaint(); }
    void   clearRange()
    {
        rangeActive_ = false;
        rangeDragging_ = false;
        repaint();
    }

private:
    /** juce::AudioThumbnail emits a change notification each time the
     *  background reader updates its peak-data window.  Repaint the
     *  whole body when any thumbnail progresses -- cheap, JUCE
     *  invalidates only what changed at the windowing layer. */
    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        repaint();
    }

public:

    void paint (Graphics& g) override
    {
        g.fillAll (Colors::contentBackgroundColor);

        /* Ruler at the top, ahead of any lanes; matches tracker
         * gutter colour for visual continuity with the rest of
         * Element's timeline-style views. */
        paintRuler (g);

        const int laneCount = owner.lanes_.size();
        /* Viewport-clip skip: at high lane counts + zoomed-in vertical
         * extent, the dirty rect from a scroll / playhead / zoom only
         * intersects a handful of lanes.  Walking paintLane for the
         * rest is wasted work (each lane builds Paths + reads thumbs +
         * paints region fills).  JUCE clips the actual draw calls, but
         * the per-region preamble runs anyway -- skipping at the lane
         * level kills the whole branch. */
        const auto laneClip = g.getClipBounds();
        for (int i = 0; i < laneCount; ++i)
        {
            const int laneY = kRulerH + i * kLaneH;
            if (laneClip.getBottom() <= laneY || laneClip.getY() >= laneY + kLaneH)
                continue;
            paintLane (g, i);
        }

        if (laneCount == 0)
        {
            g.setColour (Colors::textColor.withAlpha (0.5f));
            g.setFont (juce::FontOptions (14.0f));
            g.drawText ("Drop an audio file here, or click + Audio to add a track.",
                        getLocalBounds().withTrimmedTop (kRulerH),
                        juce::Justification::centred);
        }

        /* Range overlay -- drawn over all lanes so it reads as a
         * timeline-wide selection.  Loop-armed range gets a brighter
         * outline + tinted fill; plain range gets a softer wash. */
        if (rangeActive_ && rangeEnd_ > rangeStart_)
        {
            const int xs = kLabelW + (int) (rangeStart_ * kPxPerBeat);
            const int xe = kLabelW + (int) (rangeEnd_   * kPxPerBeat);
            const int w  = juce::jmax (1, xe - xs);
            const int yTop  = 0;
            const int yLanes = kRulerH + juce::jmax (1, laneCount) * kLaneH;

            const juce::Colour fillCol = loopActive_
                ? juce::Colour::fromRGBA (90, 180, 110, 50)
                : juce::Colour::fromRGBA (140, 170, 210, 38);
            const juce::Colour edgeCol = loopActive_
                ? juce::Colour::fromRGB (110, 220, 130)
                : juce::Colour::fromRGB (170, 200, 240);

            /* Ruler-row band (brighter, marks the range as a loop
             * marker when looping is on). */
            g.setColour (loopActive_
                            ? juce::Colour::fromRGBA (90, 200, 110, 130)
                            : juce::Colour::fromRGBA (140, 170, 210, 100));
            g.fillRect (xs, yTop, w, kRulerH);

            /* Lane-area wash. */
            g.setColour (fillCol);
            g.fillRect (xs, kRulerH, w, juce::jmax (0, yLanes - kRulerH));

            /* Edges -- 1 px vertical lines at start + end. */
            g.setColour (edgeCol);
            g.drawVerticalLine (xs,        (float) yTop, (float) yLanes);
            g.drawVerticalLine (xe - 1,    (float) yTop, (float) yLanes);

            /* Loop badge in the ruler row when loop is on. */
            if (loopActive_)
            {
                g.setColour (juce::Colours::black);
                g.setFont (monoFont (
                                              9.0f, juce::Font::bold));
                g.drawText ("LOOP", xs + 4, 2, juce::jmin (40, w - 8), kRulerH - 4,
                            juce::Justification::centredLeft, false);
            }
        }

        if (dropHover_)
        {
            const Rectangle<int> hover = dropHoverLaneIdx_ >= 0
                ? Rectangle<int> (kLabelW, dropHoverLaneIdx_ * kLaneH,
                                   getWidth() - kLabelW, kLaneH)
                : Rectangle<int> (kLabelW, laneCount * kLaneH,
                                   getWidth() - kLabelW, kLaneH);
            g.setColour (Colours::limegreen.withAlpha (0.18f));
            g.fillRect (hover);
            g.setColour (Colours::limegreen);
            g.drawRect (hover, 2);
        }
    }

    //==========================================================================
    // FileDragAndDropTarget

    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        const bool interested = anyAudioFileIn (files);
        juce::Logger::writeToLog (
            juce::String ("[ArrangementView::Body] isInterestedInFileDrag count=")
            + juce::String (files.size())
            + (interested ? " ACCEPT" : " REJECT"));
        return interested;
    }

    void fileDragEnter (const juce::StringArray& files, int x, int y) override
    {
        juce::Logger::writeToLog (
            juce::String ("[ArrangementView::Body] fileDragEnter x=")
            + juce::String (x) + " y=" + juce::String (y)
            + " count=" + juce::String (files.size()));
        if (! anyAudioFileIn (files)) return;
        dropHover_         = true;
        dropHoverLaneIdx_  = owner.laneIdxFromY (y);
        juce::ignoreUnused (x);
        repaint();
    }

    void fileDragMove (const juce::StringArray&, int /*x*/, int y) override
    {
        const int li = owner.laneIdxFromY (y);
        if (li == dropHoverLaneIdx_) return;
        dropHoverLaneIdx_ = li;
        repaint();
    }

    void fileDragExit (const juce::StringArray&) override
    {
        dropHover_        = false;
        dropHoverLaneIdx_ = -1;
        repaint();
    }

    void filesDropped (const juce::StringArray& files, int x, int y) override
    {
        juce::Logger::writeToLog (
            juce::String ("[ArrangementView::Body] filesDropped x=")
            + juce::String (x) + " y=" + juce::String (y)
            + " count=" + juce::String (files.size()));

        dropHover_        = false;
        dropHoverLaneIdx_ = -1;
        repaint();

        const int targetLane = owner.laneIdxFromY (y);
        const double dropBeats =
            juce::jmax (0.0, (double) (x - kLabelW) / (double) kPxPerBeat);

        /* If dropping on a tracker lane, force-create a fresh audio
         * lane below it instead of trying to mix kinds.  -1 = create
         * new. */
        int laneIdx = targetLane;
        if (laneIdx >= 0 && laneIdx < owner.lanes_.size())
        {
            const auto& runtime = owner.laneRuntime_.getReference (laneIdx);
            if (! runtime.isAudioLane())
                laneIdx = -1;
        }
        else
        {
            laneIdx = -1;
        }

        double cursor = dropBeats;
        for (const auto& path : files)
        {
            if (! isAcceptableAudioFile (path)) continue;
            const juce::File f (path);
            if (! f.existsAsFile()) continue;

            const bool ok = owner.importAudioFileToLane (f, laneIdx, cursor);
            if (! ok) continue;

            /* First file may have created the lane; subsequent files
             * append into the same lane.  Lookup by file path is
             * fragile; the simplest re-target is "the last lane in
             * the array" since createEmptyAudioLane appends. */
            if (laneIdx < 0)
                laneIdx = owner.lanes_.size() - 1;

            /* Walk past the just-appended region for the next file.
             * Each Region's lengthBeats was computed from file
             * duration + session bpm. */
            if (laneIdx >= 0 && laneIdx < owner.lanes_.size())
            {
                const auto& regs = owner.lanes_.getReference (laneIdx).playlist.regions();
                if (! regs.empty())
                    cursor = regs.back().endBeats();
            }
        }
    }

    //==========================================================================
    // Mouse interaction
    //
    // Gesture model:
    //   - mouseDown on label arm-toggle  -> flip arm (audio lanes)
    //   - mouseDown on region right-edge -> begin resize
    //   - mouseDown on region body       -> select + arm potential drag
    //   - mouseDrag (after threshold)    -> commit drag/resize and live-update
    //   - mouseUp without drag           -> click-launch the region
    //   - mouseUp after drag/resize      -> writeLanesToSession + repaint
    //
    // Keyboard:
    //   - Delete / Backspace             -> remove selected region
    //
    // Hit-test sub-region for the resize handle is the last
    // kEdgeHandlePx pixels of the region's strip rectangle.  Inside
    // that band, mouseDown enters Resize mode; outside it's Move.

    void mouseDown (const MouseEvent& e) override
    {
        /* Ruler clicks (the bars:beats strip at the top): seek
         * transport to the clicked beat; if a drag follows, redefine
         * the range from the click point.  Mirrors Ardour/Bitwig
         * convention: ruler = locator + range surface. */
        if (e.y < kRulerH && e.x >= kLabelW)
        {
            const double beat = juce::jmax (0.0,
                (double) (e.x - kLabelW) / (double) kPxPerBeat);
            owner.seekToBeat (beat);

            rangeAnchor_   = beat;
            rangeStart_    = beat;
            rangeEnd_      = beat;
            rangeActive_   = true;
            rangeDragging_ = true;
            grabKeyboardFocus();
            repaint();
            return;
        }
        if (e.y < kRulerH) return;   /* ruler click in label gutter */

        const int laneIdx = (e.y - kRulerH) / kLaneH;
        if (laneIdx < 0 || laneIdx >= owner.lanes_.size()) return;
        auto& lane    = owner.lanes_.getReference (laneIdx);
        auto& runtime = owner.laneRuntime_.getReference (laneIdx);

        /* Label area button hits (M/S/R) are tool-independent. */
        if (e.x < kLabelW)
        {
            if (muteToggleRect (laneIdx).contains (e.x, e.y))
            {
                lane.muted = ! lane.muted;
                owner.writeLanesToSession();
                owner.propagateMuteSolo();
                repaintLane (laneIdx);
                return;
            }
            if (soloToggleRect (laneIdx).contains (e.x, e.y))
            {
                lane.soloed = ! lane.soloed;
                owner.writeLanesToSession();
                owner.propagateMuteSolo();
                repaint();
                return;
            }
            if (runtime.isAudioLane() && armToggleRect (laneIdx).contains (e.x, e.y))
            {
                lane.armed = ! lane.armed;
                runtime.audioClipCache->setArmed (lane.armed);
                owner.writeLanesToSession();
                repaintLane (laneIdx);
            }
            return;
        }

        if (runtime.isOrphan()) return;

        const double beat = (e.x - kLabelW) / (double) kPxPerBeat;

        /* Range tool: any strip-area click begins a range drag.
         * Doesn't matter whether a region was hit -- range overlays
         * sit above all lanes. */
        if (activeTool_ == Tool::Range)
        {
            rangeAnchor_   = beat;
            rangeStart_    = beat;
            rangeEnd_      = beat;
            rangeActive_   = true;
            rangeDragging_ = true;
            selectedLane_   = -1;
            selectedRegion_ = juce::Uuid::null();
            grabKeyboardFocus();
            repaint();
            return;
        }

        const auto& regions = lane.playlist.regions();
        for (const auto& r : regions)
        {
            if (! r.containsBeat (beat)) continue;

            /* Envelope-point hit test (Select tool, audio regions
             * only).  Takes priority over region-body click so
             * clicking on a breakpoint dot starts a point drag
             * instead of moving the region.  Right-click on a point
             * opens the curve-type / delete menu. */
            if (activeTool_ == Tool::Select
                && runtime.isAudioLane()
                && r.sequenceIdx < 0
                && ! r.volumeEnvelope.empty())
            {
                const int ptIdx = envPointHitAt (laneIdx, r, e.x, e.y);
                if (ptIdx >= 0)
                {
                    const auto pointId = r.volumeEnvelope[(size_t) ptIdx].id;
                    if (e.mods.isPopupMenu())
                    {
                        showEnvPointContextMenu (laneIdx, r.id, pointId);
                        return;
                    }
                    envGesture_.laneIdx    = laneIdx;
                    envGesture_.regionId   = r.id;
                    envGesture_.pointId    = pointId;
                    envGesture_.dragActive = false;
                    selectedLane_   = laneIdx;
                    selectedRegion_ = r.id;
                    grabKeyboardFocus();
                    repaintLane (laneIdx);
                    return;
                }
            }

            /* Right-click context menu works in every tool. */
            if (e.mods.isPopupMenu())
            {
                selectedLane_   = laneIdx;
                selectedRegion_ = r.id;
                repaintLane (laneIdx);
                showRegionContextMenu (laneIdx, r.id, beat);
                return;
            }

            /* Split tool: cleave the region in two at the click beat. */
            if (activeTool_ == Tool::Split)
            {
                const auto newId = lane.playlist.splitRegion (r.id, beat);
                if (! newId.isNull())
                {
                    owner.writeLanesToSession();
                    repaintLane (laneIdx);
                }
                return;
            }

            /* Audition tool: launch the region immediately (preview),
             * skipping the move/resize gesture path entirely. */
            if (activeTool_ == Tool::Audition)
            {
                launchRegionAudition (laneIdx, r);
                selectedLane_   = laneIdx;
                selectedRegion_ = r.id;
                repaintLane (laneIdx);
                return;
            }

            /* Select + Trim tools start a gesture; the mode differs:
             * - Select: edge hit = Resize, body = Move
             * - Trim:   anywhere on the region = Resize */
            gesture_.laneIdx         = laneIdx;
            gesture_.regionId        = r.id;
            gesture_.originalPos     = r.positionBeats;
            gesture_.originalLen     = r.lengthBeats;
            gesture_.mouseDownXBeats = beat;
            gesture_.dragActive      = false;

            const int regionEndX = kLabelW + (int) (r.endBeats() * kPxPerBeat);
            const bool overRightEdge =
                (e.x >= regionEndX - kEdgeHandlePx && e.x <= regionEndX);

            gesture_.mode = (activeTool_ == Tool::Trim || overRightEdge)
                              ? Gesture::Resize
                              : Gesture::Move;

            selectedLane_    = laneIdx;
            selectedRegion_  = r.id;

            grabKeyboardFocus();
            repaintLane (laneIdx);
            return;
        }

        /* Empty strip area -- clear selection. */
        selectedLane_   = -1;
        selectedRegion_ = juce::Uuid::null();
        gesture_        = Gesture {};
        repaint();
    }

    /** Compute the body rect of a region (the area BELOW its title
     *  strip).  Mirrors the paintLane layout maths so hit-tests +
     *  envelope edits sit precisely on the painted curve. */
    Rectangle<int> regionBodyRect (int laneIdx, const Region& r) const noexcept
    {
        constexpr int kTitleH = 13;
        const int laneTopY = kRulerH + laneIdx * kLaneH;
        const int xs = kLabelW + (int) (r.positionBeats * kPxPerBeat);
        const int ws = juce::jmax (4, (int) (r.lengthBeats * kPxPerBeat));
        const int innerTopY = laneTopY + 4 + kTitleH;
        const int innerH    = juce::jmax (1, kLaneH - 8 - kTitleH);
        return Rectangle<int> (xs, innerTopY, ws, innerH).reduced (3, 2);
    }

    /** Inverse of paintVolumeEnvelope's gainToY -- map a Y inside
     *  a body rect back to a gain in dB over the [+6, -24] window. */
    float envYToGainDb (int y, const Rectangle<int>& body) const noexcept
    {
        constexpr float kTopDb = 6.0f;
        constexpr float kBotDb = -24.0f;
        const int yPad = 3;
        const int H = juce::jmax (1, body.getHeight() - yPad * 2);
        const float t = juce::jlimit (0.0f, 1.0f,
            (float) (y - body.getY() - yPad) / (float) H);
        return kTopDb - t * (kTopDb - kBotDb);
    }

    /** Inverse of beatToX -- map an X to a beat offset within the
     *  region's local timeline.  Clamped to [0, lengthBeats]. */
    double envXToBeatOffset (int x, const Rectangle<int>& body,
                              const Region& r) const noexcept
    {
        if (r.lengthBeats <= 1e-6) return 0.0;
        const double t = juce::jlimit (0.0, 1.0,
            (double) (x - body.getX()) / juce::jmax (1.0, (double) body.getWidth()));
        return t * r.lengthBeats;
    }

    /** Hit-test for envelope breakpoint dots.  5 px radius -- matches
     *  the visual dot size when selected, with a small slop for
     *  finger / trackpad use.  Returns the matching point's index in
     *  the region's envelope, or -1 if none. */
    int envPointHitAt (int laneIdx, const Region& r, int x, int y) const noexcept
    {
        if (r.volumeEnvelope.empty()) return -1;
        const auto body = regionBodyRect (laneIdx, r);
        constexpr float kTopDb = 6.0f, kBotDb = -24.0f;
        const int yPad = 3;
        const int H = juce::jmax (1, body.getHeight() - yPad * 2);
        for (size_t i = 0; i < r.volumeEnvelope.size(); ++i)
        {
            const auto& pt = r.volumeEnvelope[i];
            const float px = body.getX()
                + (float) (pt.beatOffset / juce::jmax (1e-9, r.lengthBeats))
                  * (float) body.getWidth();
            const float t  = juce::jlimit (0.0f, 1.0f,
                (kTopDb - pt.gainDb) / (kTopDb - kBotDb));
            const float py = body.getY() + yPad + t * (float) H;
            const float dx = (float) x - px;
            const float dy = (float) y - py;
            if (dx * dx + dy * dy <= 25.0f) return (int) i;
        }
        return -1;
    }

    /** Insert a breakpoint at the clicked beat + gain.  When the
     *  envelope was empty, ALSO seed start + end anchors at the
     *  region's static gainDb so the user gets a visible curve out
     *  of one double-click instead of an invisible single point. */
    void insertEnvelopePoint (int laneIdx, juce::Uuid regionId,
                                double beatOffset, float gainDb)
    {
        if (laneIdx < 0 || laneIdx >= owner.lanes_.size()) return;
        auto& lane = owner.lanes_.getReference (laneIdx);
        auto* r = lane.playlist.findRegion (regionId);
        if (r == nullptr) return;

        if (r->volumeEnvelope.empty())
        {
            /* Seed at 0 dB (unity gain) so the envelope defaults to
             * "no attenuation", not whatever the static gainDb was.
             * The user drags individual points down from there. */
            EnvelopePoint anchor0; anchor0.id = juce::Uuid();
            anchor0.beatOffset = 0.0; anchor0.gainDb = 0.0f;
            EnvelopePoint anchorN; anchorN.id = juce::Uuid();
            anchorN.beatOffset = r->lengthBeats; anchorN.gainDb = 0.0f;
            r->volumeEnvelope.push_back (anchor0);
            r->volumeEnvelope.push_back (anchorN);
        }
        EnvelopePoint pt;
        pt.id         = juce::Uuid();
        pt.beatOffset = juce::jlimit (0.0, r->lengthBeats, beatOffset);
        pt.gainDb     = juce::jlimit (-60.0f, 12.0f, gainDb);
        pt.curve      = EnvelopeCurve::Linear;
        r->volumeEnvelope.push_back (pt);
        r->sortEnvelope();
        owner.writeLanesToSession();
        repaintLane (laneIdx);
    }

    /** Right-click context menu on an envelope point.  Curve type
     *  + delete.  Async so the menu can outlive its trigger event
     *  without keeping mouse state alive. */
    void showEnvPointContextMenu (int laneIdx, juce::Uuid regionId,
                                    juce::Uuid pointId)
    {
        if (laneIdx < 0 || laneIdx >= owner.lanes_.size()) return;
        auto& lane = owner.lanes_.getReference (laneIdx);
        auto* r = lane.playlist.findRegion (regionId);
        if (r == nullptr) return;

        EnvelopePoint* match = nullptr;
        for (auto& pt : r->volumeEnvelope)
            if (pt.id == pointId) { match = &pt; break; }
        if (match == nullptr) return;

        enum { ItemLinear = 1, ItemExp, ItemSmooth, ItemHold, ItemDelete = 10 };

        juce::PopupMenu menu;
        menu.addSectionHeader ("Curve to next point");
        menu.addItem (ItemLinear, "Linear", true, match->curve == EnvelopeCurve::Linear);
        menu.addItem (ItemExp,    "Exponential", true, match->curve == EnvelopeCurve::Exponential);
        menu.addItem (ItemSmooth, "Smooth",       true, match->curve == EnvelopeCurve::Smooth);
        menu.addItem (ItemHold,   "Hold (step)",  true, match->curve == EnvelopeCurve::Hold);
        menu.addSeparator();
        menu.addItem (ItemDelete, "Delete point");

        juce::Component::SafePointer<Body> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options(),
            [safe, laneIdx, regionId, pointId] (int result)
            {
                auto* self = safe.getComponent();
                if (self == nullptr || result == 0) return;
                if (laneIdx < 0 || laneIdx >= self->owner.lanes_.size()) return;
                auto& l = self->owner.lanes_.getReference (laneIdx);
                auto* rr = l.playlist.findRegion (regionId);
                if (rr == nullptr) return;

                if (result == ItemDelete)
                {
                    rr->volumeEnvelope.erase (
                        std::remove_if (rr->volumeEnvelope.begin(),
                                         rr->volumeEnvelope.end(),
                                         [pointId] (const EnvelopePoint& p)
                                         { return p.id == pointId; }),
                        rr->volumeEnvelope.end());
                }
                else
                {
                    EnvelopeCurve nc = EnvelopeCurve::Linear;
                    if (result == ItemExp)    nc = EnvelopeCurve::Exponential;
                    if (result == ItemSmooth) nc = EnvelopeCurve::Smooth;
                    if (result == ItemHold)   nc = EnvelopeCurve::Hold;
                    for (auto& pt : rr->volumeEnvelope)
                        if (pt.id == pointId) { pt.curve = nc; break; }
                }
                self->owner.writeLanesToSession();
                self->repaintLane (laneIdx);
            });
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        if (e.y < kRulerH) return;
        const int laneIdx = (e.y - kRulerH) / kLaneH;
        if (laneIdx < 0 || laneIdx >= owner.lanes_.size()) return;
        const auto& runtime = owner.laneRuntime_.getReference (laneIdx);
        if (! runtime.isAudioLane()) return;

        const auto& lane = owner.lanes_.getReference (laneIdx);
        const double beat = (e.x - kLabelW) / (double) kPxPerBeat;
        for (const auto& r : lane.playlist.regions())
        {
            if (! r.containsBeat (beat) || r.sequenceIdx >= 0) continue;
            const auto body = regionBodyRect (laneIdx, r);
            if (! body.contains (e.x, e.y)) continue;
            const double off = envXToBeatOffset (e.x, body, r);
            const float  db  = envYToGainDb     (e.y, body);
            insertEnvelopePoint (laneIdx, r.id, off, db);
            return;
        }
    }

    /** Audition helper -- routes a region launch through TrackerNode /
     *  AudioClipNode the same way the mouseUp click-launch does, but
     *  factored so the Audition tool can call it from mouseDown. */
    void launchRegionAudition (int laneIdx, const Region& r)
    {
        if (laneIdx < 0 || laneIdx >= owner.lanes_.size()) return;
        auto& runtime = owner.laneRuntime_.getReference (laneIdx);
        if (runtime.isTrackerLane() && r.sequenceIdx >= 0)
        {
            if (runtime.lastDispatchedSeqIdx >= 0
                && runtime.lastDispatchedSeqIdx != r.sequenceIdx)
                runtime.trackerCache->schedulePlaying (
                    runtime.lastDispatchedSeqIdx, -1.0, false);
            runtime.trackerCache->schedulePlaying (r.sequenceIdx, -1.0, true);
        }
        else if (runtime.isAudioLane())
        {
            const double bpm = owner.monitor_ != nullptr
                ? (double) owner.monitor_->tempo.get() : 120.0;
            const double sessionSR = owner.monitor_ != nullptr
                ? (double) owner.monitor_->sampleRate.get() : 48000.0;
            const double secsPerBeat = bpm > 0.0 ? 60.0 / bpm : 0.5;
            runtime.audioClipCache->schedulePlay (
                r.id, r.sourceId, -1.0, 0, r.looped,
                r.gainDb,
                (juce::int64) (r.fadeInBeats  * secsPerBeat * sessionSR),
                (juce::int64) (r.fadeOutBeats * secsPerBeat * sessionSR),
                (juce::int64) (r.lengthBeats  * secsPerBeat * sessionSR));
        }
        runtime.lastDispatchedRegion = r.id;
        runtime.lastDispatchedSeqIdx = r.sequenceIdx;
    }

    void mouseDrag (const MouseEvent& e) override
    {
        /* Envelope point drag -- updates the point's beat + gain.
         * Sort happens at mouseUp so neighbours don't reshuffle
         * mid-drag (would break the pointer to the in-flight
         * pt for subsequent drag events). */
        if (envGesture_.laneIdx >= 0)
        {
            envGesture_.dragActive = true;
            if (envGesture_.laneIdx >= owner.lanes_.size()) return;
            auto& lane = owner.lanes_.getReference (envGesture_.laneIdx);
            auto* r = lane.playlist.findRegion (envGesture_.regionId);
            if (r == nullptr) return;
            EnvelopePoint* pt = nullptr;
            for (auto& p : r->volumeEnvelope)
                if (p.id == envGesture_.pointId) { pt = &p; break; }
            if (pt == nullptr) return;

            const auto body = regionBodyRect (envGesture_.laneIdx, *r);
            pt->beatOffset = juce::jlimit (0.0, r->lengthBeats,
                                            envXToBeatOffset (e.x, body, *r));
            pt->gainDb     = juce::jlimit (-60.0f, 12.0f,
                                            envYToGainDb (e.y, body));
            repaintLane (envGesture_.laneIdx);
            return;
        }

        /* Range drag (lane strip OR ruler) is independent of the
         * region-gesture path -- it updates the range overlay and
         * skips lane mutation. */
        if (rangeDragging_)
        {
            const double mouseBeat = juce::jmax (0.0,
                (double) (e.x - kLabelW) / (double) kPxPerBeat);
            rangeStart_ = juce::jmin (rangeAnchor_, mouseBeat);
            rangeEnd_   = juce::jmax (rangeAnchor_, mouseBeat);
            repaint();
            return;
        }

        if (gesture_.laneIdx < 0) return;
        if (e.getDistanceFromDragStart() < kDragThresholdPx && ! gesture_.dragActive)
            return;
        gesture_.dragActive = true;

        if (gesture_.laneIdx >= owner.lanes_.size()) return;
        auto& lane = owner.lanes_.getReference (gesture_.laneIdx);
        auto* r = lane.playlist.findRegion (gesture_.regionId);
        if (r == nullptr) return;

        const double mouseBeat = juce::jmax (0.0,
            (double) (e.x - kLabelW) / (double) kPxPerBeat);

        /* Snap the cursor's beat-position to the configured grid
         * before computing the new region pos/length.  Snap to the
         * nearest division so the user can drag PAST a grid line a
         * little and still land cleanly.  When snap is disabled
         * (toggle off) we use the raw beat. */
        auto snap = [this] (double beats) noexcept -> double
        {
            if (! owner.snapEnabled_ || owner.snapDivision_ <= 0.0)
                return beats;
            return std::round (beats / owner.snapDivision_) * owner.snapDivision_;
        };

        if (gesture_.mode == Gesture::Move)
        {
            const double delta  = mouseBeat - gesture_.mouseDownXBeats;
            const double target = juce::jmax (0.0,
                snap (gesture_.originalPos + delta));

            /* moveRegion enforces no-overlap; if the target collides,
             * snap to the latest non-overlapping position before it.
             * For v1 we just attempt; failure leaves the region in
             * place. */
            lane.playlist.moveRegion (gesture_.regionId, target);
        }
        else /* Resize */
        {
            const double snappedEnd = snap (mouseBeat);
            const double newLength  = juce::jmax (kMinRegionBeats,
                snappedEnd - gesture_.originalPos);
            lane.playlist.resizeRegion (gesture_.regionId, newLength);
        }

        if (body_resizeNeeded()) resizeForLanes();
        else                     repaintLane (gesture_.laneIdx);
    }

    void mouseUp (const MouseEvent& e) override
    {
        /* Finish envelope point drag -- sort the envelope (in case
         * the point passed neighbours) + persist.  Clicks without
         * drag still consumed by the env path, no fallthrough to
         * region-launch. */
        if (envGesture_.laneIdx >= 0)
        {
            if (envGesture_.dragActive
                && envGesture_.laneIdx < owner.lanes_.size())
            {
                auto& lane = owner.lanes_.getReference (envGesture_.laneIdx);
                if (auto* r = lane.playlist.findRegion (envGesture_.regionId))
                    r->sortEnvelope();
                owner.writeLanesToSession();
                repaintLane (envGesture_.laneIdx);
            }
            envGesture_ = EnvGesture {};
            return;
        }

        /* Finish range drag (lane strip OR ruler).  Empty range
         * (start == end) collapses to no-range; non-empty range
         * stays armed for loop / future delete-in-range. */
        if (rangeDragging_)
        {
            rangeDragging_ = false;
            if (rangeEnd_ <= rangeStart_ + 1e-6)
            {
                rangeActive_ = false;
            }
            repaint();
            return;
        }

        if (gesture_.laneIdx < 0) return;

        const int   laneIdx     = gesture_.laneIdx;
        const bool  wasDragged  = gesture_.dragActive;
        const auto  gestureMode = gesture_.mode;
        const auto  regionId    = gesture_.regionId;

        gesture_ = Gesture {};

        if (laneIdx < 0 || laneIdx >= owner.lanes_.size()) return;
        auto& lane    = owner.lanes_.getReference (laneIdx);
        auto& runtime = owner.laneRuntime_.getReference (laneIdx);

        if (! wasDragged)
        {
            /* Click without drag = launch the region.  Existing
             * behaviour preserved. */
            const auto* r = lane.playlist.findRegion (regionId);
            if (r == nullptr) return;

            if (runtime.isTrackerLane() && r->sequenceIdx >= 0)
            {
                if (runtime.lastDispatchedSeqIdx >= 0
                    && runtime.lastDispatchedSeqIdx != r->sequenceIdx)
                    runtime.trackerCache->schedulePlaying (
                        runtime.lastDispatchedSeqIdx, -1.0, false);
                runtime.trackerCache->schedulePlaying (r->sequenceIdx, -1.0, true);
            }
            else if (runtime.isAudioLane())
            {
                const double bpm = owner.monitor_ != nullptr
                    ? (double) owner.monitor_->tempo.get() : 120.0;
                const double sessionSR = owner.monitor_ != nullptr
                    ? (double) owner.monitor_->sampleRate.get() : 48000.0;
                const double secsPerBeat = bpm > 0.0 ? 60.0 / bpm : 0.5;
                runtime.audioClipCache->schedulePlay (
                    r->id, r->sourceId, -1.0, 0, r->looped,
                    r->gainDb,
                    (juce::int64) (r->fadeInBeats  * secsPerBeat * sessionSR),
                    (juce::int64) (r->fadeOutBeats * secsPerBeat * sessionSR),
                    (juce::int64) (r->lengthBeats  * secsPerBeat * sessionSR));
            }
            runtime.lastDispatchedRegion = r->id;
            runtime.lastDispatchedSeqIdx = r->sequenceIdx;
            repaintLane (laneIdx);
            juce::ignoreUnused (e, gestureMode);
            return;
        }

        /* Drag/resize finished -- persist + invalidate cached
         * dispatch state so the new position fires next pass. */
        runtime.lastDispatchedRegion = juce::Uuid::null();
        owner.writeLanesToSession();
        resizeForLanes();
    }

    void mouseMove (const MouseEvent& e) override
    {
        /* Cursor feedback: change to resize cursor when hovering a
         * region's right-edge handle. */
        if (e.y < kRulerH)
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);
            return;
        }
        const int laneIdx = (e.y - kRulerH) / kLaneH;
        if (laneIdx < 0 || laneIdx >= owner.lanes_.size())
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);
            return;
        }
        if (e.x < kLabelW)
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);
            return;
        }
        const auto& lane = owner.lanes_.getReference (laneIdx);
        const int laneTopY = kRulerH + laneIdx * kLaneH;
        for (const auto& r : lane.playlist.regions())
        {
            const int regionStartX = kLabelW + (int) (r.positionBeats * kPxPerBeat);
            const int regionEndX   = kLabelW + (int) (r.endBeats()    * kPxPerBeat);
            if (e.x >= regionEndX - kEdgeHandlePx && e.x <= regionEndX
                && e.y >= laneTopY && e.y < laneTopY + kLaneH)
            {
                setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
                return;
            }
            if (e.x >= regionStartX && e.x < regionEndX
                && e.y >= laneTopY && e.y < laneTopY + kLaneH)
            {
                setMouseCursor (juce::MouseCursor::DraggingHandCursor);
                return;
            }
        }
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    /** Sampler-style zoom on mouse wheel.
     *
     *  - Plain wheel: horizontal zoom (px-per-beat).  Beat under the
     *    cursor stays under the cursor after zoom.
     *  - Ctrl/Cmd + wheel: vertical zoom (lane height).  Lane row
     *    under the cursor stays under the cursor after zoom.
     *  - Trackpad horizontal swipes (deltaX dominant) pass through
     *    to the viewport's default scroll handling. */
    void mouseWheelMove (const MouseEvent& e,
                          const juce::MouseWheelDetails& w) override
    {
        if (std::abs (w.deltaX) > std::abs (w.deltaY))
        {
            juce::Component::mouseWheelMove (e, w);
            return;
        }
        if (std::abs (w.deltaY) < 0.001f) return;

        const double factor = (w.deltaY > 0) ? 1.20 : (1.0 / 1.20);

        /* Ctrl/Cmd modifier -> vertical (lane height) zoom.  Anchor:
         * keep the lane row directly under the cursor pinned to its
         * current screen Y. */
        if (e.mods.isCtrlDown() || e.mods.isCommandDown())
        {
            const int anchorBodyY = e.y;
            const int oldScrollY  = owner.viewport_.getViewPositionY();
            const int anchorScreenY = anchorBodyY - oldScrollY;

            const int relY = juce::jmax (0, anchorBodyY - kRulerH);
            const int oldLaneIdx   = relY / kLaneH;
            const double yRelInLane = (double) (relY - oldLaneIdx * kLaneH)
                                     / (double) kLaneH;

            const int newH = juce::jlimit (kLaneHMin, kLaneHMax,
                                            (int) std::lround (kLaneH * factor));
            if (newH == kLaneH) return;
            kLaneH = newH;
            resizeForLanes();

            const int newAnchorBodyY = kRulerH + oldLaneIdx * kLaneH
                                     + (int) (yRelInLane * kLaneH);
            const int newScrollY = juce::jmax (0, newAnchorBodyY - anchorScreenY);
            const auto viewArea = owner.viewport_.getViewArea();
            owner.viewport_.setViewPosition (viewArea.getX(), newScrollY);
            return;
        }

        /* Plain wheel -> horizontal (px-per-beat) zoom.  Anchor: keep
         * the beat directly under the cursor pinned to its current
         * screen X. */
        const int anchorPxX  = e.x;
        const int stripPxX   = anchorPxX - kLabelW;
        const double anchorBeat = (stripPxX <= 0)
            ? 0.0
            : (double) stripPxX / (double) kPxPerBeat;

        const int newPxPerBeat = juce::jlimit (kPxPerBeatMin, kPxPerBeatMax,
                                                (int) std::lround (kPxPerBeat * factor));
        if (newPxPerBeat == kPxPerBeat) return;
        kPxPerBeat = newPxPerBeat;
        resizeForLanes();

        const int newAnchorBodyX = kLabelW + (int) (anchorBeat * kPxPerBeat);
        const int newScrollX     = juce::jmax (0, newAnchorBodyX - anchorPxX);
        const auto viewArea = owner.viewport_.getViewArea();
        owner.viewport_.setViewPosition (newScrollX, viewArea.getY());
    }

    /** Build + show the region right-click popup.  Stop-gap UI until
     *  the Ardour-style tool-mode toolbar lands -- once the toolbar
     *  exists, tool selection determines mouseDown behaviour and
     *  this menu goes away.  Until then, it's the only way to expose
     *  Region.looped toggling + Split. */
    void showRegionContextMenu (int laneIdx, juce::Uuid regionId, double atBeat)
    {
        auto& lane = owner.lanes_.getReference (laneIdx);
        const auto* r = lane.playlist.findRegion (regionId);
        if (r == nullptr) return;

        enum { ItemLoop = 1, ItemSplit = 2, ItemDelete = 3 };

        juce::PopupMenu menu;
        menu.addItem (ItemLoop,  "Loop",   true, r->looped);
        menu.addItem (ItemSplit, "Split at click",
                       atBeat > r->positionBeats + 0.0625
                       && atBeat < r->endBeats() - 0.0625);
        menu.addSeparator();
        menu.addItem (ItemDelete, "Delete");

        juce::Component::SafePointer<Body> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options(),
            [safe, laneIdx, regionId, atBeat] (int result)
            {
                auto* self = safe.getComponent();
                if (self == nullptr || result == 0) return;

                if (laneIdx < 0 || laneIdx >= self->owner.lanes_.size()) return;
                auto& l = self->owner.lanes_.getReference (laneIdx);

                bool changed = false;
                switch (result)
                {
                    case ItemLoop:
                        if (auto* m = l.playlist.findRegion (regionId))
                        {
                            m->looped = ! m->looped;
                            changed = true;
                        }
                        break;
                    case ItemSplit:
                        if (! l.playlist.splitRegion (regionId, atBeat).isNull())
                            changed = true;
                        break;
                    case ItemDelete:
                        if (l.playlist.removeRegion (regionId))
                        {
                            if (self->selectedRegion_ == regionId)
                            {
                                self->selectedLane_   = -1;
                                self->selectedRegion_ = juce::Uuid::null();
                            }
                            changed = true;
                        }
                        break;
                    default: break;
                }

                if (changed)
                {
                    self->owner.writeLanesToSession();
                    self->resizeForLanes();
                    self->repaintLane (laneIdx);
                }
            });
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        {
            if (selectedLane_ < 0 || selectedLane_ >= owner.lanes_.size())
                return false;
            auto& lane = owner.lanes_.getReference (selectedLane_);
            if (! lane.playlist.removeRegion (selectedRegion_))
                return false;
            const int laneIdx = selectedLane_;
            selectedLane_   = -1;
            selectedRegion_ = juce::Uuid::null();
            owner.writeLanesToSession();
            resizeForLanes();
            repaintLane (laneIdx);
            return true;
        }

        /* Shift + = / + : zoom in.  Shift + - : zoom out.  Mirrors the
         * mouse-wheel pinch step; anchor stays at the viewport centre
         * since there's no cursor X for keyboard zoom. */
        if (key.getModifiers().isShiftDown())
        {
            if (key.isKeyCode ('=') || key.isKeyCode ('+'))
            {
                zoomBy (1.20);
                return true;
            }
            if (key.isKeyCode ('-') || key.isKeyCode ('_'))
            {
                zoomBy (1.0 / 1.20);
                return true;
            }
        }
        return false;
    }

    /** Step the horizontal zoom by `factor` (>1 zooms in).  When
     *  `anchorBodyX < 0` the anchor is the centre of the visible
     *  viewport area so the user keeps roughly the same beats on
     *  screen across the step.  Shared between toolbar +/- buttons
     *  and the keyboard Shift +/- shortcut. */
    void zoomBy (double factor, int anchorBodyX = -1)
    {
        const auto viewArea = owner.viewport_.getViewArea();
        if (anchorBodyX < 0)
            anchorBodyX = viewArea.getCentreX();

        const int stripPxX = anchorBodyX - kLabelW;
        const double anchorBeat = (stripPxX <= 0)
            ? 0.0
            : (double) stripPxX / (double) kPxPerBeat;

        const int newPxPerBeat = juce::jlimit (kPxPerBeatMin, kPxPerBeatMax,
                                                (int) std::lround (kPxPerBeat * factor));
        if (newPxPerBeat == kPxPerBeat) return;
        kPxPerBeat = newPxPerBeat;
        resizeForLanes();

        const int newAnchorBodyX = kLabelW + (int) (anchorBeat * kPxPerBeat);
        const int anchorScreenX  = anchorBodyX - viewArea.getX();
        const int newScrollX     = juce::jmax (0, newAnchorBodyX - anchorScreenX);
        owner.viewport_.setViewPosition (newScrollX, viewArea.getY());
    }

    //==========================================================================
    // Layout helpers

    void resizeForLanes()
    {
        /* Floor + per-lane padding tuned to keep the strip flush with
         * content.  Floor of 16 beats = 4 bars at 4/4 -- enough empty
         * timeline to be a drop target on a fresh session, no further.
         * Padding of +4 beats past the last region = ~1 bar buffer to
         * make trailing drag-resize feel snappy without leaving a
         * sea of empty area past the content.  Was 32 + 8 which left
         * 8 bars of wasted scroll area on session open. */
        int maxBeats = 16;
        for (const auto& l : owner.lanes_)
        {
            double end = 0.0;
            for (const auto& r : l.playlist.regions())
                end = juce::jmax (end, r.endBeats());
            const int needed = (int) end + 4;
            if (needed > maxBeats) maxBeats = needed;
        }
        const int w = kLabelW + maxBeats * kPxPerBeat;
        const int h = kRulerH + juce::jmax (kLaneH, owner.lanes_.size() * kLaneH);
        if (w != getWidth() || h != getHeight())
            setSize (w, h);
        else
            repaint();
    }

    void repaintLane (int idx)
    {
        if (idx < 0) { repaint(); return; }
        repaint (0, kRulerH + idx * kLaneH, getWidth(), kLaneH);
    }

    void repaintPlayhead (int oldPxX, int newPxX)
    {
        const int W = getWidth();
        const int H = getHeight();
        if (oldPxX >= 0)
            repaint (juce::jlimit (0, W, oldPxX - 1), 0, 3, H);
        if (newPxX >= 0)
            repaint (juce::jlimit (0, W, newPxX - 1), 0, 3, H);
    }

    static constexpr int kLabelW         = 130;
    /* Vertical zoom: per-instance lane height.  Ctrl+wheel scales
     * within [kLaneHMin, kLaneHMax].  Default bumped from 64 -> 88
     * so the title strip + waveform + R/S/M cluster breathe. */
    int kLaneH = 88;
    static constexpr int kLaneHMin = 40;
    static constexpr int kLaneHMax = 240;
    /* Horizontal zoom: per-instance px-per-beat.  Sampler-style
     * mouse-wheel zoom (mouseWheelMove) scales this in [kPxPerBeatMin,
     * kPxPerBeatMax].  All paint + hit-test math uses the instance
     * value; the outer ArrangementView dereferences it via
     * body_->kPxPerBeat for playhead scrolling. */
    int kPxPerBeat = 24;
    static constexpr int kPxPerBeatMin = 4;
    static constexpr int kPxPerBeatMax = 256;
    static constexpr int kRulerH         = 24;   /* bars:beats ruler row */
    static constexpr int kEdgeHandlePx   = 6;    /* width of right-edge resize handle */
    static constexpr int kDragThresholdPx = 4;   /* pixels before mouseDown -> drag */
    static constexpr double kMinRegionBeats = 0.25;

    /* AudioThumbnail config: 1024 source samples per thumbnail
     * sample -- standard for DAW-style waveform overviews at the
     * zoom levels arrangement view uses.  Cache holds up to 256
     * recently-rendered sources; enough headroom that thumbnails
     * survive across multiple lane-add cycles without reload. */
    static constexpr int kThumbnailSamplesPerPixel = 1024;
    static constexpr int kThumbnailCacheEntries    = 256;

private:
    /** Paint the bars:beats ruler row at the top of the strip area.
     *  LCD-style faceplate matching TransportBar + MainDisplayPanel
     *  (matte-black bezel + cool-grey vertical gradient inside), bar
     *  numbers + ticks in LCD digit blue so the ruler reads as a
     *  hardware-style display flush with the transport cluster.  Bar
     *  count derives from the session's beatsPerBar (default 4 in
     *  4/4). */
    void paintRuler (Graphics& g)
    {
        /* LCD palette: matches the digit blue used in MainDisplayPanel
         * (content.cpp BPM / POS displays) so the ruler + transport
         * read as one continuous LCD strip.  Brightened from the
         * initial pass per visual feedback -- the body text + bar
         * markers now read clearly against the LCD gradient. */
        const juce::Colour kBezel       { 0xff'08'08'08 };
        const juce::Colour kBezelEdge   { 0xff'3a'3a'3a };
        const juce::Colour kLcdTop      { 0xff'14'19'1e };
        const juce::Colour kLcdBot      { 0xff'0c'0f'12 };
        const juce::Colour kLcdBlue     { 0xff'9e'dc'ff };  // bar marker / text -- bright
        const juce::Colour kLcdBlueMid  { 0xff'6f'b0'e0 };  // beat tick -- mid
        const juce::Colour kLcdBlueDim  { 0xff'4a'7c'a0 };  // sub-beat tick -- dim

        const Rectangle<int> rulerArea (0, 0, getWidth(), kRulerH);

        /* Outer matte-black bezel, then cool-grey vertical gradient
         * inset by 2 px so the bezel band is visible top + bottom.
         * Mirrors the TransportBar paint structure without rounded
         * corners -- the ruler spans the full width and clips to the
         * viewport edges, so rounded ends never show. */
        g.setColour (kBezel);
        g.fillRect (rulerArea);

        const auto inner = rulerArea.reduced (0, 2).toFloat();
        juce::ColourGradient lcdGrad (kLcdTop,
                                       inner.getX(), inner.getY(),
                                       kLcdBot,
                                       inner.getX(), inner.getBottom(),
                                       false);
        g.setGradientFill (lcdGrad);
        g.fillRect (inner);

        /* Hairline below the ruler against the lane bodies. */
        g.setColour (kBezelEdge);
        g.drawHorizontalLine (kRulerH - 1, 0.0f, (float) getWidth());

        /* Label column header: "Bars:Beats" in LCD blue. */
        g.setColour (kLcdBlue);
        g.setFont (monoFont (10.0f, juce::Font::bold));
        g.drawText ("Bars:Beats",
                    Rectangle<int> (6, 0, kLabelW - 12, kRulerH),
                    juce::Justification::centredLeft, true);

        /* Determine beats per bar from the session monitor; fall back
         * to 4 (4/4) if unavailable. */
        const int beatsPerBar = owner.monitor_ != nullptr
            ? juce::jmax (1, (int) owner.monitor_->beatsPerBar.get())
            : 4;

        const int stripX = kLabelW;
        const int stripW = getWidth() - kLabelW;
        const int totalBeats = stripW / kPxPerBeat + 1;

        g.setFont (monoFont (10.0f, juce::Font::bold));

        /* Tick subdivision scales with zoom so more ruler resolution
         * appears as the user zooms in.  Threshold = ~8 px between
         * adjacent ticks so they don't blur into a solid line:
         *   kPxPerBeat >= 32  -> 4 ticks/beat (1/16 in 4/4 if beat=quarter)
         *   kPxPerBeat >= 16  -> 2 ticks/beat (1/2 beat)
         *   else              -> 1 tick/beat (beats only). */
        const int subdiv = (kPxPerBeat >= 32) ? 4
                        : (kPxPerBeat >= 16) ? 2
                        : 1;
        const int subStepPx   = kPxPerBeat / subdiv;
        const int totalSubticks = totalBeats * subdiv;

        /* Viewport-clip the tick loop.  At low kPxPerBeat (zoomed out)
         * + a wide strip this loop fires drawVerticalLine + drawText
         * thousands of times per repaint; only ticks whose X lies
         * inside the clip rect are observable.  +/-1 sub padding on
         * each end protects bar-number labels that overhang their
         * tick by a few px. */
        const auto rulerClip = g.getClipBounds();
        const int subStart = juce::jmax (0,
            (rulerClip.getX() - stripX) / subStepPx - 1);
        const int subEnd   = juce::jmin (totalSubticks,
            (rulerClip.getRight() - stripX) / subStepPx + 1);

        for (int sub = subStart; sub <= subEnd; ++sub)
        {
            const int x      = stripX + sub * subStepPx;
            const int beat   = sub / subdiv;
            const int phase  = sub % subdiv;
            const bool atBeat = (phase == 0);
            const bool atBar  = atBeat && (beat % beatsPerBar) == 0;

            /* Tick height + colour by class:
             *   bar  -> full height, bright LCD blue (and bar number above)
             *   beat -> half height, mid LCD blue
             *   sub  -> short stub at bottom, dim LCD blue */
            int tickTop;
            juce::Colour tickCol;
            if (atBar)        { tickTop = 3;            tickCol = kLcdBlue;    }
            else if (atBeat)  { tickTop = kRulerH - 12; tickCol = kLcdBlueMid; }
            else              { tickTop = kRulerH - 6;  tickCol = kLcdBlueDim; }

            g.setColour (tickCol);
            g.drawVerticalLine (x, (float) tickTop, (float) (kRulerH - 2));

            if (atBar)
            {
                const int barNum = beat / beatsPerBar + 1;
                g.setColour (kLcdBlue);
                g.drawText (juce::String (barNum),
                            x + 3, 1, 28, kRulerH - 4,
                            juce::Justification::topLeft);
            }
        }

        /* Playhead overlay on the ruler. */
        const int phx = stripX + (int) (owner.lastBeat_ * kPxPerBeat);
        if (phx >= stripX && phx < getWidth())
        {
            g.setColour (Colours::limegreen);
            g.drawVerticalLine (phx, 0.0f, (float) kRulerH);
        }
    }

    /** ARM / MUTE / SOLO button stack lives on the right side of
     *  the lane label area, three buttons tall.  Visual style mirrors
     *  SessionView column-header buttons: flat fillRect + drawRect
     *  with full-word labels and tint-derived idle colour.  Tracker
     *  lanes paint only MUTE + SOLO (ARM slot is left blank so M+S
     *  stay at the same Y positions across lane kinds).
     *
     *  Cluster X is right-anchored to the label area so shrinking
     *  kLabelW just shrinks the lane-name column to its left.  Vertical
     *  stacking trades horizontal label width for height -- buttons
     *  read big because they span the full width of the right column. */
    static constexpr int kBtnW       = 46;
    static constexpr int kBtnH       = 14;
    static constexpr int kBtnGap     = 2;
    static constexpr int kBtnRightPad = 4;
    static constexpr int kBtnTopPad  = 4;

    int laneButtonX() const noexcept
    {
        return kLabelW - kBtnW - kBtnRightPad;
    }

    Rectangle<int> armToggleRect (int laneIdx) const noexcept
    {
        const int y = kRulerH + laneIdx * kLaneH + kBtnTopPad;
        return Rectangle<int> (laneButtonX(), y, kBtnW, kBtnH);
    }

    Rectangle<int> muteToggleRect (int laneIdx) const noexcept
    {
        const int y = kRulerH + laneIdx * kLaneH + kBtnTopPad
                    + (kBtnH + kBtnGap);
        return Rectangle<int> (laneButtonX(), y, kBtnW, kBtnH);
    }

    Rectangle<int> soloToggleRect (int laneIdx) const noexcept
    {
        const int y = kRulerH + laneIdx * kLaneH + kBtnTopPad
                    + (kBtnH + kBtnGap) * 2;
        return Rectangle<int> (laneButtonX(), y, kBtnW, kBtnH);
    }

    /** Paint a volume envelope curve over the audio region body
     *  plus an attenuation-shading polygon above the curve.
     *
     *  Visual: the area BETWEEN the top of the body and the envelope
     *  curve is dimmed by a translucent black overlay, so the
     *  shaded portion of the clip reads as "this much gain is being
     *  cut here".  Below the curve stays at full waveform intensity.
     *
     *  Empty envelope -> single horizontal line at 0 dB (the curve
     *  defaults to unity gain on first paint); the area above that
     *  line is also shaded so the surface always has something to
     *  read against. */
    void paintVolumeEnvelope (Graphics& g,
                                const Rectangle<int>& body,
                                const Region& r,
                                juce::Colour tint,
                                bool selected) const
    {
        if (body.getWidth() < 4 || body.getHeight() < 6) return;
        if (r.lengthBeats <= 1e-6) return;

        /* Map gainDb to Y over a [+6, -24] dB window.  0 dB sits at
         * 80 % up the body so the user can pull above unity gain too. */
        constexpr float kTopDb = 6.0f;
        constexpr float kBotDb = -24.0f;
        const int yPad = 3;
        auto gainToY = [&] (float dB) noexcept
        {
            const float t = juce::jlimit (0.0f, 1.0f,
                (kTopDb - dB) / (kTopDb - kBotDb));
            return (float) body.getY() + yPad
                 + t * (float) juce::jmax (1, body.getHeight() - yPad * 2);
        };
        auto beatToX = [&] (double localBeat) noexcept
        {
            const double t = juce::jlimit (0.0, 1.0, localBeat / r.lengthBeats);
            return (float) body.getX() + (float) (t * body.getWidth());
        };

        /* Selection brightens the curve (no more hard white -- matches
         * the body-fill brightening above so the region reads as
         * "selected" without three different highlight colours). */
        const juce::Colour lineCol = selected
            ? tint.brighter (0.75f)
            : tint.brighter (0.45f).withAlpha (0.85f);

        /* Build a stroke path for the envelope curve AND an aligned
         * polygon path for the attenuation shading above the curve.
         * Both share the same per-segment subdivision so the shaded
         * polygon's bottom edge sits perfectly on the line. */
        juce::Path stroke;
        juce::Path shadePoly;

        constexpr int kSubdiv = 12;

        auto pushSegment = [&] (double beatA, float dbA,
                                 double beatB, float dbB,
                                 EnvelopeCurve curve, bool first)
        {
            const float xA = beatToX (beatA);
            const float yA = gainToY (dbA);
            if (first)
            {
                stroke.startNewSubPath (xA, yA);
                shadePoly.startNewSubPath ((float) body.getX(), (float) body.getY());
                shadePoly.lineTo (xA, (float) body.getY());
                shadePoly.lineTo (xA, yA);
            }
            if (curve == EnvelopeCurve::Hold)
            {
                const float xB = beatToX (beatB);
                stroke.lineTo (xB, yA);
                stroke.lineTo (xB, gainToY (dbB));
                shadePoly.lineTo (xB, yA);
                shadePoly.lineTo (xB, gainToY (dbB));
                return;
            }
            if (curve == EnvelopeCurve::Linear)
            {
                const float xB = beatToX (beatB);
                const float yB = gainToY (dbB);
                stroke.lineTo (xB, yB);
                shadePoly.lineTo (xB, yB);
                return;
            }
            for (int k = 1; k <= kSubdiv; ++k)
            {
                const double t = (double) k / (double) kSubdiv;
                double shaped = t;
                if (curve == EnvelopeCurve::Exponential) shaped = t * t;
                if (curve == EnvelopeCurve::Smooth)
                    shaped = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * t);
                const double beat = beatA + t * (beatB - beatA);
                const double db   = dbA + shaped * (dbB - dbA);
                const float xK = beatToX (beat);
                const float yK = gainToY ((float) db);
                stroke.lineTo (xK, yK);
                shadePoly.lineTo (xK, yK);
            }
        };

        if (r.volumeEnvelope.empty())
        {
            /* No breakpoints -> render an implicit "constant at 0 dB"
             * line spanning the full region.  Shading above is still
             * meaningful (zero, since 0 dB is at the top window edge,
             * but the surface IS there to double-click). */
            const float y = gainToY (0.0f);
            stroke.startNewSubPath ((float) body.getX(),     y);
            stroke.lineTo            ((float) body.getRight(), y);
            shadePoly.startNewSubPath ((float) body.getX(),     (float) body.getY());
            shadePoly.lineTo            ((float) body.getRight(), (float) body.getY());
            shadePoly.lineTo            ((float) body.getRight(), y);
            shadePoly.lineTo            ((float) body.getX(),     y);
            shadePoly.closeSubPath();
        }
        else
        {
            for (size_t i = 0; i + 1 < r.volumeEnvelope.size(); ++i)
            {
                const auto& a = r.volumeEnvelope[i];
                const auto& b = r.volumeEnvelope[i + 1];
                pushSegment (a.beatOffset, a.gainDb,
                              b.beatOffset, b.gainDb,
                              a.curve, i == 0);
            }
            /* Close the shading polygon: traverse along the top edge
             * back to the starting X. */
            const auto& last = r.volumeEnvelope.back();
            const float xEnd = beatToX (last.beatOffset);
            shadePoly.lineTo (xEnd, (float) body.getY());
            shadePoly.closeSubPath();
        }

        /* Attenuation shading -- translucent black above the curve.
         * Sits below the envelope line so it never hides the curve
         * itself; sits above the waveform so the dimming is visible. */
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillPath (shadePoly);

        /* Envelope line on top of everything. */
        g.setColour (lineCol);
        g.strokePath (stroke, juce::PathStrokeType (selected ? 1.5f : 1.0f));

        /* Breakpoint dots -- larger + ringed when selected. */
        const float dotR = selected ? 3.0f : 2.5f;
        for (const auto& pt : r.volumeEnvelope)
        {
            const float x = beatToX (pt.beatOffset);
            const float y = gainToY (pt.gainDb);
            g.setColour (lineCol);
            g.fillEllipse (x - dotR, y - dotR, dotR * 2, dotR * 2);
            if (selected)
            {
                g.setColour (juce::Colours::black);
                g.drawEllipse (x - dotR, y - dotR, dotR * 2, dotR * 2, 1.0f);
            }
        }
    }

    /** Paint a piano-roll style overview of a tracker sequence into
     *  the region body.  Walks every note_on row across every track
     *  of the sequence; maps row index -> X, MIDI pitch -> inverted
     *  Y over the sequence's used pitch range.  Thread-safe: takes
     *  the TrackerNode engine lock for the duration of the iteration
     *  so vht doesn't free the sequence/track arrays underneath. */
    void paintTrackerThumb (Graphics& g,
                              const Rectangle<int>& body,
                              TrackerNode* trk,
                              int sequenceIdx,
                              juce::Colour tint) const
    {
        if (trk == nullptr || sequenceIdx < 0) return;
        if (body.getWidth() < 4 || body.getHeight() < 4) return;

        juce::ScopedLock sl (trk->engineLock());
        auto* mod = trk->modulePtr();
        if (mod == nullptr || sequenceIdx >= mod->nseq) return;
        auto* seq = mod->seq[sequenceIdx];
        if (seq == nullptr || seq->ntrk <= 0) return;

        /* First pass: discover the in-use MIDI pitch range across
         * all tracks/cols/rows.  If no notes, bail. */
        int minNote = 127, maxNote = 0;
        int totalRows = 0;
        for (int t = 0; t < seq->ntrk; ++t)
        {
            auto* trkP = seq->trk[t];
            if (trkP == nullptr) continue;
            if (trkP->nrows > totalRows) totalRows = trkP->nrows;
            for (int c = 0; c < trkP->ncols; ++c)
                for (int r = 0; r < trkP->nrows; ++r)
                {
                    const auto& row = trkP->rows[c][r];
                    if (row.type == note_on && row.note > 0)
                    {
                        if (row.note < minNote) minNote = row.note;
                        if (row.note > maxNote) maxNote = row.note;
                    }
                }
        }
        if (totalRows <= 0 || minNote > maxNote) return;

        /* Give a one-octave minimum range so a single-pitch loop
         * doesn't paint as one flat line at the top of the body. */
        const int pitchPad = juce::jmax (0, 12 - (maxNote - minNote));
        const int loNote = juce::jmax (0, minNote - pitchPad / 2);
        const int hiNote = juce::jmin (127, maxNote + (pitchPad - pitchPad / 2));
        const double pitchRange = (double) juce::jmax (1, hiNote - loNote);

        const int pad = 2;
        const int innerW = juce::jmax (1, body.getWidth() - pad * 2);
        const int innerH = juce::jmax (1, body.getHeight() - pad * 2);
        const int innerX = body.getX() + pad;
        const int innerY = body.getY() + pad;

        g.setColour (tint.withMultipliedBrightness (1.35f)
                          .withMultipliedSaturation (0.85f));

        /* Second pass: paint each note_on row as a small horizontal
         * tick.  Width derives from sequence-row-density so notes
         * read as ticks at low zoom and as bars when zoomed in. */
        for (int t = 0; t < seq->ntrk; ++t)
        {
            auto* trkP = seq->trk[t];
            if (trkP == nullptr || trkP->nrows <= 0) continue;
            const float rowStepPx = (float) innerW / (float) trkP->nrows;
            const int tickW = juce::jmax (1, (int) std::ceil (rowStepPx));

            for (int c = 0; c < trkP->ncols; ++c)
                for (int r = 0; r < trkP->nrows; ++r)
                {
                    const auto& row = trkP->rows[c][r];
                    if (row.type != note_on || row.note <= 0) continue;

                    const float xRel = (float) r / (float) trkP->nrows;
                    const float yRel = (float) (row.note - loNote) / (float) pitchRange;
                    const int dotX = innerX + (int) (xRel * innerW);
                    const int dotY = innerY + innerH - 1
                                     - (int) (yRel * (innerH - 1));
                    g.fillRect (dotX, dotY, tickW, 2);
                }
        }
    }

    void paintLane (Graphics& g, int laneIdx)
    {
        const auto& lane    = owner.lanes_.getReference (laneIdx);
        const auto& runtime = owner.laneRuntime_.getReference (laneIdx);
        const int y = kRulerH + laneIdx * kLaneH;
        const Rectangle<int> bounds (0, y, getWidth(), kLaneH);

        /* Bitwig-style vertical tint strip on the LEFT edge of the
         * label area, low-alpha wash for the rest, monospace font
         * for the lane name. */
        constexpr int kTintStripW = 6;
        const juce::Colour kGutterColour { 0xff'14'14'14 };
        const juce::Colour kRowDividerColour { 0xff'22'22'22 };
        const juce::Colour kRowTextColour { 0xff'a8'a8'a8 };

        const bool orphan = runtime.isOrphan();
        const bool isAudio = runtime.isAudioLane();
        const juce::Colour fullTint = lane.colour;
        const juce::Colour tint = orphan
            ? fullTint.withSaturation (0.2f).withBrightness (0.4f)
            : fullTint;

        /* Label area background only -- the strip area to the right
         * inherits the Body's contentBackgroundColor fill from paint()
         * so lanes don't repaint the entire row width every tick.
         * Per-row alternating fills + strip-area tint wash were
         * removed per visual-design call 2026-05-24: only the label
         * column carries the gutter / tint identity; the strip body
         * stays uniform background with regions floating on top. */
        const Rectangle<int> labelArea (0, y, kLabelW, kLaneH);
        g.setColour (kGutterColour);
        g.fillRect (labelArea);

        /* Vertical tint strip on the LEFT edge + low-alpha wash
         * across the rest of the label area. */
        g.setColour (tint);
        g.fillRect (labelArea.getX(), labelArea.getY(),
                    kTintStripW, labelArea.getHeight() - 1);
        g.setColour (tint.withAlpha (0.12f));
        g.fillRect (labelArea.getX() + kTintStripW, labelArea.getY(),
                    labelArea.getWidth() - kTintStripW - 1,
                    labelArea.getHeight() - 1);

        /* Lane name + kind pill sit in the left column of the label
         * area, between the tint strip and the right-anchored button
         * stack.  Name on top, pill below, both left-aligned. */
        const int labelInset = kTintStripW + 4;
        const int leftColW   = laneButtonX() - labelInset - 2;
        g.setColour (orphan ? tint.withAlpha (0.55f) : tint);
        g.setFont (monoFont (
                                      12.0f, juce::Font::bold));
        juce::String label = lane.name.isNotEmpty()
                                ? lane.name
                                : (isAudio ? juce::String ("Audio") : juce::String ("Tracker"));
        if (orphan) label += " (orphan)";
        g.drawText (label,
                    Rectangle<int> (labelArea.getX() + labelInset,
                                     labelArea.getY() + 3,
                                     leftColW, 14),
                    juce::Justification::centredLeft, true);

        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.setFont (monoFont (
                                      10.0f, juce::Font::plain));
        const juce::String pill = isAudio ? "audio"
                                : runtime.isTrackerLane() ? "trk"
                                : "?";
        g.drawText (pill,
                    Rectangle<int> (labelArea.getX() + labelInset,
                                     labelArea.getY() + 19,
                                     leftColW, 12),
                    juce::Justification::centredLeft);

        /* MUTE / SOLO / ARM cluster at the bottom of the label area.
         * Flat-panel style mirroring SessionView column buttons:
         * fillRect + drawRect, full-word labels, tint-derived idle
         * background so the row reads as a horizontal cousin of a
         * session column. */
        const juce::Colour btnTint = tint.withMultipliedSaturation (0.6f)
                                         .withMultipliedBrightness (0.55f);
        auto drawFlatButton = [&] (const Rectangle<int>& rect,
                                    bool active,
                                    juce::Colour activeFill,
                                    juce::Colour activeText,
                                    const juce::String& label)
        {
            g.setColour (active ? activeFill : btnTint);
            g.fillRect (rect);
            g.setColour (kRowDividerColour);
            g.drawRect (rect, 1);
            g.setColour (active ? activeText
                                : juce::Colours::white.withAlpha (0.70f));
            g.setFont (monoFont (
                                          9.0f, juce::Font::bold));
            g.drawText (label, rect, juce::Justification::centred);
        };

        drawFlatButton (muteToggleRect (laneIdx), lane.muted,
                         juce::Colour { 0xff'c0'30'30 },
                         juce::Colours::white, "MUTE");
        drawFlatButton (soloToggleRect (laneIdx), lane.soloed,
                         juce::Colour { 0xff'd5'b0'30 },
                         juce::Colours::black, "SOLO");

        /* Arm toggle (audio lanes only).  Same pill idiom, with a
         * recording-mode growth + red-200 letter overlay so the user
         * sees capture is in flight without losing the round button
         * style. */
        const bool transportRecording = owner.monitor_ != nullptr
                                      && owner.monitor_->recording.get();
        const bool capturing = isAudio && lane.armed && transportRecording;

        if (isAudio)
        {
            const auto rect = armToggleRect (laneIdx);

            const Colour fillCol = lane.armed
                ? (capturing ? Colour::fromRGB (255, 60, 60)
                             : Colour::fromRGB (220, 70, 70))
                : btnTint;
            g.setColour (fillCol);
            g.fillRect (rect);
            g.setColour (capturing ? Colour::fromRGB (255, 160, 160)
                                    : kRowDividerColour);
            g.drawRect (rect, capturing ? 2 : 1);
            g.setColour (lane.armed ? juce::Colours::white
                                    : juce::Colours::white.withAlpha (0.70f));
            g.setFont (monoFont (
                                          9.0f, juce::Font::bold));
            /* "REC" both at-rest and while capturing -- the fill
             * colour conveys state (dim/bright red when armed-but-
             * idle vs capturing) so the label can stay one word. */
            g.drawText ("REC", rect, juce::Justification::centred);
        }

        /* Strip area beat grid only -- no per-lane background fill or
         * tint wash.  Body::paint's fillAll handles the strip's base
         * colour and regions paint themselves on top; what's left for
         * paintLane to draw across the strip is the bar + beat grid
         * lines.  Bar lines slightly darker than the body bg, beat
         * lines (1/4-note in 4/4) one notch dimmer below that so the
         * timeline tempo reads as a glanceable grid behind regions. */
        const Rectangle<int> stripArea (kLabelW, y, getWidth() - kLabelW, kLaneH);

        const int gridBeatsPerBar = owner.monitor_ != nullptr
            ? juce::jmax (1, (int) owner.monitor_->beatsPerBar.get())
            : 4;
        const juce::Colour kStripGridBar  { 0xff'2a'2a'2a };
        const juce::Colour kStripGridBeat { 0xff'1c'1c'1c };

        const auto gridClip = g.getClipBounds();
        const int laneBeatStart = juce::jmax (0,
            (gridClip.getX() - stripArea.getX()) / kPxPerBeat - 1);
        const int laneBeatEnd   = juce::jmin (stripArea.getWidth() / kPxPerBeat + 1,
            (gridClip.getRight() - stripArea.getX()) / kPxPerBeat + 1);

        /* Beat lines only show when each beat is at least ~10 px wide;
         * below that the grid would solid-fill the strip with dim
         * gray.  Bar lines always draw. */
        const bool showBeatLines = (kPxPerBeat >= 10);

        for (int beat = laneBeatStart; beat <= laneBeatEnd; ++beat)
        {
            const bool barLine = (beat % gridBeatsPerBar) == 0;
            if (! barLine && ! showBeatLines) continue;

            g.setColour (barLine ? kStripGridBar : kStripGridBeat);
            g.drawVerticalLine (stripArea.getX() + beat * kPxPerBeat,
                                (float) stripArea.getY(),
                                (float) stripArea.getBottom());
        }

        /* Regions painted in the graph-block visual language: filled
         * body in a desaturated lane-tint, then a tint outer stroke
         * + 0.6α-black inner ring with rounded corners.  Matches
         * BlockComponent::paint at ui/block.cpp:915-921 so the
         * arrangement reads as a row of mini graph blocks. */
        constexpr float kCornerSize = 2.0f;
        /* Viewport-clip the regions loop.  Each per-region branch
         * builds 2-3 Paths + sets a clip + walks the audio thumbnail
         * (or paintTrackerThumb) + paintVolumeEnvelope.  Skipping the
         * branch entirely when the region rect is outside the dirty
         * area is the biggest single timeline-zoom + scroll win at
         * high clip counts -- the dirty rect from a playhead tick is
         * a thin vertical strip, so all but a handful of regions per
         * lane drop out. */
        const auto regionClip = g.getClipBounds();
        for (const auto& r : lane.playlist.regions())
        {
            const int xs = stripArea.getX() + (int) (r.positionBeats * kPxPerBeat);
            const int ws = juce::jmax (4, (int) (r.lengthBeats * kPxPerBeat));
            Rectangle<int> rect (xs, stripArea.getY() + 4,
                                 ws, stripArea.getHeight() - 8);

            if (! regionClip.intersects (rect))
                continue;

            const bool selected = (laneIdx == selectedLane_ && r.id == selectedRegion_);

            /* Tint = lane colour, desaturated + dimmed for orphan
             * lanes.  The previous active-region brightening (when
             * the playhead crossed into a region) was removed -- it
             * fired a per-region repaint on every dispatch transition
             * across every lane, which doesn't scale to 32-64 track
             * sessions and the visual delta was modest enough that
             * the playhead line itself reads as the "now playing"
             * marker. */
            juce::Colour borderTint = lane.colour;
            if (orphan)  borderTint = borderTint.withMultipliedSaturation (0.3f)
                                                 .withMultipliedBrightness (0.6f);

            /* Selection visual = slight brightening of the body +
             * title fills.  The earlier white outer stroke read as
             * over-emphasized (and read worse when 32+ tracks were
             * stacked); a small brightness bump keeps the selected
             * region visible without competing with playhead /
             * recording indicators. */
            juce::Colour fill = borderTint.withMultipliedSaturation (0.55f)
                                          .withMultipliedBrightness (0.45f);
            if (selected) fill = fill.brighter (0.30f);

            /* Region body fill (rounded). */
            g.setColour (fill);
            g.fillRoundedRectangle (rect.toFloat(), kCornerSize);

            /* Bitwig-style region title strip: thin band across the
             * top of the region in a more saturated/darker shade of
             * the lane tint.  Holds the region label so the waveform
             * area below stays uncluttered.  Squared-off bottom via
             * an inner overdraw with the body fill below the band. */
            constexpr int kTitleH = 13;
            const Rectangle<int> titleRect (rect.getX(), rect.getY(),
                                             rect.getWidth(),
                                             juce::jmin (kTitleH, rect.getHeight()));
            const Rectangle<int> bodyRect (rect.getX(),
                                            rect.getY() + titleRect.getHeight(),
                                            rect.getWidth(),
                                            juce::jmax (0, rect.getHeight() - titleRect.getHeight()));
            juce::Colour titleFill = borderTint.withMultipliedBrightness (0.70f)
                                                .withMultipliedSaturation (1.10f);
            if (selected) titleFill = titleFill.brighter (0.25f);
            {
                /* Clip to the rounded outer rect so the title band's
                 * top corners follow the region's rounding while its
                 * bottom edge stays straight against the body fill. */
                juce::Graphics::ScopedSaveState save (g);
                juce::Path clipPath;
                clipPath.addRoundedRectangle (rect.toFloat(), kCornerSize);
                g.reduceClipRegion (clipPath);
                g.setColour (titleFill);
                g.fillRect (titleRect);

                /* (Envelope-driven attenuation shading is drawn by
                 * paintVolumeEnvelope below -- it shades only the area
                 * ABOVE the envelope curve, so the visible "dimmed"
                 * region maps directly to how much gain is being cut.
                 * No global gradient here.) */
            }

            /* Waveform overlay for audio regions (sequenceIdx < 0),
             * clipped to the *body* rect only so it never overlaps the
             * title band. */
            if (isAudio && r.sequenceIdx < 0 && bodyRect.getHeight() > 2)
            {
                if (auto* thumb = const_cast<Body*> (this)->getThumbnail (r.sourceId))
                {
                    const double totalSeconds = thumb->getTotalLength();
                    if (totalSeconds > 0.0)
                    {
                        const double bpm = owner.monitor_ != nullptr
                                              ? (double) owner.monitor_->tempo.get()
                                              : 120.0;
                        const double srcStartSec = r.startBeats * 60.0 / bpm;
                        const double srcLenSec   = r.lengthBeats * 60.0 / bpm;
                        const double srcEndSec   = juce::jmin (totalSeconds,
                                                                srcStartSec + srcLenSec);

                        juce::Graphics::ScopedSaveState save (g);
                        juce::Path clipPath;
                        clipPath.addRoundedRectangle (
                            rect.toFloat().reduced (2.0f, 2.0f),
                            juce::jmax (0.5f, kCornerSize - 1.0f));
                        g.reduceClipRegion (clipPath);

                        g.setColour (borderTint.withMultipliedBrightness (1.2f)
                                               .withMultipliedSaturation (0.8f));
                        thumb->drawChannels (g,
                                              bodyRect.reduced (3, 2),
                                              srcStartSec, srcEndSec, 0.85f);
                    }
                }
            }
            else if (! isAudio && r.sequenceIdx >= 0 && bodyRect.getHeight() > 2)
            {
                /* Tracker pattern thumbnail: piano-roll-style dots so
                 * each region carries a glanceable shape of its
                 * sequence, paralleling the audio waveform overlay. */
                juce::Graphics::ScopedSaveState save (g);
                juce::Path clipPath;
                clipPath.addRoundedRectangle (
                    rect.toFloat().reduced (2.0f, 2.0f),
                    juce::jmax (0.5f, kCornerSize - 1.0f));
                g.reduceClipRegion (clipPath);
                paintTrackerThumb (g, bodyRect.reduced (3, 2),
                                    runtime.trackerCache,
                                    r.sequenceIdx, borderTint);
            }

            /* Volume envelope -- audio regions only.  Empty envelope
             * still draws a flat line at the static gainDb so the
             * surface is always there to double-click on. */
            if (isAudio && r.sequenceIdx < 0 && bodyRect.getHeight() > 6)
            {
                juce::Graphics::ScopedSaveState save (g);
                juce::Path clipPath;
                clipPath.addRoundedRectangle (
                    rect.toFloat().reduced (2.0f, 2.0f),
                    juce::jmax (0.5f, kCornerSize - 1.0f));
                g.reduceClipRegion (clipPath);
                paintVolumeEnvelope (g, bodyRect.reduced (3, 2),
                                       r, borderTint, selected);
            }

            /* Graph-block borders: tinted outer stroke + black inner
             * ring with rounded corners.  Selection is now signalled
             * by the brighter body / title fills above; the outer
             * stroke stays at the lane tint regardless so multiple
             * stacked tracks don't compete with hard white outlines. */
            constexpr float outerWidth = 1.5f;
            g.setColour (borderTint);
            g.drawRoundedRectangle (rect.toFloat(), kCornerSize, outerWidth);
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.drawRoundedRectangle (
                rect.toFloat().reduced (outerWidth, outerWidth),
                juce::jmax (0.5f, kCornerSize - outerWidth),
                1.0f);

            /* Region label in the title strip.  Bar-position prefix
             * (e.g. "5 Drums") matches Bitwig + Cubase phrasing -- the
             * region is anchored to that bar so the bar number reads
             * as the region's primary identifier.  Pattern regions
             * fall back to their "P<idx>" tag with no bar prefix. */
            const int beatsPerBar = owner.monitor_ != nullptr
                ? juce::jmax (1, (int) owner.monitor_->beatsPerBar.get())
                : 4;
            const int barAt = (int) (r.positionBeats / beatsPerBar) + 1;

            juce::String labelText;
            if (r.sequenceIdx >= 0)
                labelText = "P" + String (r.sequenceIdx);
            else
                labelText = String (barAt) + " " +
                             (r.name.isNotEmpty() ? r.name : String ("Audio"));

            g.setColour (juce::Colours::white.withAlpha (0.92f));
            g.setFont (monoFont (
                                          10.0f, juce::Font::bold));
            g.drawText (labelText,
                        titleRect.reduced (5, 0),
                        juce::Justification::centredLeft, true);
        }

        /* Live-recording placeholder: while transport is capturing
         * + this lane is armed, paint a red translucent rect from
         * recordStartBeat to the current playhead, with a
         * "Recording..." label.  On stop, the AudioClipNode commit
         * handler appends a real Region to the playlist and the
         * placeholder vanishes (its source data lives in the
         * playlist now). */
        if (capturing)
        {
            const double startBeat = owner.recordStartBeat_;
            const double endBeat   = owner.lastBeat_;
            if (endBeat > startBeat)
            {
                const int rx = stripArea.getX() + (int) (startBeat * kPxPerBeat);
                const int rw = juce::jmax (4, (int) ((endBeat - startBeat) * kPxPerBeat));
                const Rectangle<int> recRect (rx, stripArea.getY() + 4,
                                              rw, stripArea.getHeight() - 8);

                g.setColour (Colour::fromRGB (220, 60, 60).withAlpha (0.55f));
                g.fillRect (recRect);
                g.setColour (Colour::fromRGB (255, 120, 120));
                g.drawRect (recRect, 1);
                g.setColour (Colours::white);
                g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
                g.drawText ("Recording...", recRect.reduced (6, 0),
                            juce::Justification::centredLeft, true);
            }
        }

        /* Playhead overlay. */
        const double phb = owner.lastBeat_;
        const int phx = stripArea.getX() + (int) (phb * kPxPerBeat);
        if (phx >= stripArea.getX() && phx < stripArea.getRight())
        {
            g.setColour (Colours::limegreen.withAlpha (0.9f));
            g.drawVerticalLine (phx,
                                (float) stripArea.getY(),
                                (float) stripArea.getBottom());
        }
    }

    /** Returns true if the body's current size doesn't fit the
     *  current lane content extent (e.g. resize pushed a region past
     *  the existing total width).  resizeForLanes() recomputes. */
    bool body_resizeNeeded() const noexcept
    {
        double maxEnd = 0.0;
        for (const auto& l : owner.lanes_)
            for (const auto& r : l.playlist.regions())
                maxEnd = juce::jmax (maxEnd, r.endBeats());
        const int needW = kLabelW + ((int) maxEnd + 8) * kPxPerBeat;
        const int needH = juce::jmax (kLaneH, owner.lanes_.size() * kLaneH);
        return needW != getWidth() || needH != getHeight();
    }

    /** Lookup or lazily-create the juce::AudioThumbnail for the given
     *  AudioFileSource uuid.  Returns nullptr if the source isn't
     *  registered (region might point at a missing source, e.g.
     *  imported from a session whose source paths have moved). */
    juce::AudioThumbnail* getThumbnail (juce::Uuid sourceId)
    {
        if (sourceId.isNull()) return nullptr;

        UuidHashKey key { sourceId };
        auto it = thumbnails_.find (key);
        if (it != thumbnails_.end())
            return it->second.get();

        auto src = SourceRegistry::get().findAudioFile (sourceId);
        if (src == nullptr)
            return nullptr;

        auto thumb = std::make_unique<juce::AudioThumbnail> (
            kThumbnailSamplesPerPixel, formatManager_, thumbnailCache_);
        /* setSource owns the InputSource; FileInputSource opens via
         * juce::File which lives on the Linux path under our winelib
         * build (_WIN32 undefined by winelib_compat.h).  Background-
         * thread reader, not the audio thread. */
        thumb->setSource (new juce::FileInputSource (src->file()));
        thumb->addChangeListener (this);

        auto* raw = thumb.get();
        thumbnails_.emplace (key, std::move (thumb));
        return raw;
    }

    ArrangementView& owner;

    /* Drop hover state for visual feedback during external file drag. */
    bool dropHover_         = false;
    int  dropHoverLaneIdx_  = -1;

    /* Region selection state.  selectedLane_ < 0 means no selection. */
    int        selectedLane_   = -1;
    juce::Uuid selectedRegion_;

    /* Active gesture (move / resize) -- valid between mouseDown and
     * mouseUp.  laneIdx < 0 means no gesture. */
    struct Gesture {
        enum Mode { Move, Resize };
        int        laneIdx         = -1;
        juce::Uuid regionId;
        double     originalPos     = 0.0;
        double     originalLen     = 0.0;
        double     mouseDownXBeats = 0.0;
        Mode       mode            = Move;
        bool       dragActive      = false;
    };
    Gesture gesture_;

    /** Live drag of an envelope breakpoint.  Set in mouseDown when
     *  the cursor lands on a breakpoint dot; updated in mouseDrag;
     *  cleared in mouseUp.  Decoupled from the region-Gesture so a
     *  Select-tool click on a dot doesn't also start a move/resize. */
    struct EnvGesture {
        int        laneIdx    = -1;
        juce::Uuid regionId;
        juce::Uuid pointId;
        bool       dragActive = false;
    };
    EnvGesture envGesture_;

    /* Active tool + loop-range state.  See the comment block on the
     * Tool enum declaration for the per-tool gesture mapping. */
    Tool   activeTool_     = Tool::Select;
    bool   rangeActive_    = false;
    bool   rangeDragging_  = false;
    double rangeStart_     = 0.0;
    double rangeEnd_       = 0.0;
    double rangeAnchor_    = 0.0;
    bool   loopActive_     = false;

    /* Thumbnail infrastructure.  AudioFormatManager + AudioThumbnailCache
     * are per-Body; thumbnails_ is keyed by AudioFileSource uuid so
     * regions referencing the same source share the same waveform
     * peak data (cheap; one decode per source not per region). */
    struct UuidHashKey {
        juce::Uuid uuid;
        bool operator== (const UuidHashKey& o) const noexcept { return uuid == o.uuid; }
    };
    struct UuidHashKeyHash {
        std::size_t operator() (const UuidHashKey& k) const noexcept
        {
            return std::hash<std::string>{} (k.uuid.toString().toStdString());
        }
    };

    juce::AudioFormatManager   formatManager_;
    juce::AudioThumbnailCache  thumbnailCache_;
    std::unordered_map<UuidHashKey, std::unique_ptr<juce::AudioThumbnail>, UuidHashKeyHash> thumbnails_;
};

/* ===================================================================== */

ArrangementView::ArrangementView()
{
    setName (EL_VIEW_ARRANGEMENT);

    addAndMakeVisible (rescanBtn_);
    addAndMakeVisible (addAudioBtn_);
    addAndMakeVisible (loadAudioBtn_);
    addAndMakeVisible (toolSelectBtn_);
    addAndMakeVisible (toolRangeBtn_);
    addAndMakeVisible (toolSplitBtn_);
    addAndMakeVisible (toolTrimBtn_);
    addAndMakeVisible (toolAuditionBtn_);
    addAndMakeVisible (loopBtn_);
    addAndMakeVisible (snapBtn_);
    addAndMakeVisible (snapBox_);
    addAndMakeVisible (zoomOutBtn_);
    addAndMakeVisible (zoomInBtn_);
    addAndMakeVisible (viewport_);

    /* Horizontal zoom +/- step.  Mirrors the body's mouse-wheel pinch
     * factor (1.20).  Anchor stays at viewport-centre so the visible
     * beats roughly hold across taps.  Shift +/- key shortcuts route
     * to the same zoomBy() entry point on Body. */
    zoomOutBtn_.onClick = [this]() { if (body_) body_->zoomBy (1.0 / 1.20); };
    zoomInBtn_ .onClick = [this]() { if (body_) body_->zoomBy (1.20); };

    /* Snap controls.  snapBtn toggles snap on/off; snapBox picks
     * the snap unit in beats.  Visual highlight = on. */
    snapBtn_.setClickingTogglesState (true);
    snapBtn_.setToggleState (snapEnabled_, juce::dontSendNotification);
    snapBtn_.onClick = [this]()
    {
        snapEnabled_ = snapBtn_.getToggleState();
        snapBox_.setEnabled (snapEnabled_);
    };

    snapBox_.addItem ("1/16",  1);
    snapBox_.addItem ("1/8",   2);
    snapBox_.addItem ("1/4",   3);
    snapBox_.addItem ("Beat",  4);
    snapBox_.addItem ("1/2",   5);
    snapBox_.addItem ("Bar",   6);
    snapBox_.setSelectedId (4, juce::dontSendNotification);   // Beat default
    snapBox_.onChange = [this]()
    {
        switch (snapBox_.getSelectedId())
        {
            case 1: snapDivision_ = 0.25; break;
            case 2: snapDivision_ = 0.5;  break;
            case 3: snapDivision_ = 1.0;  break;
            case 4: snapDivision_ = 1.0;  break;
            case 5: snapDivision_ = 2.0;  break;
            case 6: snapDivision_ = 4.0;  break;
            default: break;
        }
    };

    body_ = std::make_unique<Body> (*this);
    viewport_.setViewedComponent (body_.get(), false);
    viewport_.setScrollBarsShown (true, true);

    rescanBtn_.onClick    = [this]() { rescanLaneTargets(); };
    addAudioBtn_.onClick  = [this]() { createEmptyAudioLane (true /*stereo*/); };
    loadAudioBtn_.onClick = [this]() { promptLoadAudioFile(); };

    /* Tool buttons: radio-group toggles; one tool active at a time.
     * Select is default + sticky on start. */
    toolSelectBtn_   .setClickingTogglesState (true);
    toolRangeBtn_    .setClickingTogglesState (true);
    toolSplitBtn_    .setClickingTogglesState (true);
    toolTrimBtn_     .setClickingTogglesState (true);
    toolAuditionBtn_ .setClickingTogglesState (true);
    toolSelectBtn_.setToggleState (true, juce::dontSendNotification);

    toolSelectBtn_  .onClick = [this]() { if (body_) body_->setActiveTool (Body::Tool::Select);   syncToolToggleStates(); };
    toolRangeBtn_   .onClick = [this]() { if (body_) body_->setActiveTool (Body::Tool::Range);    syncToolToggleStates(); };
    toolSplitBtn_   .onClick = [this]() { if (body_) body_->setActiveTool (Body::Tool::Split);    syncToolToggleStates(); };
    toolTrimBtn_    .onClick = [this]() { if (body_) body_->setActiveTool (Body::Tool::Trim);     syncToolToggleStates(); };
    toolAuditionBtn_.onClick = [this]() { if (body_) body_->setActiveTool (Body::Tool::Audition); syncToolToggleStates(); };

    loopBtn_.setClickingTogglesState (true);
    loopBtn_.setToggleState (false, juce::dontSendNotification);
    loopBtn_.onClick = [this]() { onLoopToggled(); };

    /* Active-state green tint -- mirrors tracker FOLLOW button so a
     * row of tool buttons reads as "this one is currently armed". */
    const juce::Colour kActiveTint { 0xff'4a'a5'5a };
    toolSelectBtn_  .setActiveTint (kActiveTint);
    toolRangeBtn_   .setActiveTint (kActiveTint);
    toolSplitBtn_   .setActiveTint (kActiveTint);
    toolTrimBtn_    .setActiveTint (kActiveTint);
    toolAuditionBtn_.setActiveTint (kActiveTint);
    loopBtn_        .setActiveTint (kActiveTint);
    snapBtn_        .setActiveTint (kActiveTint);

    /* Tool icons (vector paths, foreground-coloured so they pop in
     * both active + idle states).  Drawn in a square chunk on the
     * left of the button; label sits beside. */
    toolSelectBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            /* Mouse-pointer arrow: thin filled triangle pointing
             * upper-left to lower-right.  Standard cursor glyph. */
            juce::Path p;
            const float x0 = b.getX() + b.getWidth() * 0.20f;
            const float y0 = b.getY() + b.getHeight() * 0.15f;
            const float x1 = b.getX() + b.getWidth() * 0.55f;
            const float y1 = b.getY() + b.getHeight() * 0.55f;
            const float x2 = b.getX() + b.getWidth() * 0.35f;
            const float y2 = b.getY() + b.getHeight() * 0.55f;
            const float x3 = b.getX() + b.getWidth() * 0.55f;
            const float y3 = b.getY() + b.getHeight() * 0.85f;
            const float x4 = b.getX() + b.getWidth() * 0.35f;
            const float y4 = b.getY() + b.getHeight() * 0.85f;
            const float x5 = b.getX() + b.getWidth() * 0.20f;
            const float y5 = b.getY() + b.getHeight() * 0.65f;
            p.startNewSubPath (x0, y0);
            p.lineTo (x1, y1);
            p.lineTo (x2, y2);
            p.lineTo (x3, y3);
            p.lineTo (x4, y4);
            p.lineTo (x5, y5);
            p.closeSubPath();
            g.setColour (fg);
            g.fillPath (p);
        });

    toolRangeBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            /* Two vertical bookends + horizontal line spanning between
             * them: classic "range" / "loop bounds" glyph. */
            const float pad = b.getHeight() * 0.20f;
            const float top = b.getY() + pad;
            const float bot = b.getBottom() - pad;
            const float xL  = b.getX() + b.getWidth() * 0.20f;
            const float xR  = b.getRight() - b.getWidth() * 0.20f;
            const float yMid = b.getCentreY();
            g.setColour (fg);
            g.drawLine (xL, top, xL, bot, 1.5f);
            g.drawLine (xR, top, xR, bot, 1.5f);
            g.drawLine (xL, yMid, xR, yMid, 1.0f);
        });

    toolSplitBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            /* Vertical blade with two outward arrowheads at the
             * midline -- "cut here, two pieces". */
            const float xMid = b.getCentreX();
            const float yMid = b.getCentreY();
            const float pad = b.getHeight() * 0.15f;
            g.setColour (fg);
            g.drawLine (xMid, b.getY() + pad, xMid, b.getBottom() - pad, 1.5f);
            const float arrowLen = b.getWidth() * 0.22f;
            const float arrowH   = b.getHeight() * 0.18f;
            juce::Path leftArrow, rightArrow;
            leftArrow.startNewSubPath (xMid - 1.5f, yMid);
            leftArrow.lineTo (xMid - arrowLen, yMid - arrowH);
            leftArrow.lineTo (xMid - arrowLen, yMid + arrowH);
            leftArrow.closeSubPath();
            rightArrow.startNewSubPath (xMid + 1.5f, yMid);
            rightArrow.lineTo (xMid + arrowLen, yMid - arrowH);
            rightArrow.lineTo (xMid + arrowLen, yMid + arrowH);
            rightArrow.closeSubPath();
            g.fillPath (leftArrow);
            g.fillPath (rightArrow);
        });

    toolTrimBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            /* Two brackets pointing inward -- ][ -- "trim from both
             * sides into the middle". */
            const float pad = b.getHeight() * 0.18f;
            const float top = b.getY() + pad;
            const float bot = b.getBottom() - pad;
            const float xL  = b.getX() + b.getWidth() * 0.22f;
            const float xR  = b.getRight() - b.getWidth() * 0.22f;
            const float armLen = b.getWidth() * 0.18f;
            g.setColour (fg);
            g.drawLine (xL, top, xL, bot, 1.5f);
            g.drawLine (xL, top, xL + armLen, top, 1.5f);
            g.drawLine (xL, bot, xL + armLen, bot, 1.5f);
            g.drawLine (xR, top, xR, bot, 1.5f);
            g.drawLine (xR, top, xR - armLen, top, 1.5f);
            g.drawLine (xR, bot, xR - armLen, bot, 1.5f);
        });

    toolAuditionBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            /* Speaker glyph -- trapezoid body + two arc lines for
             * sound waves.  Standard "preview" affordance. */
            const float yMid = b.getCentreY();
            const float bodyL = b.getX() + b.getWidth() * 0.18f;
            const float bodyR = b.getX() + b.getWidth() * 0.42f;
            const float coneR = b.getX() + b.getWidth() * 0.62f;
            const float bodyTop = yMid - b.getHeight() * 0.15f;
            const float bodyBot = yMid + b.getHeight() * 0.15f;
            const float coneTop = yMid - b.getHeight() * 0.30f;
            const float coneBot = yMid + b.getHeight() * 0.30f;
            juce::Path sp;
            sp.startNewSubPath (bodyL, bodyTop);
            sp.lineTo (bodyR, bodyTop);
            sp.lineTo (coneR, coneTop);
            sp.lineTo (coneR, coneBot);
            sp.lineTo (bodyR, bodyBot);
            sp.lineTo (bodyL, bodyBot);
            sp.closeSubPath();
            g.setColour (fg);
            g.fillPath (sp);
            const float waveX = b.getX() + b.getWidth() * 0.72f;
            g.drawLine (waveX, yMid - 3, waveX + b.getWidth() * 0.10f, yMid - 6, 1.2f);
            g.drawLine (waveX, yMid + 3, waveX + b.getWidth() * 0.10f, yMid + 6, 1.2f);
        });

    loopBtn_.setIcon (
        [] (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
        {
            /* Open circular arrow -- loop / wrap glyph. */
            const float cx = b.getCentreX();
            const float cy = b.getCentreY();
            const float rad = juce::jmin (b.getWidth(), b.getHeight()) * 0.32f;
            juce::Path arc;
            arc.addCentredArc (cx, cy, rad, rad, 0.0f,
                                0.25f, 6.0f, true);
            g.setColour (fg);
            g.strokePath (arc, juce::PathStrokeType (1.4f));
            /* Tiny arrowhead at the end of the arc. */
            const float ax = cx + rad * 0.7f;
            const float ay = cy - rad * 0.7f;
            juce::Path tip;
            tip.startNewSubPath (ax, ay);
            tip.lineTo (ax - rad * 0.45f, ay - rad * 0.15f);
            tip.lineTo (ax - rad * 0.10f, ay - rad * 0.55f);
            tip.closeSubPath();
            g.fillPath (tip);
        });
}

ArrangementView::~ArrangementView()
{
    stopTimer();
    detachFromActiveGraph();

    /* Drop record-committed handlers so AudioClipNodes don't try to
     * invoke us during teardown.  Bound lambdas captured `this`; the
     * AudioClipNode instances outlive the ArrangementView while the
     * graph is being torn down. */
    for (auto& rs : laneRuntime_)
        if (rs.audioClipCache != nullptr)
            rs.audioClipCache->setRecordingCommittedHandler (nullptr);
}

void ArrangementView::initializeView (Services& s)
{
    services_ = &s;
    if (auto* eng = s.context().audio().get())
        monitor_ = eng->getTransportMonitor();
}

void ArrangementView::didBecomeActive()
{
    attachToActiveGraph();
    rescanLaneTargets();

    /* Zoom restored synchronously BEFORE the first paint so the user
     * never sees the default kPxPerBeat/kLaneH flash through.
     * Previously the entire view-state was deferred via callAsync
     * which left one paint frame of unstyled content visible. */
    loadZoomFromSession();

    startTimerHz (30);

    /* Scroll restore deferred to the next message-thread tick: the
     * parent setContentView path has not yet invoked our resized(),
     * so viewport_ has zero dimensions at this point and an immediate
     * setViewPosition would clamp to (0, 0).  callAsync defers past
     * resized() so loadViewStateFromSession (scroll only) applies
     * against the real viewport area. */
    juce::Component::SafePointer<ArrangementView> self (this);
    juce::MessageManager::callAsync ([self]()
    {
        if (auto* v = self.getComponent())
            v->loadViewStateFromSession();
    });
}

void ArrangementView::willBeRemoved()
{
    writeViewStateToSession();
    cancelPendingUpdate();
    stopTimer();
    detachFromActiveGraph();
}

void ArrangementView::stabilizeContent()
{
    /* Coalesce multiple stabilize requests fired in one event-loop
     * tick into one rescan on the next tick.  Triggers include
     * post-rescan callbacks + ValueTree-listener cascades + outer
     * GuiService stabilizeContent fan-outs.  handleAsyncUpdate does
     * the actual attach + rescan once. */
    triggerAsyncUpdate();
}

void ArrangementView::handleAsyncUpdate()
{
    attachToActiveGraph();
    rescanLaneTargets();
}

void ArrangementView::attachToActiveGraph()
{
    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    auto graph = sess->getActiveGraph();
    if (! graph.isValid()) return;
    auto tree = graph.data();
    if (tree == attachedGraphTree_) return;

    detachFromActiveGraph();
    attachedGraphTree_ = tree;
    if (attachedGraphTree_.isValid())
        attachedGraphTree_.addListener (this);
}

void ArrangementView::detachFromActiveGraph()
{
    if (attachedGraphTree_.isValid())
        attachedGraphTree_.removeListener (this);
    attachedGraphTree_ = juce::ValueTree();
}

void ArrangementView::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& child)
{
    /* Graph mutations may fire a burst of child-added events (e.g.
     * adding a subgraph with several nodes drops one event per
     * node).  Route to triggerAsyncUpdate so a burst collapses to a
     * single rescan on the next tick. */
    if (child.hasType (types::Node) || child.hasType (tags::nodes))
        triggerAsyncUpdate();
}

void ArrangementView::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    if (child.hasType (types::Node) || child.hasType (tags::nodes))
        triggerAsyncUpdate();
}

void ArrangementView::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (kHeaderH).reduced (4, 4);
    rescanBtn_      .setBounds (top.removeFromLeft (60)); top.removeFromLeft (4);
    addAudioBtn_    .setBounds (top.removeFromLeft (70)); top.removeFromLeft (4);
    loadAudioBtn_   .setBounds (top.removeFromLeft (60)); top.removeFromLeft (12);

    toolSelectBtn_  .setBounds (top.removeFromLeft (76)); top.removeFromLeft (2);
    toolRangeBtn_   .setBounds (top.removeFromLeft (72)); top.removeFromLeft (2);
    toolSplitBtn_   .setBounds (top.removeFromLeft (64)); top.removeFromLeft (2);
    toolTrimBtn_    .setBounds (top.removeFromLeft (64)); top.removeFromLeft (2);
    toolAuditionBtn_.setBounds (top.removeFromLeft (72)); top.removeFromLeft (12);

    loopBtn_        .setBounds (top.removeFromLeft (64)); top.removeFromLeft (12);

    snapBtn_        .setBounds (top.removeFromLeft (52)); top.removeFromLeft (4);
    snapBox_        .setBounds (top.removeFromLeft (64)); top.removeFromLeft (12);

    /* Horizontal zoom step buttons, narrow square footprint.  Shift
     * +/- on the keyboard does the same action. */
    zoomOutBtn_     .setBounds (top.removeFromLeft (28)); top.removeFromLeft (2);
    zoomInBtn_      .setBounds (top.removeFromLeft (28));

    viewport_.setBounds (r);
    if (body_ != nullptr) body_->resizeForLanes();
}

/* =====================================================================
 * Outer FileDragAndDropTarget on the ContentView itself.  Belt-and-
 * suspenders alongside Body's same interface (Body lives inside a
 * Viewport which can rarely swallow X11 XDND routing in some peer
 * configurations).
 * ===================================================================== */

bool ArrangementView::isInterestedInFileDrag (const juce::StringArray& files)
{
    const bool interested = anyAudioFileIn (files);
    juce::Logger::writeToLog (
        juce::String ("[ArrangementView] isInterestedInFileDrag count=")
        + juce::String (files.size())
        + (interested ? " ACCEPT" : " REJECT"));
    return interested;
}

void ArrangementView::fileDragEnter (const juce::StringArray& files, int x, int y)
{
    juce::Logger::writeToLog (
        juce::String ("[ArrangementView] fileDragEnter x=") + juce::String (x)
        + " y=" + juce::String (y)
        + " count=" + juce::String (files.size()));
}

void ArrangementView::fileDragExit (const juce::StringArray&)
{
    juce::Logger::writeToLog ("[ArrangementView] fileDragExit");
}

void ArrangementView::filesDropped (const juce::StringArray& files, int x, int y)
{
    juce::Logger::writeToLog (
        juce::String ("[ArrangementView] filesDropped x=") + juce::String (x)
        + " y=" + juce::String (y)
        + " count=" + juce::String (files.size()));

    /* Translate ArrangementView-local coords to Body-local coords:
     * subtract header height + add viewport scroll offset.  body_'s
     * (0,0) is at the top of the lanes area. */
    if (body_ == nullptr) return;

    const int bodyY = (y - kHeaderH) + viewport_.getViewPositionY();
    const int bodyX =  x              + viewport_.getViewPositionX();

    const int laneIdx = laneIdxFromY (bodyY);
    const double dropBeats =
        juce::jmax (0.0, (double) (bodyX - kLabelW) / (double) body_->kPxPerBeat);

    int targetLane = laneIdx;
    if (targetLane >= 0 && targetLane < lanes_.size())
    {
        const auto& runtime = laneRuntime_.getReference (targetLane);
        if (! runtime.isAudioLane())
            targetLane = -1;   // tracker lane -> create new audio
    }
    else
    {
        targetLane = -1;       // empty area -> create new audio
    }

    double cursor = dropBeats;
    for (const auto& path : files)
    {
        const juce::File f (path);
        if (! f.existsAsFile())
        {
            juce::Logger::writeToLog (
                juce::String ("[ArrangementView] skipping non-existent file: ") + path);
            continue;
        }

        const bool ok = importAudioFileToLane (f, targetLane, cursor);
        juce::Logger::writeToLog (
            juce::String ("[ArrangementView] importAudioFileToLane(")
            + f.getFileName() + ", " + juce::String (targetLane) + ", "
            + juce::String (cursor, 2) + ") = " + (ok ? "OK" : "FAIL"));
        if (! ok) continue;

        if (targetLane < 0)
            targetLane = lanes_.size() - 1;

        if (targetLane >= 0 && targetLane < lanes_.size())
        {
            const auto& regs = lanes_.getReference (targetLane).playlist.regions();
            if (! regs.empty())
                cursor = regs.back().endBeats();
        }
    }
}

void ArrangementView::paint (Graphics& g)
{
    g.fillAll (Colors::contentBackgroundColor);
    g.setColour (Colors::backgroundColor);
    g.fillRect (getLocalBounds().removeFromTop (kHeaderH));
}

/* ===================================================================== */

namespace {

/** Recursive: collects every TrackerNode AND AudioClipNode reachable
 *  from `graph`, including subgraphs.  Used to seed / rebind lanes
 *  against the live graph. */
void collectLaneTargetsFromGraph (const Node& graph,
                                  juce::Array<Node>&           outNodes,
                                  juce::Array<TrackerNode*>&   outTrackers,
                                  juce::Array<AudioClipNode*>& outAudioClips)
{
    const int n = graph.getNumNodes();
    for (int i = 0; i < n; ++i)
    {
        Node child = graph.getNode (i);
        if (! child.isValid()) continue;
        if (auto* proc = child.getObject())
        {
            /* TrackerNode IS-A element::Processor directly so the
             * cast is on `proc` itself. */
            if (auto* t = dynamic_cast<TrackerNode*> (proc))
            {
                outNodes.add (child);
                outTrackers.add (t);
                outAudioClips.add (nullptr);
                continue;
            }
            /* AudioClipNode is a juce::AudioPluginInstance wrapped by
             * element::Processor -- Node::getObject() returns the
             * wrapper, so we have to go through proc->getAudioProcessor()
             * to reach the underlying AudioClipNode.  Mirrors
             * resolveAudioClipByUuid's unwrap; before this helper
             * never populated outAudioClips and audio lanes were
             * silently sent to the orphan path. */
            if (auto* ap = proc->getAudioProcessor())
            {
                if (auto* a = dynamic_cast<AudioClipNode*> (ap))
                {
                    outNodes.add (child);
                    outTrackers.add (nullptr);
                    outAudioClips.add (a);
                    continue;
                }
            }
        }
        if (child.isGraph())
            collectLaneTargetsFromGraph (child, outNodes, outTrackers, outAudioClips);
    }
}

Node findNodeByUuid (const Node& graph, juce::Uuid target)
{
    const int n = graph.getNumNodes();
    for (int i = 0; i < n; ++i)
    {
        Node child = graph.getNode (i);
        if (! child.isValid()) continue;
        if (child.getUuid() == target) return child;
        if (child.isGraph())
        {
            Node nested = findNodeByUuid (child, target);
            if (nested.isValid()) return nested;
        }
    }
    return Node();
}

} // namespace

TrackerNode* ArrangementView::resolveTrackerByUuid (juce::Uuid targetNodeUuid) const
{
    if (services_ == nullptr) return nullptr;
    auto sess = services_->context().session();
    if (sess == nullptr) return nullptr;
    const Node active = sess->getActiveGraph();
    if (! active.isValid()) return nullptr;

    const Node target = findNodeByUuid (active, targetNodeUuid);
    if (! target.isValid()) return nullptr;
    return dynamic_cast<TrackerNode*> (target.getObject());
}

AudioClipNode* ArrangementView::resolveAudioClipByUuid (juce::Uuid targetNodeUuid) const
{
    if (services_ == nullptr) return nullptr;
    auto sess = services_->context().session();
    if (sess == nullptr) return nullptr;
    const Node active = sess->getActiveGraph();
    if (! active.isValid()) return nullptr;

    const Node target = findNodeByUuid (active, targetNodeUuid);
    if (! target.isValid()) return nullptr;

    /* Two-step unwrap: AudioClipNode is a juce::AudioPluginInstance,
     * so Element wraps it in an AudioProcessorNode (element::Processor).
     * Node::getObject() returns the wrapper -- we have to go through
     * its getAudioProcessor() to reach the underlying AudioClipNode.
     * (TrackerNode IS an element::Processor directly, so the
     * resolveTrackerByUuid path doesn't need this dance.) */
    auto* proc = target.getObject();
    if (proc == nullptr) return nullptr;
    return dynamic_cast<AudioClipNode*> (proc->getAudioProcessor());
}

void ArrangementView::autoFillLaneForTracker (Lane& lane, TrackerNode* trk)
{
    if (trk == nullptr) return;
    const int numPatterns = trk->numPatterns();
    double cursor = 0.0;
    for (int p = 0; p < numPatterns; ++p)
    {
        Region r;
        r.id            = juce::Uuid();
        r.sourceId      = lane.targetNodeUuid;
        r.sequenceIdx   = p;
        r.positionBeats = cursor;
        r.lengthBeats   = 4.0;
        r.colour        = juce::Colour::fromRGB (90, 130, 170);
        lane.playlist.addRegion (std::move (r));
        cursor += 4.0;
    }
}

void ArrangementView::rescanLaneTargets()
{
    if (! lanesLoadedFromSession_)
    {
        loadLanesFromSession();
        lanesLoadedFromSession_ = true;
    }

    juce::Array<Node>            foundNodes;
    juce::Array<TrackerNode*>    foundTrackers;
    juce::Array<AudioClipNode*>  foundAudioClips;

    if (services_ != nullptr)
    {
        if (auto sess = services_->context().session())
        {
            const Node active = sess->getActiveGraph();
            if (active.isValid())
                collectLaneTargetsFromGraph (active, foundNodes,
                                              foundTrackers, foundAudioClips);
        }
    }

    /* Auto-fill: tracker nodes get a default lane on first discovery.
     * AudioClipNodes do NOT auto-fill -- they're created explicitly
     * via "+ Audio Track" or file drop, both of which create the lane
     * inline. */
    bool mutated = false;
    for (int i = 0; i < foundNodes.size(); ++i)
    {
        if (foundTrackers[i] == nullptr) continue;   // skip audio clips here
        const juce::Uuid uuid = foundNodes.getReference (i).getUuid();
        bool alreadyBound = false;
        for (const auto& l : lanes_)
            if (l.targetNodeUuid == uuid) { alreadyBound = true; break; }
        if (alreadyBound) continue;

        Lane lane;
        lane.id             = juce::Uuid();
        lane.targetNodeUuid = uuid;
        lane.name           = foundNodes.getReference (i).getName().isNotEmpty()
                                ? foundNodes.getReference (i).getName()
                                : juce::String ("Tracker");
        autoFillLaneForTracker (lane, foundTrackers[i]);
        lanes_.add (std::move (lane));
        mutated = true;
    }

    /* Assign palette tint to every lane by its index.  Lanes
     * created before the palette landed default to dark-gray; this
     * line replaces those with the shared tracker palette.  Future
     * "lane colour picker" UI will introduce a separate override
     * flag so user-customised colours aren't reset on rescan. */
    for (int i = 0; i < lanes_.size(); ++i)
        lanes_.getReference (i).colour = laneTintForIndex (i);

    /* Rebuild runtime state in lockstep.  For each persisted lane,
     * resolve which kind of node it binds to + wire up the
     * AudioLaneAdapter / record commit handler if applicable.
     *
     * Optimisation: replace the per-lane resolveTrackerByUuid +
     * resolveAudioClipByUuid calls (each a recursive findNodeByUuid
     * graph walk = O(M) for M=graph node count) with a linear scan
     * over the foundNodes array that collectLaneTargetsFromGraph
     * already populated.  Lookup cost drops from O(N*M) to O(N*F)
     * where F=N tracker+audio nodes (typically <50), eliminating
     * most of the graph -> timeline switch latency on dense sessions.
     * collectLaneTargetsFromGraph now properly unwraps
     * proc->getAudioProcessor() for AudioClipNodes so foundAudioClips
     * is correctly populated.
     *
     * Per-lane Logger::writeToLog (sync disk IO on the message
     * thread) is also gone -- the other dominant cost in this loop. */
    laneRuntime_.clearQuick();
    laneRuntime_.ensureStorageAllocated (lanes_.size());
    for (int i = 0; i < lanes_.size(); ++i)
    {
        const auto& l = lanes_.getReference (i);
        LaneRuntimeState s;

        for (int j = 0; j < foundNodes.size(); ++j)
        {
            if (foundNodes.getReference (j).getUuid() == l.targetNodeUuid)
            {
                s.trackerCache   = foundTrackers  [j];
                s.audioClipCache = foundAudioClips[j];
                break;
            }
        }

        if (s.audioClipCache != nullptr)
        {
            s.audioAdapter.setTargetNode (s.audioClipCache);

            /* Propagate persisted Lane.armed onto the freshly-bound
             * node so re-opening a session restores arm state. */
            s.audioClipCache->setArmed (l.armed);

            /* Capture lane id by value (survives lane-index shifts)
             * and the ArrangementView via Component::SafePointer so
             * the lambda is safe to invoke after this view has been
             * destroyed (graph-teardown race: AudioClipNode's Timer
             * may fire after the view is already gone). */
            const juce::Uuid laneId = l.id;
            juce::Component::SafePointer<ArrangementView> safe (this);
            s.audioClipCache->setRecordingCommittedHandler (
                [safe, laneId] (const juce::File& file)
                {
                    auto* self = safe.getComponent();
                    if (self == nullptr) return;

                    /* Resolve lane by id (linear scan).  If user
                     * deleted the lane mid-capture, drop the file
                     * silently -- the .wav stays on disk for
                     * recovery but no Region is created. */
                    int idx = -1;
                    for (int k = 0; k < self->lanes_.size(); ++k)
                        if (self->lanes_.getReference (k).id == laneId) { idx = k; break; }
                    if (idx < 0) return;

                    auto src = SourceRegistry::get().importFromFile (file);
                    if (src == nullptr) return;

                    auto& lane = self->lanes_.getReference (idx);

                    /* Position the new Region at recordStartBeat_ --
                     * where the transport playhead was when record
                     * started -- so the audio lands at the spot the
                     * user actually triggered, not appended after
                     * the last existing region.  If the target slot
                     * overlaps an existing region, nudge forward to
                     * the next free spot (defensive). */
                    double position = self->recordStartBeat_;
                    for (const auto& r : lane.playlist.regions())
                        if (r.containsBeat (position))
                            position = juce::jmax (position, r.endBeats());

                    const double bpm = self->monitor_ != nullptr
                                          ? (double) self->monitor_->tempo.get()
                                          : 120.0;
                    const double lengthSeconds = src->sourceSampleRate() > 0
                        ? (double) src->durationSamples() / (double) src->sourceSampleRate()
                        : 0.0;
                    const double lengthBeats = juce::jmax (
                        0.25,
                        lengthSeconds * (bpm / 60.0));

                    Region r;
                    r.id            = juce::Uuid();
                    r.sourceId      = src->uuid();
                    r.positionBeats = position;
                    r.lengthBeats   = lengthBeats;
                    r.name          = file.getFileNameWithoutExtension();
                    r.colour        = juce::Colour::fromRGB (90, 170, 130);
                    lane.playlist.addRegion (std::move (r));

                    self->writeLanesToSession();
                    if (self->body_ != nullptr)
                    {
                        self->body_->resizeForLanes();
                        self->body_->repaintLane (idx);
                    }
                });
        }
        else if (s.trackerCache == nullptr)
        {
            /* Orphan lane: defensively detach any previous adapter
             * binding so dispatch silently skips. */
            s.audioAdapter.setTargetNode (nullptr);
        }

        laneRuntime_.add (std::move (s));
    }

    if (mutated) writeLanesToSession();
    if (body_ != nullptr) body_->resizeForLanes();

    /* After any rescan, re-apply persisted mute / solo state to the
     * freshly-bound target processors (a graph rebind clears the
     * underlying setMuted state). */
    propagateMuteSolo();
}

void ArrangementView::loadLanesFromSession()
{
    lanes_.clearQuick();
    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;

    auto tree = sess->data().getChildWithName (tags::arrangement);
    if (! tree.isValid()) return;
    auto lanesTree = tree.getChildWithName ("lanes");
    if (! lanesTree.isValid()) return;

    for (int i = 0; i < lanesTree.getNumChildren(); ++i)
    {
        const auto laneTree = lanesTree.getChild (i);
        if (laneTree.getType() != juce::Identifier ("lane")) continue;
        lanes_.add (Lane::fromValueTree (laneTree));
    }
}

void ArrangementView::writeLanesToSession()
{
    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;

    auto tree = sess->data().getOrCreateChildWithName (tags::arrangement, nullptr);
    auto lanesTree = tree.getOrCreateChildWithName ("lanes", nullptr);
    lanesTree.removeAllChildren (nullptr);
    for (const auto& l : lanes_)
        lanesTree.appendChild (l.toValueTree(), nullptr);
}

void ArrangementView::loadZoomFromSession()
{
    if (services_ == nullptr || body_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    auto tree = sess->data().getChildWithName (tags::arrangement);
    if (! tree.isValid()) return;
    auto vs = tree.getChildWithName ("viewState");
    if (! vs.isValid()) return;

    /* Horizontal + vertical zoom are clamped to Body's defined limits
     * so a session edited under a different build can't park kPxPerBeat
     * outside [4, 256] or kLaneH outside [40, 240]. */
    body_->kPxPerBeat = juce::jlimit (Body::kPxPerBeatMin, Body::kPxPerBeatMax,
        (int) vs.getProperty ("pxPerBeat", body_->kPxPerBeat));
    body_->kLaneH = juce::jlimit (Body::kLaneHMin, Body::kLaneHMax,
        (int) vs.getProperty ("laneH", body_->kLaneH));
    body_->resizeForLanes();
}

void ArrangementView::loadViewStateFromSession()
{
    /* Scroll only -- zoom is applied synchronously by
     * loadZoomFromSession from didBecomeActive so the first paint
     * already has the saved kPxPerBeat / kLaneH.  This deferred path
     * waits for the parent's resized() to size viewport_, then
     * restores the saved scroll offset. */
    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    auto tree = sess->data().getChildWithName (tags::arrangement);
    if (! tree.isValid()) return;
    auto vs = tree.getChildWithName ("viewState");
    if (! vs.isValid()) return;

    const int scrollX = (int) vs.getProperty ("scrollX", 0);
    const int scrollY = (int) vs.getProperty ("scrollY", 0);
    viewport_.setViewPosition (scrollX, scrollY);
}

void ArrangementView::writeViewStateToSession()
{
    if (services_ == nullptr || body_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    auto tree = sess->data().getOrCreateChildWithName (tags::arrangement, nullptr);
    auto vs   = tree.getOrCreateChildWithName ("viewState", nullptr);
    vs.setProperty ("pxPerBeat", body_->kPxPerBeat,           nullptr);
    vs.setProperty ("laneH",     body_->kLaneH,               nullptr);
    vs.setProperty ("scrollX",   viewport_.getViewPositionX(), nullptr);
    vs.setProperty ("scrollY",   viewport_.getViewPositionY(), nullptr);
}

double ArrangementView::computePlayheadBeats() const
{
    if (monitor_ == nullptr) return 0.0;
    return (double) monitor_->getPositionBeats();
}

void ArrangementView::dispatchAtBeat (double beat)
{
    const double bpm = monitor_ != nullptr ? (double) monitor_->tempo.get() : 120.0;

    for (int laneIdx = 0; laneIdx < lanes_.size(); ++laneIdx)
    {
        const auto& lane    = lanes_.getReference (laneIdx);
        auto&       runtime = laneRuntime_.getReference (laneIdx);
        if (runtime.isOrphan()) continue;

        const Region* active = lane.playlist.regionAt (beat);

        /* Case 1: playhead is outside any region.  Stop whatever was
         * playing on this lane.  Tracker lanes don't auto-stop on
         * gaps -- vht state persists between regions and the next
         * region launch re-fires; audio lanes DO stop (DAW
         * convention: gaps = silence). */
        if (active == nullptr)
        {
            if (! runtime.lastDispatchedRegion.isNull())
            {
                if (runtime.isAudioLane())
                    runtime.audioClipCache->scheduleStop (
                        runtime.lastDispatchedRegion, -1.0);
                runtime.lastDispatchedRegion = juce::Uuid::null();
                runtime.lastDispatchedSeqIdx = -1;
                /* No repaintLane: per-region visual no longer differs
                 * by active state, so the dispatch transition is
                 * model-only. */
            }
            continue;
        }

        /* Case 2: still inside the same region the lane is already
         * playing.  No state change. */
        if (active->id == runtime.lastDispatchedRegion)
            continue;

        /* Case 3: entering a new region (either first region under
         * the playhead OR transitioning from a different region).
         * Stop the prior + launch the new. */
        if (! runtime.lastDispatchedRegion.isNull())
        {
            if (runtime.isAudioLane())
                runtime.audioClipCache->scheduleStop (
                    runtime.lastDispatchedRegion, -1.0);
            /* Tracker lanes: existing pattern stays "playing" in
             * vht state; the new schedulePlaying below explicitly
             * stops the old sequence + starts the new. */
        }

        if (runtime.isTrackerLane())
        {
            if (active->sequenceIdx >= 0)
            {
                if (runtime.lastDispatchedSeqIdx >= 0
                    && runtime.lastDispatchedSeqIdx != active->sequenceIdx)
                    runtime.trackerCache->schedulePlaying (
                        runtime.lastDispatchedSeqIdx, -1.0, false);
                runtime.trackerCache->schedulePlaying (
                    active->sequenceIdx, -1.0, true);
            }
        }
        else if (runtime.isAudioLane())
        {
            /* Mid-region launch: compute the source-sample offset so
             * the playback head matches where the transport is
             * within the region.  bumping into a region from beat 0
             * fires the source from its beginning (modulo startBeats
             * trim); bumping in at beat 2 of a 4-beat region whose
             * startBeats=0 fires from 2 beats into the source. */
            const double beatIntoRegion = juce::jmax (
                0.0, beat - active->positionBeats);
            const double sourceBeat = active->startBeats + beatIntoRegion;

            juce::int64 sampleOffset = 0;
            if (auto src = SourceRegistry::get().findAudioFile (active->sourceId))
            {
                if (src->sourceSampleRate() > 0 && bpm > 0.0)
                {
                    const double sourceSec = sourceBeat * (60.0 / bpm);
                    sampleOffset = (juce::int64) (sourceSec
                        * (double) src->sourceSampleRate());
                }
            }

            /* Convert region beat-domain envelope params to source
             * samples for the audio-thread envelope code. */
            const double sessionSampleRate = monitor_ != nullptr
                ? (double) monitor_->sampleRate.get()
                : 48000.0;
            const double secsPerBeat = bpm > 0.0 ? 60.0 / bpm : 0.5;
            const juce::int64 fadeInSamples
                = (juce::int64) (active->fadeInBeats  * secsPerBeat * sessionSampleRate);
            const juce::int64 fadeOutSamples
                = (juce::int64) (active->fadeOutBeats * secsPerBeat * sessionSampleRate);
            const juce::int64 regionLenSamples
                = (juce::int64) (active->lengthBeats  * secsPerBeat * sessionSampleRate);

            /* beatTarget = the region's positionBeats so AudioClipNode's
             * audio-thread applyPendingForBlock fires at the block
             * whose range contains it (or catches up if we already
             * passed it).  Sample-accurate to +/- one block ~ 5-10 ms,
             * vs. ~33 ms latency on the prior immediate-only path. */
            runtime.audioClipCache->schedulePlay (
                active->id, active->sourceId,
                active->positionBeats,
                sampleOffset,
                active->looped,
                active->gainDb,
                fadeInSamples, fadeOutSamples, regionLenSamples);
        }

        runtime.lastDispatchedRegion = active->id;
        runtime.lastDispatchedSeqIdx = active->sequenceIdx;
        /* No repaintLane on dispatch transition -- per-region visual
         * is invariant to playhead-over-region state now. */
    }
}

void ArrangementView::stopAllAudioLanes()
{
    for (auto& rs : laneRuntime_)
        if (rs.isAudioLane())
            rs.audioClipCache->scheduleStop (juce::Uuid::null(), -1.0);
}

void ArrangementView::propagateMuteSolo()
{
    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    const Node active = sess->getActiveGraph();
    if (! active.isValid()) return;

    /* Any-soloed gate: in Bitwig / Ableton / Ardour, the moment one
     * lane is soloed, every non-soloed lane is effectively muted. */
    bool anySoloed = false;
    for (const auto& l : lanes_)
        if (l.soloed) { anySoloed = true; break; }

    for (int i = 0; i < lanes_.size(); ++i)
    {
        const auto& l = lanes_.getReference (i);
        const bool effMuted = l.muted || (anySoloed && ! l.soloed);

        const Node target = findNodeByUuid (active, l.targetNodeUuid);
        if (! target.isValid()) continue;
        if (auto* proc = target.getObject())
            proc->setMuted (effMuted);

        /* TrackerNode also has its own session-view mute / solo state
         * so the SessionView grid + TrackerEditor stay in sync.
         * Audio lanes go through Processor::setMuted only -- the
         * AudioProcessorNode wrapper's setMuted gates the audio
         * graph layer, which is enough. */
        const auto& rs = laneRuntime_.getReference (i);
        if (rs.isTrackerLane())
        {
            rs.trackerCache->setUserMuted (l.muted);
            rs.trackerCache->setSoloed (l.soloed);
        }
    }
}

int ArrangementView::createEmptyAudioLane (bool stereo)
{
    if (services_ == nullptr)
    {
        juce::Logger::writeToLog ("[ArrangementView::createEmptyAudioLane] services_ null");
        return -1;
    }
    auto sess = services_->context().session();
    if (sess == nullptr)
    {
        juce::Logger::writeToLog ("[ArrangementView::createEmptyAudioLane] session null");
        return -1;
    }
    auto* engineService = services_->find<EngineService>();
    if (engineService == nullptr)
    {
        juce::Logger::writeToLog ("[ArrangementView::createEmptyAudioLane] EngineService null");
        return -1;
    }

    Node subgraph = ArrangementTracksService::findOrCreateSubgraph (*engineService, *sess);
    juce::Logger::writeToLog (
        juce::String ("[ArrangementView::createEmptyAudioLane] subgraph valid=")
        + (subgraph.isValid() ? "yes" : "no")
        + " isGraph=" + (subgraph.isValid() && subgraph.isGraph() ? "yes" : "no")
        + " uuid=" + (subgraph.isValid() ? subgraph.getUuid().toString() : juce::String ("(none)"))
        + " numChildren=" + juce::String (subgraph.isValid() ? subgraph.getNumNodes() : 0));
    if (! subgraph.isValid()) return -1;

    Node clip = ArrangementTracksService::addAudioClipNode (*engineService, subgraph, stereo);
    juce::Logger::writeToLog (
        juce::String ("[ArrangementView::createEmptyAudioLane] clip valid=")
        + (clip.isValid() ? "yes" : "no")
        + " uuid=" + (clip.isValid() ? clip.getUuid().toString() : juce::String ("(none)"))
        + " name=" + (clip.isValid() ? clip.getName() : juce::String ("(none)"))
        + " parentIsGraph=" + (clip.isValid() ? clip.getParentGraph().getUuid().toString() : juce::String ("(none)")));
    if (! clip.isValid()) return -1;

    Lane lane;
    lane.id             = juce::Uuid();
    lane.targetNodeUuid = clip.getUuid();
    lane.name           = juce::String ("Audio ") + juce::String (lanes_.size() + 1);
    lane.colour         = laneTintForIndex (lanes_.size());
    lanes_.add (std::move (lane));

    rescanLaneTargets();   // resolves the new lane's audioClipCache
    writeLanesToSession();
    if (body_ != nullptr) body_->resizeForLanes();
    return lanes_.size() - 1;
}

bool ArrangementView::importAudioFileToLane (const juce::File& file,
                                             int               laneIdx,
                                             double            positionBeats)
{
    if (! file.existsAsFile())
        return false;

    auto src = SourceRegistry::get().importFromFile (file);
    if (src == nullptr) return false;

    /* Create lane if requested.  -1 = always create new. */
    if (laneIdx < 0 || laneIdx >= lanes_.size())
    {
        const int newIdx = createEmptyAudioLane (true /*stereo*/);
        if (newIdx < 0)
            return false;
        laneIdx = newIdx;
    }
    else
    {
        const auto& runtime = laneRuntime_.getReference (laneIdx);
        if (! runtime.isAudioLane())
            return false;
    }

    auto& lane = lanes_.getReference (laneIdx);

    /* Compute lengthBeats from file duration + current session bpm. */
    const double bpm = monitor_ != nullptr
                          ? (double) monitor_->tempo.get()
                          : 120.0;
    const double lengthSeconds = src->sourceSampleRate() > 0
        ? (double) src->durationSamples() / (double) src->sourceSampleRate()
        : 0.0;
    const double lengthBeats = juce::jmax (
        0.25,
        lengthSeconds * (bpm / 60.0));

    Region r;
    r.id            = juce::Uuid();
    r.sourceId      = src->uuid();
    r.positionBeats = juce::jmax (0.0, positionBeats);
    r.lengthBeats   = lengthBeats;
    r.name          = file.getFileNameWithoutExtension();
    r.colour        = juce::Colour::fromRGB (90, 170, 130);

    if (! lane.playlist.addRegion (Region (r)))
    {
        /* Overlap rejected; nudge to next free spot beyond the last
         * region's end. */
        double cursor = 0.0;
        for (const auto& existing : lane.playlist.regions())
            cursor = juce::jmax (cursor, existing.endBeats());
        r.positionBeats = cursor;
        if (! lane.playlist.addRegion (Region (r)))
            return false;   // shouldn't happen; defensive
    }

    /* Fire immediately so the user gets audible confirmation.
     * Subsequent transport playback will re-dispatch at the region
     * boundary. */
    auto& runtime = laneRuntime_.getReference (laneIdx);
    if (runtime.isAudioLane())
    {
        const double sessionSR = monitor_ != nullptr
            ? (double) monitor_->sampleRate.get() : 48000.0;
        const double secsPerBeat = bpm > 0.0 ? 60.0 / bpm : 0.5;
        runtime.audioClipCache->schedulePlay (
            r.id, r.sourceId, -1.0, 0, r.looped, r.gainDb,
            (juce::int64) (r.fadeInBeats  * secsPerBeat * sessionSR),
            (juce::int64) (r.fadeOutBeats * secsPerBeat * sessionSR),
            (juce::int64) (r.lengthBeats  * secsPerBeat * sessionSR));
    }

    writeLanesToSession();
    if (body_ != nullptr)
    {
        body_->resizeForLanes();
        body_->repaintLane (laneIdx);
    }
    return true;
}

void ArrangementView::promptLoadAudioFile()
{
    /* The DiskOp Request pattern navigates to the Disk Op page via
     * setMainView, which DESTROYS the previous ContentView (i.e.
     * THIS ArrangementView; see standard.cpp:181 primary.reset).
     * So the onAccept callback fires AFTER this view is gone --
     * we cannot capture `this` or even SafePointer<ArrangementView>
     * and expect to do work on it.
     *
     * Capture Services* instead (lifetime-stable; lives in Element's
     * Context) and route through the view-independent helper
     * ArrangementTracksService::importAudioFileAsNewLane.  That
     * helper writes the new Lane + Region directly into the
     * session's tags::arrangement/lanes ValueTree; the next time
     * ArrangementView opens (or didBecomeActive is called), its
     * loadLanesFromSession picks the lane up. */
    if (services_ == nullptr) return;
    auto* gui = services_->find<GuiService>();
    if (gui == nullptr) return;

    Services* svc = services_;
    const juce::String wildcard ("*.wav;*.aiff;*.aif;*.flac;*.ogg;*.mp3;*.w64;*.au");
    const juce::File start = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    juce::Logger::writeToLog ("[ArrangementView::promptLoadAudioFile] arming Disk Op Request");

    gui->requestFile (
        "Load audio file into arrangement",
        wildcard,
        start,
        juce::String() /*initialFilename*/,
        false /*isSave*/,
        [svc] (const juce::File& file)
        {
            juce::Logger::writeToLog (
                juce::String ("[ArrangementView::promptLoadAudioFile] callback fired: ")
                + file.getFullPathName());

            if (svc == nullptr) return;
            if (! file.existsAsFile())
            {
                juce::Logger::writeToLog (" -> chosen file does not exist; abort");
                return;
            }
            ArrangementTracksService::importAudioFileAsNewLane (file, *svc);
        },
        nullptr /*onCancel*/);
}

int ArrangementView::laneIdxFromY (int yPx) const noexcept
{
    /* Body coordinate; account for the top ruler row.  Negative or
     * inside-ruler y returns -1 (no lane at that y).  Lane height
     * is the Body's zoomable kLaneH. */
    if (yPx < Body::kRulerH) return -1;
    if (body_ == nullptr) return -1;
    const int idx = (yPx - Body::kRulerH) / body_->kLaneH;
    if (idx < 0 || idx >= lanes_.size()) return -1;
    return idx;
}

void ArrangementView::updateTransportLabel()
{
    /* BPM + Beat read-outs moved to the global transport bar (above
     * the content view), so this is a no-op now.  Kept as a stub so
     * timerCallback's call site stays valid without conditional
     * checks; remove once a follow-up audits timerCallback. */
}

void ArrangementView::syncToolToggleStates()
{
    if (body_ == nullptr) return;
    const auto tool = body_->getActiveTool();
    toolSelectBtn_  .setToggleState (tool == Body::Tool::Select,   juce::dontSendNotification);
    toolRangeBtn_   .setToggleState (tool == Body::Tool::Range,    juce::dontSendNotification);
    toolSplitBtn_   .setToggleState (tool == Body::Tool::Split,    juce::dontSendNotification);
    toolTrimBtn_    .setToggleState (tool == Body::Tool::Trim,     juce::dontSendNotification);
    toolAuditionBtn_.setToggleState (tool == Body::Tool::Audition, juce::dontSendNotification);
}

void ArrangementView::onLoopToggled()
{
    if (body_ == nullptr) return;
    body_->setLoopActive (loopBtn_.getToggleState());
}

int64_t ArrangementView::beatsToFrames (double beats) const noexcept
{
    if (monitor_ == nullptr) return 0;
    const double bpm = juce::jmax (1.0, (double) monitor_->tempo.get());
    const double sr  = juce::jmax (1.0, (double) monitor_->sampleRate.get());
    const double secs = beats * 60.0 / bpm;
    return (int64_t) std::llround (secs * sr);
}

double ArrangementView::framesToBeats (int64_t frames) const noexcept
{
    if (monitor_ == nullptr) return 0.0;
    const double bpm = juce::jmax (1.0, (double) monitor_->tempo.get());
    const double sr  = juce::jmax (1.0, (double) monitor_->sampleRate.get());
    const double secs = (double) frames / sr;
    return secs * bpm / 60.0;
}

void ArrangementView::seekToBeat (double beats)
{
    if (services_ == nullptr) return;
    if (auto* eng = services_->context().audio().get())
        eng->seekToAudioFrame (beatsToFrames (juce::jmax (0.0, beats)));
}

void ArrangementView::timerCallback()
{
    if (monitor_ == nullptr) return;

    const bool playing   = monitor_->playing.get();
    const bool recording = monitor_->recording.get();
    const double beat    = computePlayheadBeats();

    /* Transport start edge: clear per-lane "last dispatched" so the
     * first region under the playhead fires when playback resumes. */
    if (! wasPlaying_ && playing)
        for (auto& rs : laneRuntime_)
        {
            rs.lastDispatchedRegion = juce::Uuid::null();
            rs.lastDispatchedSeqIdx = -1;
        }

    /* Transport stop edge: silence audio lanes so playback halts on
     * stop.  Tracker lanes don't get an explicit stop here -- their
     * sequences carry "playing" state in vht state and stop on next
     * launch start.  DAW convention is audio stops with transport. */
    if (wasPlaying_ && ! playing)
        stopAllAudioLanes();

    /* Transport-recording rising edge: snapshot playhead position
     * so Body can paint a placeholder growing rect from here to
     * the current playhead on each armed lane. */
    if (recording && ! wasRecording_)
        recordStartBeat_ = beat;

    /* Repaint armed lanes while transport-recording so the REC
     * indicator + growing-placeholder rect refresh.  The
     * dispatchAtBeat path only repaints lanes that change region
     * state -- which doesn't happen during capture (no region
     * exists until finalise). */
    const bool recordingStateChanged = (recording != wasRecording_);
    if (recording || recordingStateChanged)
    {
        if (body_ != nullptr)
        {
            for (int i = 0; i < lanes_.size(); ++i)
            {
                const auto& l = lanes_.getReference (i);
                const auto& rs = laneRuntime_.getReference (i);
                if (rs.isAudioLane() && l.armed)
                    body_->repaintLane (i);
            }
        }
    }
    wasRecording_ = recording;

    /* Loop wrap: if loop is armed and the playhead has crossed the
     * range-end boundary, seek the transport back to range-start.
     * Driven from the 30 Hz UI timer -- one tick of jitter at the
     * wrap is acceptable for v1; future move to engine-side seek
     * for sample-accurate loop. */
    if (playing && body_ != nullptr && body_->isLooping())
    {
        const double loopStart = body_->rangeStart();
        const double loopEnd   = body_->rangeEnd();
        if (beat >= loopEnd - 1e-6 && loopEnd > loopStart)
        {
            seekToBeat (loopStart);
            for (auto& rs : laneRuntime_)
            {
                rs.lastDispatchedRegion = juce::Uuid::null();
                rs.lastDispatchedSeqIdx = -1;
            }
        }
    }

    /* Clip volume envelope -- push the evaluated live gain to each
     * audio lane whose last-dispatched region carries an envelope.
     * 30 Hz update rate; coarser than per-sample but matches Bitwig's
     * display refresh + is plenty for most musical envelopes.  Lanes
     * without an envelope clear the override (NaN) so the per-launch
     * static gain takes back over. */
    if (playing)
    {
        for (int i = 0; i < lanes_.size(); ++i)
        {
            auto& rs = laneRuntime_.getReference (i);
            if (! rs.isAudioLane()) continue;
            if (rs.lastDispatchedRegion.isNull())
            {
                rs.audioClipCache->setLiveGain (std::numeric_limits<float>::quiet_NaN());
                continue;
            }
            const auto& lane = lanes_.getReference (i);
            const auto* r = lane.playlist.findRegion (rs.lastDispatchedRegion);
            if (r == nullptr || r->volumeEnvelope.size() < 2)
            {
                rs.audioClipCache->setLiveGain (std::numeric_limits<float>::quiet_NaN());
                continue;
            }
            const double localBeat = beat - r->positionBeats;
            const float  envDb = r->gainAtBeatOffset (localBeat);
            const float  linear = juce::Decibels::decibelsToGain (envDb);
            rs.audioClipCache->setLiveGain (linear);
        }
    }

    if (playing) dispatchAtBeat (beat);

    if (body_ != nullptr && (playing || wasPlaying_ != playing))
    {
        const int oldPxX = (lastBeat_ > -0.001)
                              ? Body::kLabelW + (int) (lastBeat_ * body_->kPxPerBeat)
                              : -1;
        const int newPxX = Body::kLabelW + (int) (beat * body_->kPxPerBeat);
        if (oldPxX != newPxX || wasPlaying_ != playing)
            body_->repaintPlayhead (oldPxX, newPxX);
    }

    lastBeat_   = beat;
    wasPlaying_ = playing;
    updateTransportLabel();
}

} // namespace element
