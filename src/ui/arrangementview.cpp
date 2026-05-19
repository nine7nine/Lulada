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
        auto& lane = owner.lanes_.getReference (laneIdx);
        if (e.x < kLabelW || lane.tracker == nullptr) return;

        const double beat = (e.x - kLabelW) / (double) kPxPerBeat;
        for (int i = 0; i < lane.blocks.size(); ++i)
        {
            const auto& b = lane.blocks.getReference (i);
            if (beat >= b.startBeat && beat < b.startBeat + b.lengthBeats)
            {
                lane.tracker->advanceToPattern (b.patternIdx);
                lane.lastDispatchedBlockIdx = i;
                repaintLane (laneIdx);
                return;
            }
        }
    }

    /** Recompute size based on current lanes_ + longest block strip. */
    void resizeForLanes()
    {
        int maxBeats = 32;
        for (const auto& l : owner.lanes_)
        {
            double end = 0.0;
            for (const auto& b : l.blocks)
                end = juce::jmax (end, b.startBeat + b.lengthBeats);
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
        const auto& lane = owner.lanes_.getReference (laneIdx);
        const int y = laneIdx * kLaneH;
        const Rectangle<int> bounds (0, y, getWidth(), kLaneH);

        const bool alt = (laneIdx & 1) != 0;
        g.setColour (alt ? Colors::widgetBackgroundColor
                         : Colors::widgetBackgroundColor.darker (0.25f));
        g.fillRect (bounds);

        /* Label column. */
        const Rectangle<int> labelArea (0, y, kLabelW, kLaneH);
        g.setColour (Colors::widgetBackgroundColor.darker (0.6f));
        g.fillRect (labelArea);
        g.setColour (Colors::textColor);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (lane.name, labelArea.reduced (8, 0),
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

        /* Blocks. */
        for (int i = 0; i < lane.blocks.size(); ++i)
        {
            const auto& b = lane.blocks.getReference (i);
            const int xs = stripArea.getX() + (int) (b.startBeat * kPxPerBeat);
            const int ws = juce::jmax (4, (int) (b.lengthBeats * kPxPerBeat));
            Rectangle<int> r (xs, stripArea.getY() + 4, ws, stripArea.getHeight() - 8);

            const bool active = (i == lane.lastDispatchedBlockIdx);
            Colour fill = active ? Colour::fromRGB (220, 140, 60)
                                 : Colour::fromRGB (90, 130, 170);
            g.setColour (fill);
            g.fillRect (r);
            g.setColour (fill.brighter (0.4f));
            g.drawRect (r, 1);
            g.setColour (Colours::white);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText ("P" + String (b.patternIdx),
                        r.reduced (4, 0),
                        juce::Justification::centredLeft, true);
        }

        /* Playhead overlay — single thin vertical line. */
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

static void collectTrackersFromGraph (const Node& graph,
                                      juce::Array<TrackerNode*>& out,
                                      juce::Array<juce::String>& names)
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
                out.add (t);
                names.add (child.getName().isNotEmpty() ? child.getName() : String ("Tracker"));
                continue;
            }
        }
        if (child.isGraph())
            collectTrackersFromGraph (child, out, names);
    }
}

void ArrangementView::rescanTrackers()
{
    lanes_.clearQuick();

    if (services_ != nullptr)
    {
        if (auto session = services_->context().session())
        {
            auto active = session->getActiveGraph();
            if (active.isValid())
            {
                juce::Array<TrackerNode*> ts;
                juce::Array<String> names;
                collectTrackersFromGraph (active, ts, names);

                for (int i = 0; i < ts.size(); ++i)
                {
                    Lane lane;
                    lane.tracker = ts[i];
                    lane.name = names[i];
                    lane.numPatterns = ts[i] != nullptr ? ts[i]->numPatterns() : 0;

                    /* v0 auto-fill: one block per pattern, equal 4-beat
                     * length, laid out end-to-end.  Real authoring lands
                     * next. */
                    double cursor = 0.0;
                    for (int p = 0; p < lane.numPatterns; ++p)
                    {
                        Block b;
                        b.patternIdx = p;
                        b.startBeat = cursor;
                        b.lengthBeats = 4.0;
                        lane.blocks.add (b);
                        cursor += b.lengthBeats;
                    }

                    lanes_.add (lane);
                }
            }
        }
    }

    if (body_ != nullptr) body_->resizeForLanes();
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
        auto& lane = lanes_.getReference (laneIdx);
        if (lane.tracker == nullptr || lane.blocks.isEmpty()) continue;
        int newIdx = -1;
        for (int i = 0; i < lane.blocks.size(); ++i)
        {
            const auto& b = lane.blocks.getReference (i);
            if (beat >= b.startBeat && beat < b.startBeat + b.lengthBeats)
            {
                newIdx = i;
                break;
            }
        }
        if (newIdx >= 0 && newIdx != lane.lastDispatchedBlockIdx)
        {
            lane.tracker->advanceToPattern (lane.blocks.getReference (newIdx).patternIdx);
            lane.lastDispatchedBlockIdx = newIdx;
            if (body_ != nullptr) body_->repaintLane (laneIdx);
        }
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

    if (! wasPlaying_ && playing)
        for (auto& l : lanes_) l.lastDispatchedBlockIdx = -1;

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
