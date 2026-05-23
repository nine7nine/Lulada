// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/arrangementview.hpp"

#include <element/audioengine.hpp>
#include <element/context.hpp>
#include <element/node.hpp>
#include <element/session.hpp>
#include <element/services.hpp>
#include <element/tags.hpp>
#include <element/ui/style.hpp>

#include "nodes/tracker.hpp"

namespace element {

using juce::Colour;
using juce::Graphics;
using juce::MouseEvent;
using juce::Rectangle;
using juce::String;
namespace Colours = juce::Colours;

/* ===================================================================== */
/* Body: single inline-paint component that draws every lane in one      */
/* paint() pass.  Cheaper than a tree of LaneStrip children for the      */
/* common case of small lane counts; partial repaints are surgical.     */
/* ===================================================================== */

class ArrangementView::Body : public juce::Component
{
public:
    explicit Body (ArrangementView& o) : owner (o) { setOpaque (true); }

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
            g.drawText ("No Tracker nodes in graph. Add one to see lanes.",
                        getLocalBounds(),
                        juce::Justification::centred);
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        const int laneIdx = e.y / kLaneH;
        if (laneIdx < 0 || laneIdx >= owner.lanes_.size()) return;
        auto& lane    = owner.lanes_.getReference (laneIdx);
        auto& runtime = owner.laneRuntime_.getReference (laneIdx);
        if (e.x < kLabelW || runtime.trackerCache == nullptr) return;

        const double beat = (e.x - kLabelW) / (double) kPxPerBeat;
        const auto& regions = lane.playlist.regions();
        for (const auto& r : regions)
        {
            if (r.containsBeat (beat))
            {
                if (r.sequenceIdx >= 0)
                    runtime.trackerCache->advanceToPattern (r.sequenceIdx);
                runtime.lastDispatchedRegion  = r.id;
                runtime.lastDispatchedSeqIdx  = r.sequenceIdx;
                repaintLane (laneIdx);
                return;
            }
        }
    }

    /** Recompute size based on current lanes_ + longest region strip. */
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

    /** Partial repaint of a single lane + playhead vertical sweep. */
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

    static constexpr int kLabelW    = 160;
    static constexpr int kLaneH     = 64;
    static constexpr int kPxPerBeat = 24;

private:
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

        /* Label column.  Orphaned lanes (target tracker missing) paint
         * dimmed so the user can tell they don't dispatch. */
        const Rectangle<int> labelArea (0, y, kLabelW, kLaneH);
        g.setColour (Colors::widgetBackgroundColor.darker (0.6f));
        g.fillRect (labelArea);
        const bool orphaned = (runtime.trackerCache == nullptr);
        g.setColour (orphaned ? Colors::textColor.withAlpha (0.45f)
                              : Colors::textColor);
        g.setFont (juce::FontOptions (12.0f));
        const juce::String label = orphaned ? (lane.name + " (orphan)") : lane.name;
        g.drawText (label, labelArea.reduced (8, 0),
                    juce::Justification::centredLeft, true);

        /* Strip background + beat grid. */
        const Rectangle<int> stripArea (kLabelW, y, getWidth() - kLabelW, kLaneH);
        g.setColour (Colors::widgetBackgroundColor.darker (0.4f));
        g.fillRect (stripArea);

        g.setColour (Colors::widgetBackgroundColor.brighter (0.05f));
        for (int x = 0; x < stripArea.getWidth(); x += kPxPerBeat * 4)
            g.drawVerticalLine (stripArea.getX() + x,
                                (float) stripArea.getY(),
                                (float) stripArea.getBottom());

        /* Regions. */
        for (const auto& r : lane.playlist.regions())
        {
            const int xs = stripArea.getX() + (int) (r.positionBeats * kPxPerBeat);
            const int ws = juce::jmax (4, (int) (r.lengthBeats * kPxPerBeat));
            Rectangle<int> rect (xs, stripArea.getY() + 4, ws, stripArea.getHeight() - 8);

            const bool active = (r.id == runtime.lastDispatchedRegion);
            Colour fill = active ? Colour::fromRGB (220, 140, 60)
                                 : Colour::fromRGB (90, 130, 170);
            if (orphaned) fill = fill.withMultipliedSaturation (0.3f)
                                     .withMultipliedBrightness (0.6f);
            g.setColour (fill);
            g.fillRect (rect);
            g.setColour (fill.brighter (0.4f));
            g.drawRect (rect, 1);
            g.setColour (Colours::white);
            g.setFont (juce::FontOptions (11.0f));
            /* For tracker regions the meaningful display is the
             * sequence index ("P0", "P1", ...).  Audio regions
             * (Phase 3+) will use r.name. */
            const juce::String tag = r.sequenceIdx >= 0
                                        ? "P" + String (r.sequenceIdx)
                                        : (r.name.isNotEmpty() ? r.name
                                                               : String ("Audio"));
            g.drawText (tag, rect.reduced (4, 0),
                        juce::Justification::centredLeft, true);
        }

        /* Playhead overlay -- single thin vertical line. */
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

    ArrangementView& owner;
};

/* ===================================================================== */

ArrangementView::ArrangementView()
{
    setName (EL_VIEW_ARRANGEMENT);

    addAndMakeVisible (rescanBtn_);
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

    rescanBtn_.onClick = [this]() { rescanTrackers(); };
}

ArrangementView::~ArrangementView()
{
    stopTimer();
    detachFromActiveGraph();
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
    rescanTrackers();
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
    rescanTrackers();
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
        rescanTrackers();
}

void ArrangementView::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    if (child.hasType (types::Node) || child.hasType (tags::nodes))
        rescanTrackers();
}

void ArrangementView::resized()
{
    /* Transport (play/stop/record) lives in the global top toolbar —
     * don't duplicate it here.  Tight tracker-toolbar-style spacing. */
    auto r = getLocalBounds();
    auto top = r.removeFromTop (kHeaderH).reduced (4, 4);
    rescanBtn_.setBounds (top.removeFromLeft (56)); top.removeFromLeft (10);
    bpmLabel_ .setBounds (top.removeFromLeft (96)); top.removeFromLeft (4);
    posLabel_ .setBounds (top.removeFromLeft (96));
    viewport_.setBounds (r);
    if (body_ != nullptr) body_->resizeForLanes();
}

void ArrangementView::paint (Graphics& g)
{
    g.fillAll (Colors::contentBackgroundColor);
    g.setColour (Colors::backgroundColor);
    g.fillRect (getLocalBounds().removeFromTop (kHeaderH));
}

/* ===================================================================== */

namespace {
/** Recursive: append (Node, TrackerNode*) pairs for every TrackerNode
 *  reachable from `graph` (including subgraphs).  Used to seed lanes
 *  for trackers the user adds. */
void collectTrackersFromGraph (const Node& graph,
                                juce::Array<Node>&         outNodes,
                                juce::Array<TrackerNode*>& outProcs)
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
                outProcs.add (t);
                continue;
            }
        }
        if (child.isGraph())
            collectTrackersFromGraph (child, outNodes, outProcs);
    }
}

/** Recursive: find the first node with `targetUuid`.  Returns
 *  default Node() on miss.  O(graph size); cheap. */
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

void ArrangementView::autoFillLaneForTracker (Lane& lane, TrackerNode* trk)
{
    /* One Region per existing sequence, 4 beats each, end-to-end.
     * Same shape as the v0 auto-fill but expressed via the shared
     * Region model.  sourceId = tracker's uuid (so resolvers can
     * map back to the owning TrackerNode); sequenceIdx is the
     * canonical "which pattern" pointer. */
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

void ArrangementView::rescanTrackers()
{
    /* First-ever activation: pull persisted lanes off the session.
     * Empty result is the normal case for pre-Phase-1e sessions --
     * graph-walk auto-fill below fills in defaults for any tracker
     * that doesn't yet have a lane. */
    if (! lanesLoadedFromSession_)
    {
        loadLanesFromSession();
        lanesLoadedFromSession_ = true;
    }

    /* Collect current trackers + decide which need a new lane.
     * Lanes already pointing at an existing tracker stay put. */
    juce::Array<Node>           foundNodes;
    juce::Array<TrackerNode*>   foundProcs;
    if (services_ != nullptr)
    {
        if (auto sess = services_->context().session())
        {
            const Node active = sess->getActiveGraph();
            if (active.isValid())
                collectTrackersFromGraph (active, foundNodes, foundProcs);
        }
    }

    bool mutated = false;
    for (int i = 0; i < foundNodes.size(); ++i)
    {
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
        autoFillLaneForTracker (lane, foundProcs[i]);
        lanes_.add (std::move (lane));
        mutated = true;
    }

    /* Resync the parallel runtime-state array + resolve each lane's
     * TrackerNode* cache via uuid lookup.  Cheap O(lanes * graph
     * size); runs on graph topology change, not per render block. */
    laneRuntime_.clearQuick();
    for (const auto& l : lanes_)
    {
        LaneRuntimeState s;
        s.trackerCache = resolveTrackerByUuid (l.targetNodeUuid);
        laneRuntime_.add (s);
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
    /* Rebuild from scratch -- simpler than diff-edit + only runs on
     * mutation, not per frame.  No undo support yet (Phase 1e ships
     * read+auto-fill+persist; authoring + undo come later). */
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
    /* Idempotent per region: the same region won't dispatch twice on
     * consecutive ticks (runtime.lastDispatchedRegion gates).
     * Orphaned lanes (trackerCache == nullptr) skip silently. */
    for (int laneIdx = 0; laneIdx < lanes_.size(); ++laneIdx)
    {
        const auto& lane    = lanes_.getReference (laneIdx);
        auto&       runtime = laneRuntime_.getReference (laneIdx);
        if (runtime.trackerCache == nullptr) continue;

        const Region* active = lane.playlist.regionAt (beat);
        if (active == nullptr) continue;
        if (active->id == runtime.lastDispatchedRegion) continue;

        if (active->sequenceIdx >= 0)
            runtime.trackerCache->advanceToPattern (active->sequenceIdx);
        runtime.lastDispatchedRegion = active->id;
        runtime.lastDispatchedSeqIdx = active->sequenceIdx;
        if (body_ != nullptr) body_->repaintLane (laneIdx);
    }
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
    const double beat = computePlayheadBeats();

    /* Transport start edge: clear the per-lane "last dispatched"
     * gate so the first region under the playhead fires when
     * playback resumes (otherwise it would be considered already
     * dispatched from before the stop). */
    if (! wasPlaying_ && playing)
        for (auto& rs : laneRuntime_)
        {
            rs.lastDispatchedRegion = juce::Uuid::null();
            rs.lastDispatchedSeqIdx = -1;
        }

    if (playing) dispatchAtBeat (beat);

    /* Partial repaint of the playhead sweep — only the old + new vertical
     * line columns, not the whole view. */
    if (body_ != nullptr && (playing || wasPlaying_ != playing))
    {
        const int oldPxX = (lastBeat_ > -0.001) ? Body::kLabelW + (int) (lastBeat_ * Body::kPxPerBeat) : -1;
        const int newPxX = Body::kLabelW + (int) (beat * Body::kPxPerBeat);
        if (oldPxX != newPxX || wasPlaying_ != playing)
            body_->repaintPlayhead (oldPxX, newPxX);
    }

    lastBeat_ = beat;
    wasPlaying_ = playing;
    updateTransportLabel();
}

} // namespace element
