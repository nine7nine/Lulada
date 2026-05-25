// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/trackerstripview.hpp"
#include "nodes/tracker.hpp"
#include "nodes/trackereditor.hpp"
#include "ui/fontcache.hpp"

#include <element/context.hpp>
#include <element/session.hpp>
#include <element/processor.hpp>
#include <element/graph.hpp>

namespace element {

/* Top-edge drag handle.  Vertical-only resize -- forwards the pixel
 * delta to the strip's onResizeDrag callback so StandardContent
 * owns the height field + layout invalidation. */
class TrackerStripView::DragHandle : public juce::Component
{
public:
    DragHandle (TrackerStripView& s) : strip_ (s)
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    }

    void paint (juce::Graphics& g) override
    {
        /* Match the SmartLayoutResizeBar visual cue: faint horizontal
         * line + 3 centred dots so the user spots the affordance. */
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
        const int dy = dragStartY_ - e.getScreenY();
        dragStartY_ = e.getScreenY();
        if (strip_.onResizeDrag) strip_.onResizeDrag (dy);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (strip_.onResizeDragEnd) strip_.onResizeDragEnd();
    }

private:
    TrackerStripView& strip_;
    int dragStartY_ { 0 };
};

TrackerStripView::TrackerStripView (Services& services)
    : services_ (services)
{
    dragHandle_ = std::make_unique<DragHandle> (*this);
    addAndMakeVisible (*dragHandle_);

    addAndMakeVisible (trackerCombo_);
    trackerCombo_.setTextWhenNothingSelected ("(no tracker)");
    trackerCombo_.onChange = [this]() {
        const int idx = trackerCombo_.getSelectedItemIndex();
        if (idx < 0) return;
        const auto uuidStr = trackerCombo_.getItemText (idx);
        /* ItemText holds the node name; we keep ItemID == hash of
         * uuid for resolution.  ComboBox doesn't store strings as
         * uuid directly, so we walk the graph to match the display
         * name back to a node (cheap: graphs are small). */
        if (auto sess = services_.context().session())
        {
            const auto g = sess->getActiveGraph();
            for (int i = 0; i < g.getNumNodes(); ++i)
            {
                const auto n = g.getNode (i);
                if (n.getDisplayName() == uuidStr
                    && n.getObject() != nullptr
                    && dynamic_cast<TrackerNode*> (n.getObject()) != nullptr)
                {
                    setTracker (n.getUuid());
                    return;
                }
            }
        }
    };

    addAndMakeVisible (closeBtn_);
    closeBtn_.setTooltip ("Hide tracker strip");
    closeBtn_.onClick = [this]() {
        if (onCloseClicked) onCloseClicked();
    };

    refreshFromGraph();
}

TrackerStripView::~TrackerStripView() = default;

void TrackerStripView::refreshFromGraph()
{
    /* Re-populate the selector from the current graph.  Preserve the
     * binding if the previously-bound tracker is still present;
     * otherwise bind to the first tracker we find (or unbind if
     * none). */
    trackerCombo_.clear (juce::dontSendNotification);

    auto sess = services_.context().session();
    if (sess == nullptr)
    {
        boundId_ = juce::Uuid::null();
        editor_.reset();
        return;
    }

    const auto g = sess->getActiveGraph();
    bool boundStillPresent = false;
    juce::Uuid firstTrackerId;
    for (int i = 0; i < g.getNumNodes(); ++i)
    {
        const auto n = g.getNode (i);
        if (n.getObject() == nullptr) continue;
        if (dynamic_cast<TrackerNode*> (n.getObject()) == nullptr) continue;

        const auto name = n.getDisplayName();
        trackerCombo_.addItem (name, (int) trackerCombo_.getNumItems() + 1);

        if (firstTrackerId.isNull())
            firstTrackerId = n.getUuid();
        if (n.getUuid() == boundId_)
            boundStillPresent = true;
    }

    if (! boundStillPresent)
        boundId_ = firstTrackerId;

    /* Select the matching combo entry. */
    if (! boundId_.isNull())
    {
        for (int i = 0; i < g.getNumNodes(); ++i)
        {
            const auto n = g.getNode (i);
            if (n.getUuid() == boundId_)
            {
                trackerCombo_.setSelectedItemIndex (
                    trackerCombo_.indexOfItemId (-1) /* unused */, juce::dontSendNotification);
                /* indexOfItemId is awkward here because ComboBox item
                 * IDs are 1-based and we used getNumItems+1 to assign;
                 * walk the items directly instead. */
                for (int k = 0; k < trackerCombo_.getNumItems(); ++k)
                {
                    if (trackerCombo_.getItemText (k) == n.getDisplayName())
                    {
                        trackerCombo_.setSelectedItemIndex (k, juce::dontSendNotification);
                        break;
                    }
                }
                break;
            }
        }
    }

    rebuildEditorForBound();
}

void TrackerStripView::setTracker (const juce::Uuid& nodeId)
{
    if (nodeId == boundId_) return;

    auto sess = services_.context().session();
    if (sess == nullptr) return;
    const auto g = sess->getActiveGraph();

    /* Verify the uuid resolves to a TrackerNode in the graph. */
    Node match;
    for (int i = 0; i < g.getNumNodes(); ++i)
    {
        const auto n = g.getNode (i);
        if (n.getUuid() == nodeId
            && n.getObject() != nullptr
            && dynamic_cast<TrackerNode*> (n.getObject()) != nullptr)
        {
            match = n;
            break;
        }
    }
    if (! match.isValid()) return;

    boundId_ = nodeId;
    rebuildEditorForBound();

    /* Sync the combo selection without re-firing onChange. */
    for (int k = 0; k < trackerCombo_.getNumItems(); ++k)
    {
        if (trackerCombo_.getItemText (k) == match.getDisplayName())
        {
            trackerCombo_.setSelectedItemIndex (k, juce::dontSendNotification);
            break;
        }
    }
}

void TrackerStripView::setTrackerAndPattern (const juce::Uuid& nodeId, int sequenceIdx)
{
    setTracker (nodeId);
    if (sequenceIdx < 0) return;
    if (auto* ed = dynamic_cast<TrackerEditor*> (editor_.get()))
    {
        /* TrackerEditor only exposes relative switchPattern (delta).
         * Walk by the difference between the requested index and the
         * editor's current index.  Mirrors the same idiom the
         * pattern-jump dialog uses internally (trackereditor.cpp). */
        const int cur  = ed->getPatternIndex();
        const int n    = ed->getPatternCount();
        if (n <= 0) return;
        const int dst  = juce::jlimit (0, n - 1, sequenceIdx);
        ed->switchPattern (dst - cur);
    }
}

void TrackerStripView::rebuildEditorForBound()
{
    if (editor_ != nullptr)
    {
        removeChildComponent (editor_.get());
        editor_.reset();
    }

    if (boundId_.isNull()) return;

    auto sess = services_.context().session();
    if (sess == nullptr) return;
    const auto g = sess->getActiveGraph();
    Node match;
    for (int i = 0; i < g.getNumNodes(); ++i)
    {
        const auto n = g.getNode (i);
        if (n.getUuid() == boundId_) { match = n; break; }
    }
    if (! match.isValid()) return;

    /* TrackerEditor's ctor takes a Node by const ref.  We own the
     * resulting Component via unique_ptr<Component>. */
    auto ed = std::make_unique<TrackerEditor> (match);
    addAndMakeVisible (ed.get());
    editor_ = std::move (ed);
    resized();
}

void TrackerStripView::paint (juce::Graphics& g)
{
    /* Header strip background -- matte black to read as a faceplate
     * separate from the main content above.  TrackerEditor paints
     * its own body. */
    g.setColour (juce::Colour (0xff'0c'0c'0c));
    g.fillRect (0, 0, getWidth(), kDragHandleH + kHeaderH);
}

void TrackerStripView::resized()
{
    auto r = getLocalBounds();
    dragHandle_->setBounds (r.removeFromTop (kDragHandleH));

    auto header = r.removeFromTop (kHeaderH);
    closeBtn_   .setBounds (header.removeFromRight (kHeaderH).reduced (3));
    header.removeFromRight (4);
    trackerCombo_.setBounds (header.reduced (4, 2));

    if (editor_ != nullptr)
        editor_->setBounds (r);
}

} // namespace element
