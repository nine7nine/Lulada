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
                              public juce::FileDragAndDropTarget
{
public:
    explicit Body (ArrangementView& o) : owner (o)
    {
        setOpaque (true);
        setWantsKeyboardFocus (true);   // Delete key handling
    }

    void paint (Graphics& g) override
    {
        g.fillAll (Colors::contentBackgroundColor);

        const int laneCount = owner.lanes_.size();
        for (int i = 0; i < laneCount; ++i)
            paintLane (g, i);

        if (laneCount == 0)
        {
            g.setColour (Colors::textColor.withAlpha (0.5f));
            g.setFont (juce::FontOptions (14.0f));
            g.drawText ("Drop an audio file here, or click + Audio to add a track.",
                        getLocalBounds(),
                        juce::Justification::centred);
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
        const int laneIdx = e.y / kLaneH;
        if (laneIdx < 0 || laneIdx >= owner.lanes_.size()) return;
        auto& lane    = owner.lanes_.getReference (laneIdx);
        auto& runtime = owner.laneRuntime_.getReference (laneIdx);

        if (e.x < kLabelW)
        {
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
        const auto& regions = lane.playlist.regions();
        for (const auto& r : regions)
        {
            if (! r.containsBeat (beat)) continue;

            /* Begin a gesture; whether it ends in move/resize/click
             * is decided by what happens between mouseDown and
             * mouseUp. */
            gesture_.laneIdx         = laneIdx;
            gesture_.regionId        = r.id;
            gesture_.originalPos     = r.positionBeats;
            gesture_.originalLen     = r.lengthBeats;
            gesture_.mouseDownXBeats = beat;
            gesture_.dragActive      = false;

            /* Resize-vs-move hit test: cursor in the last kEdgeHandlePx
             * pixels of the rendered region rect = Resize.  Otherwise
             * Move. */
            const int regionStartX = kLabelW + (int) (r.positionBeats * kPxPerBeat);
            const int regionEndX   = kLabelW + (int) (r.endBeats()    * kPxPerBeat);
            gesture_.mode = (e.x >= regionEndX - kEdgeHandlePx && e.x <= regionEndX)
                              ? Gesture::Resize
                              : Gesture::Move;

            selectedLane_    = laneIdx;
            selectedRegion_  = r.id;

            grabKeyboardFocus();
            repaintLane (laneIdx);
            return;
        }

        /* Empty area click -- clear selection. */
        selectedLane_   = -1;
        selectedRegion_ = juce::Uuid::null();
        gesture_        = Gesture {};
        repaint();
    }

    void mouseDrag (const MouseEvent& e) override
    {
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

        if (gesture_.mode == Gesture::Move)
        {
            const double delta = mouseBeat - gesture_.mouseDownXBeats;
            const double target = juce::jmax (0.0, gesture_.originalPos + delta);

            /* moveRegion enforces no-overlap; if the target collides,
             * snap to the latest non-overlapping position before it.
             * For v1 we just attempt; failure leaves the region in
             * place. */
            lane.playlist.moveRegion (gesture_.regionId, target);
        }
        else /* Resize */
        {
            const double newLength = juce::jmax (kMinRegionBeats,
                mouseBeat - gesture_.originalPos);
            lane.playlist.resizeRegion (gesture_.regionId, newLength);
        }

        if (body_resizeNeeded()) resizeForLanes();
        else                     repaintLane (gesture_.laneIdx);
    }

    void mouseUp (const MouseEvent& e) override
    {
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
                runtime.audioClipCache->schedulePlay (
                    r->id, r->sourceId, -1.0, 0);
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
        const int laneIdx = e.y / kLaneH;
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
        for (const auto& r : lane.playlist.regions())
        {
            const int regionStartX = kLabelW + (int) (r.positionBeats * kPxPerBeat);
            const int regionEndX   = kLabelW + (int) (r.endBeats()    * kPxPerBeat);
            if (e.x >= regionEndX - kEdgeHandlePx && e.x <= regionEndX
                && e.y >= laneIdx * kLaneH && e.y < (laneIdx + 1) * kLaneH)
            {
                setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
                return;
            }
            if (e.x >= regionStartX && e.x < regionEndX
                && e.y >= laneIdx * kLaneH && e.y < (laneIdx + 1) * kLaneH)
            {
                setMouseCursor (juce::MouseCursor::DraggingHandCursor);
                return;
            }
        }
        setMouseCursor (juce::MouseCursor::NormalCursor);
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
        return false;
    }

    //==========================================================================
    // Layout helpers

    void resizeForLanes()
    {
        int maxBeats = 32;
        for (const auto& l : owner.lanes_)
        {
            double end = 0.0;
            for (const auto& r : l.playlist.regions())
                end = juce::jmax (end, r.endBeats());
            const int needed = (int) end + 8;
            if (needed > maxBeats) maxBeats = needed;
        }
        const int w = kLabelW + maxBeats * kPxPerBeat;
        const int h = juce::jmax (kLaneH, owner.lanes_.size() * kLaneH);
        if (w != getWidth() || h != getHeight())
            setSize (w, h);
        else
            repaint();
    }

    void repaintLane (int idx)
    {
        if (idx < 0) { repaint(); return; }
        repaint (0, idx * kLaneH, getWidth(), kLaneH);
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

    static constexpr int kLabelW         = 160;
    static constexpr int kLaneH          = 64;
    static constexpr int kPxPerBeat      = 24;
    static constexpr int kEdgeHandlePx   = 6;    /* width of right-edge resize handle */
    static constexpr int kDragThresholdPx = 4;   /* pixels before mouseDown -> drag */
    static constexpr double kMinRegionBeats = 0.25;

private:
    /** Bounding box of the arm-toggle dot within a lane's label area.
     *  Small square in the top-right corner; used by mouseDown to
     *  detect clicks. */
    Rectangle<int> armToggleRect (int laneIdx) const noexcept
    {
        constexpr int sz   = 12;
        constexpr int pad  = 6;
        const int y = laneIdx * kLaneH + pad;
        const int x = kLabelW - sz - pad;
        return Rectangle<int> (x, y, sz, sz);
    }

    void paintLane (Graphics& g, int laneIdx)
    {
        const auto& lane    = owner.lanes_.getReference (laneIdx);
        const auto& runtime = owner.laneRuntime_.getReference (laneIdx);
        const int y = laneIdx * kLaneH;
        const Rectangle<int> bounds (0, y, getWidth(), kLaneH);

        const bool alt = (laneIdx & 1) != 0;
        g.setColour (alt ? Colors::widgetBackgroundColor
                         : Colors::widgetBackgroundColor.darker (0.25f));
        g.fillRect (bounds);

        const Rectangle<int> labelArea (0, y, kLabelW, kLaneH);
        g.setColour (Colors::widgetBackgroundColor.darker (0.6f));
        g.fillRect (labelArea);

        const bool orphan = runtime.isOrphan();
        const bool isAudio = runtime.isAudioLane();

        /* Lane name + (orphan) / [audio] tags. */
        g.setColour (orphan ? Colors::textColor.withAlpha (0.45f)
                            : Colors::textColor);
        g.setFont (juce::FontOptions (12.0f));
        juce::String label = lane.name.isNotEmpty()
                                ? lane.name
                                : (isAudio ? juce::String ("Audio") : juce::String ("Tracker"));
        if (orphan) label += " (orphan)";
        g.drawText (label, labelArea.reduced (8, 0),
                    juce::Justification::centredLeft, true);

        /* Arm toggle (audio lanes only). */
        if (isAudio)
        {
            const auto rect = armToggleRect (laneIdx);
            g.setColour (lane.armed ? Colour::fromRGB (220, 70, 70)
                                    : Colour::fromRGB (60, 60, 60));
            g.fillRect (rect);
            g.setColour (lane.armed ? Colour::fromRGB (255, 120, 120)
                                    : Colour::fromRGB (90, 90, 90));
            g.drawRect (rect, 1);
        }

        /* Strip background + beat grid. */
        const Rectangle<int> stripArea (kLabelW, y, getWidth() - kLabelW, kLaneH);
        g.setColour (Colors::widgetBackgroundColor.darker (0.4f));
        g.fillRect (stripArea);

        g.setColour (Colors::widgetBackgroundColor.brighter (0.05f));
        for (int x = 0; x < stripArea.getWidth(); x += kPxPerBeat * 4)
            g.drawVerticalLine (stripArea.getX() + x,
                                (float) stripArea.getY(),
                                (float) stripArea.getBottom());

        /* Regions.  Color scheme branches on lane kind:
         *   Tracker idle  = blue (90, 130, 170)
         *   Tracker active= orange (220, 140, 60)
         *   Audio idle    = green (90, 170, 130)
         *   Audio active  = cyan (60, 180, 200)
         *   Orphan        = desaturated darker
         */
        for (const auto& r : lane.playlist.regions())
        {
            const int xs = stripArea.getX() + (int) (r.positionBeats * kPxPerBeat);
            const int ws = juce::jmax (4, (int) (r.lengthBeats * kPxPerBeat));
            Rectangle<int> rect (xs, stripArea.getY() + 4,
                                 ws, stripArea.getHeight() - 8);

            const bool active = (r.id == runtime.lastDispatchedRegion);
            Colour fill;
            if (isAudio)
                fill = active ? Colour::fromRGB ( 60, 180, 200)
                              : Colour::fromRGB ( 90, 170, 130);
            else
                fill = active ? Colour::fromRGB (220, 140,  60)
                              : Colour::fromRGB ( 90, 130, 170);

            if (orphan)
                fill = fill.withMultipliedSaturation (0.3f)
                           .withMultipliedBrightness (0.6f);

            g.setColour (fill);
            g.fillRect (rect);

            const bool selected = (laneIdx == selectedLane_ && r.id == selectedRegion_);
            if (selected)
            {
                g.setColour (Colours::white);
                g.drawRect (rect, 2);
            }
            else
            {
                g.setColour (fill.brighter (0.4f));
                g.drawRect (rect, 1);
            }
            g.setColour (Colours::white);
            g.setFont (juce::FontOptions (11.0f));

            const juce::String tag = r.sequenceIdx >= 0
                                        ? "P" + String (r.sequenceIdx)
                                        : (r.name.isNotEmpty() ? r.name : String ("Audio"));
            g.drawText (tag, rect.reduced (4, 0),
                        juce::Justification::centredLeft, true);
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
};

/* ===================================================================== */

ArrangementView::ArrangementView()
{
    setName (EL_VIEW_ARRANGEMENT);

    addAndMakeVisible (rescanBtn_);
    addAndMakeVisible (addAudioBtn_);
    addAndMakeVisible (loadAudioBtn_);
    addAndMakeVisible (posLabel_);
    addAndMakeVisible (bpmLabel_);
    addAndMakeVisible (viewport_);

    body_ = std::make_unique<Body> (*this);
    viewport_.setViewedComponent (body_.get(), false);
    viewport_.setScrollBarsShown (true, true);

    posLabel_.setText ("Beat: 0.0", juce::dontSendNotification);
    posLabel_.setColour (juce::Label::textColourId, Colors::textColor);
    bpmLabel_.setText ("BPM: 120.0", juce::dontSendNotification);
    bpmLabel_.setColour (juce::Label::textColourId, Colors::textColor);

    rescanBtn_.onClick    = [this]() { rescanLaneTargets(); };
    addAudioBtn_.onClick  = [this]() { createEmptyAudioLane (true /*stereo*/); };
    loadAudioBtn_.onClick = [this]() { promptLoadAudioFile(); };
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
    startTimerHz (30);
}

void ArrangementView::willBeRemoved()
{
    stopTimer();
    detachFromActiveGraph();
}

void ArrangementView::stabilizeContent()
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
    if (child.hasType (types::Node) || child.hasType (tags::nodes))
        rescanLaneTargets();
}

void ArrangementView::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    if (child.hasType (types::Node) || child.hasType (tags::nodes))
        rescanLaneTargets();
}

void ArrangementView::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (kHeaderH).reduced (4, 4);
    rescanBtn_    .setBounds (top.removeFromLeft (56)); top.removeFromLeft (6);
    addAudioBtn_  .setBounds (top.removeFromLeft (72)); top.removeFromLeft (6);
    loadAudioBtn_ .setBounds (top.removeFromLeft (72)); top.removeFromLeft (12);
    bpmLabel_     .setBounds (top.removeFromLeft (96)); top.removeFromLeft (4);
    posLabel_     .setBounds (top.removeFromLeft (96));
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
        juce::jmax (0.0, (double) (bodyX - kLabelW) / (double) kPxPerBeat);

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
            if (auto* t = dynamic_cast<TrackerNode*> (proc))
            {
                outNodes.add (child);
                outTrackers.add (t);
                outAudioClips.add (nullptr);
                continue;
            }
            if (auto* a = dynamic_cast<AudioClipNode*> (proc))
            {
                outNodes.add (child);
                outTrackers.add (nullptr);
                outAudioClips.add (a);
                continue;
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

    /* Rebuild runtime state in lockstep.  For each persisted lane,
     * resolve which kind of node it binds to + wire up the
     * AudioLaneAdapter / record commit handler if applicable. */
    laneRuntime_.clearQuick();
    laneRuntime_.ensureStorageAllocated (lanes_.size());
    for (int i = 0; i < lanes_.size(); ++i)
    {
        const auto& l = lanes_.getReference (i);
        LaneRuntimeState s;
        s.trackerCache   = resolveTrackerByUuid (l.targetNodeUuid);
        s.audioClipCache = (s.trackerCache != nullptr)
                              ? nullptr
                              : resolveAudioClipByUuid (l.targetNodeUuid);

        juce::Logger::writeToLog (
            juce::String ("[ArrangementView::rescanLaneTargets] lane[")
            + juce::String (i) + "] name=" + l.name
            + " targetUuid=" + l.targetNodeUuid.toString()
            + " tracker=" + (s.trackerCache   ? "yes" : "no")
            + " audio="   + (s.audioClipCache ? "yes" : "no"));

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

                    /* Append a Region at the end of the existing
                     * playlist.  positionBeats = max region endBeats,
                     * or 0 if empty.  lengthBeats derived from file
                     * duration + session bpm; defaults to 120 bpm
                     * when no monitor is available. */
                    double position = 0.0;
                    for (const auto& r : lane.playlist.regions())
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

double ArrangementView::computePlayheadBeats() const
{
    if (monitor_ == nullptr) return 0.0;
    return (double) monitor_->getPositionBeats();
}

void ArrangementView::dispatchAtBeat (double beat)
{
    for (int laneIdx = 0; laneIdx < lanes_.size(); ++laneIdx)
    {
        const auto& lane    = lanes_.getReference (laneIdx);
        auto&       runtime = laneRuntime_.getReference (laneIdx);
        if (runtime.isOrphan()) continue;

        const Region* active = lane.playlist.regionAt (beat);
        if (active == nullptr) continue;
        if (active->id == runtime.lastDispatchedRegion) continue;

        if (runtime.isTrackerLane())
        {
            if (active->sequenceIdx >= 0)
            {
                if (runtime.lastDispatchedSeqIdx >= 0
                    && runtime.lastDispatchedSeqIdx != active->sequenceIdx)
                    runtime.trackerCache->schedulePlaying (
                        runtime.lastDispatchedSeqIdx, -1.0, false);
                runtime.trackerCache->schedulePlaying (active->sequenceIdx, -1.0, true);
            }
        }
        else if (runtime.isAudioLane())
        {
            runtime.audioClipCache->schedulePlay (
                active->id, active->sourceId, -1.0, 0 /*sampleOffset*/);
        }

        runtime.lastDispatchedRegion = active->id;
        runtime.lastDispatchedSeqIdx = active->sequenceIdx;
        if (body_ != nullptr) body_->repaintLane (laneIdx);
    }
}

void ArrangementView::stopAllAudioLanes()
{
    for (auto& rs : laneRuntime_)
        if (rs.isAudioLane())
            rs.audioClipCache->scheduleStop (juce::Uuid::null(), -1.0);
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
    lane.colour         = juce::Colour::fromRGB (90, 170, 130);
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
        runtime.audioClipCache->schedulePlay (
            r.id, r.sourceId, -1.0, 0);

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
    if (yPx < 0) return -1;
    const int idx = yPx / kLaneH;
    if (idx < 0 || idx >= lanes_.size()) return -1;
    return idx;
}

void ArrangementView::updateTransportLabel()
{
    if (monitor_ == nullptr) return;
    const float bpm = monitor_->tempo.get();
    if (std::abs (bpm - lastBpmShown_) > 0.05f)
    {
        bpmLabel_.setText ("BPM: " + String (bpm, 1), juce::dontSendNotification);
        lastBpmShown_ = bpm;
    }
    const double beat = lastBeat_;
    if (std::abs (beat - lastBeatShown_) > 0.01)
    {
        posLabel_.setText ("Beat: " + String (beat, 2), juce::dontSendNotification);
        lastBeatShown_ = beat;
    }
}

void ArrangementView::timerCallback()
{
    if (monitor_ == nullptr) return;

    const bool playing = monitor_->playing.get();
    const double beat  = computePlayheadBeats();

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

    if (playing) dispatchAtBeat (beat);

    if (body_ != nullptr && (playing || wasPlaying_ != playing))
    {
        const int oldPxX = (lastBeat_ > -0.001)
                              ? Body::kLabelW + (int) (lastBeat_ * Body::kPxPerBeat)
                              : -1;
        const int newPxX = Body::kLabelW + (int) (beat * Body::kPxPerBeat);
        if (oldPxX != newPxX || wasPlaying_ != playing)
            body_->repaintPlayhead (oldPxX, newPxX);
    }

    lastBeat_   = beat;
    wasPlaying_ = playing;
    updateTransportLabel();
}

} // namespace element
