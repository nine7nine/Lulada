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
        return anyAudioFileIn (files);
    }

    void fileDragEnter (const juce::StringArray& files, int x, int y) override
    {
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

    void mouseDown (const MouseEvent& e) override
    {
        const int laneIdx = e.y / kLaneH;
        if (laneIdx < 0 || laneIdx >= owner.lanes_.size()) return;
        auto& lane    = owner.lanes_.getReference (laneIdx);
        auto& runtime = owner.laneRuntime_.getReference (laneIdx);

        /* Click on the label column = arm toggle for audio lanes;
         * no-op for tracker lanes (tracker arming is a Phase 4 item). */
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

        /* Orphan lanes don't dispatch. */
        if (runtime.isOrphan()) return;

        const double beat = (e.x - kLabelW) / (double) kPxPerBeat;
        const auto& regions = lane.playlist.regions();
        for (const auto& r : regions)
        {
            if (! r.containsBeat (beat)) continue;

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
                /* Audio click-launch = immediate.  beatTarget=-1
                 * fires on the next render block. */
                runtime.audioClipCache->schedulePlay (
                    r.id, r.sourceId, -1.0, 0 /*sampleOffset*/);
            }
            runtime.lastDispatchedRegion  = r.id;
            runtime.lastDispatchedSeqIdx  = r.sequenceIdx;
            repaintLane (laneIdx);
            return;
        }
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

    static constexpr int kLabelW    = 160;
    static constexpr int kLaneH     = 64;
    static constexpr int kPxPerBeat = 24;

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
            g.setColour (fill.brighter (0.4f));
            g.drawRect (rect, 1);
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

    ArrangementView& owner;

    /* Drop hover state for visual feedback during external file drag. */
    bool dropHover_         = false;
    int  dropHoverLaneIdx_  = -1;
};

/* ===================================================================== */

ArrangementView::ArrangementView()
{
    setName (EL_VIEW_ARRANGEMENT);

    addAndMakeVisible (rescanBtn_);
    addAndMakeVisible (addAudioBtn_);
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

    rescanBtn_.onClick   = [this]() { rescanLaneTargets(); };
    addAudioBtn_.onClick = [this]() { createEmptyAudioLane (true /*stereo*/); };
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
    rescanBtn_  .setBounds (top.removeFromLeft (56)); top.removeFromLeft (6);
    addAudioBtn_.setBounds (top.removeFromLeft (72)); top.removeFromLeft (12);
    bpmLabel_   .setBounds (top.removeFromLeft (96)); top.removeFromLeft (4);
    posLabel_   .setBounds (top.removeFromLeft (96));
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
    return dynamic_cast<AudioClipNode*> (target.getObject());
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
    if (services_ == nullptr) return -1;
    auto sess = services_->context().session();
    if (sess == nullptr) return -1;
    auto* engineService = services_->find<EngineService>();
    if (engineService == nullptr) return -1;

    Node subgraph = ArrangementTracksService::findOrCreateSubgraph (*engineService, *sess);
    if (! subgraph.isValid()) return -1;

    Node clip = ArrangementTracksService::addAudioClipNode (*engineService, subgraph, stereo);
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
