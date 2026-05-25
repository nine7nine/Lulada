// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/trackersidedock.hpp"
#include "nodes/tracker.hpp"
#include "nodes/trackereditor.hpp"
#include "ui/fontcache.hpp"

#include <element/context.hpp>
#include <element/session.hpp>
#include <element/processor.hpp>
#include <element/graph.hpp>

namespace element {

/* Left-edge drag handle.  Horizontal-only resize -- forwards the
 * pixel delta to the dock's onResizeDrag callback so StandardContent
 * owns the width field + layout invalidation. */
class TrackerSideDock::DragHandle : public juce::Component
{
public:
    DragHandle (TrackerSideDock& d) : dock_ (d)
    {
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    }

    void paint (juce::Graphics& g) override
    {
        /* Match the SmartLayoutResizeBar visual cue, rotated for the
         * vertical orientation: faint vertical line + 3 stacked dots
         * so the user spots the affordance. */
        g.fillAll (juce::Colour (0xff'1a'1a'1a));
        g.setColour (juce::Colour (0xff'3a'3a'3a));
        g.drawVerticalLine (getWidth() / 2,
                              0.0f, (float) getHeight());
        g.setColour (juce::Colour (0xff'5a'5a'5a));
        const int cx = getWidth() / 2;
        const int cy = getHeight() / 2;
        for (int i = -1; i <= 1; ++i)
            g.fillEllipse ((float) cx - 1.0f,
                            (float) (cy + i * 8) - 1.0f, 2.0f, 2.0f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStartX_ = e.getScreenX();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        /* Positive delta = user dragging RIGHT (dock shrinks).
         * Negative delta = user dragging LEFT (dock widens).
         * StandardContent inverts this for its width state. */
        const int dx = e.getScreenX() - dragStartX_;
        dragStartX_ = e.getScreenX();
        if (dock_.onResizeDrag) dock_.onResizeDrag (dx);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dock_.onResizeDragEnd) dock_.onResizeDragEnd();
    }

private:
    TrackerSideDock& dock_;
    int dragStartX_ { 0 };
};

TrackerSideDock::TrackerSideDock (Services& services)
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
    closeBtn_.setTooltip ("Hide tracker dock");
    closeBtn_.onClick = [this]() {
        if (onCloseClicked) onCloseClicked();
    };

    refreshFromGraph();
}

TrackerSideDock::~TrackerSideDock()
{
    if (watchedNodes_.isValid())
        watchedNodes_.removeListener (this);
}

void TrackerSideDock::attachToActiveGraph()
{
    juce::ValueTree desired;
    if (auto sess = services_.context().session())
        desired = sess->getActiveGraph().getNodesValueTree();

    if (desired == watchedNodes_) return;

    if (watchedNodes_.isValid())
        watchedNodes_.removeListener (this);
    watchedNodes_ = desired;
    if (watchedNodes_.isValid())
        watchedNodes_.addListener (this);
}

void TrackerSideDock::valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&)
{
    juce::Component::SafePointer<TrackerSideDock> sp (this);
    juce::MessageManager::callAsync ([sp]() {
        if (sp != nullptr) sp->refreshFromGraph();
    });
}

void TrackerSideDock::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int)
{
    juce::Component::SafePointer<TrackerSideDock> sp (this);
    juce::MessageManager::callAsync ([sp]() {
        if (sp != nullptr) sp->refreshFromGraph();
    });
}

void TrackerSideDock::refreshFromGraph()
{
    attachToActiveGraph();

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

    if (! boundId_.isNull())
    {
        for (int i = 0; i < g.getNumNodes(); ++i)
        {
            const auto n = g.getNode (i);
            if (n.getUuid() == boundId_)
            {
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

void TrackerSideDock::setTracker (const juce::Uuid& nodeId)
{
    if (nodeId == boundId_) return;

    auto sess = services_.context().session();
    if (sess == nullptr) return;
    const auto g = sess->getActiveGraph();

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

    for (int k = 0; k < trackerCombo_.getNumItems(); ++k)
    {
        if (trackerCombo_.getItemText (k) == match.getDisplayName())
        {
            trackerCombo_.setSelectedItemIndex (k, juce::dontSendNotification);
            break;
        }
    }
}

void TrackerSideDock::setTrackerAndPattern (const juce::Uuid& nodeId, int sequenceIdx)
{
    setTracker (nodeId);
    if (sequenceIdx < 0) return;
    if (auto* ed = dynamic_cast<TrackerEditor*> (editor_.get()))
    {
        const int cur  = ed->getPatternIndex();
        const int n    = ed->getPatternCount();
        if (n <= 0) return;
        const int dst  = juce::jlimit (0, n - 1, sequenceIdx);
        ed->switchPattern (dst - cur);
    }
}

void TrackerSideDock::rebuildEditorForBound()
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

    auto ed = std::make_unique<TrackerEditor> (match);
    addAndMakeVisible (ed.get());
    editor_ = std::move (ed);
    resized();
}

void TrackerSideDock::paint (juce::Graphics& g)
{
    /* Header strip background -- matte black faceplate behind the
     * combo + close button.  TrackerEditor paints its own body. */
    g.setColour (juce::Colour (0xff'0c'0c'0c));
    g.fillRect (kDragHandleW, 0, getWidth() - kDragHandleW, kHeaderH);
}

void TrackerSideDock::resized()
{
    auto r = getLocalBounds();
    dragHandle_->setBounds (r.removeFromLeft (kDragHandleW));

    auto header = r.removeFromTop (kHeaderH);
    closeBtn_   .setBounds (header.removeFromRight (kHeaderH).reduced (3));
    header.removeFromRight (4);
    trackerCombo_.setBounds (header.reduced (4, 2));

    if (editor_ != nullptr)
        editor_->setBounds (r);
}

} // namespace element
