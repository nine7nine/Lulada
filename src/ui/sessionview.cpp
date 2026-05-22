// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/sessionview.hpp"

#include <element/audioengine.hpp>
#include <element/context.hpp>
#include <element/node.hpp>
#include <element/session.hpp>
#include <element/tags.hpp>
#include <element/ui/style.hpp>

#include "nodes/tracker.hpp"
#include "nodes/trackereditor.hpp"

namespace element {

using juce::Colour;
using juce::Graphics;
using juce::MouseEvent;
using juce::Point;
using juce::Rectangle;
using juce::String;
using juce::Uuid;

/* === Tracker-style palette (mirrors src/nodes/trackereditor.cpp) ======== */
namespace {

constexpr float kCellFontSize     = 12.0f;
constexpr float kHeaderFontSize   = 11.0f;
constexpr float kLabelFontSize    = 11.0f;

const Colour kBgColour          { 0xff'18'18'18 };
const Colour kGutterColour      { 0xff'14'14'14 };
const Colour kHeaderBgColour    { 0xff'1a'1a'1a };
const Colour kEmptyCellColour   { 0xff'24'24'24 };
const Colour kCellOutlineColour { 0xff'33'33'33 };
const Colour kRowTextColour     { 0xff'6a'6a'6a };
const Colour kLabelTextColour   { 0xff'a0'a0'a0 };
const Colour kPlayheadAccent    { 0xff'ff'a0'40 };  // amber

/* Column-tint palette, cycling — same hues as trackereditor's track
 * tints so a column on the session view feels visually adjacent to
 * its tracker. */
const Colour kColumnTints[] = {
    Colour { 0xff'c5'5a'5a }, // red
    Colour { 0xff'c5'8a'4a }, // orange
    Colour { 0xff'b5'b5'4a }, // yellow
    Colour { 0xff'6a'b5'5a }, // green
    Colour { 0xff'4a'a5'b5 }, // cyan
    Colour { 0xff'5a'7a'c5 }, // blue
    Colour { 0xff'9a'5a'c5 }, // purple
    Colour { 0xff'c5'5a'9a }, // pink
};

inline Colour columnTint (int idx)
{
    return kColumnTints[((unsigned) idx)
                        % (sizeof (kColumnTints) / sizeof (kColumnTints[0]))];
}

/* Walk the active graph (recursing into subgraphs) collecting every
 * TrackerNode + its display name.  Mirrors the pattern used by
 * arrangementview.cpp::collectTrackersFromGraph. */
void collectTrackerNodes (const Node& graph,
                          juce::Array<TrackerNode*>& outTrackers,
                          juce::Array<juce::uint32>& outNodeIds,
                          juce::Array<String>& outNames)
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
                outTrackers.add (t);
                outNodeIds.add (child.getNodeId());
                outNames.add (child.getName().isNotEmpty() ? child.getName()
                                                           : String ("Tracker"));
                continue;
            }
        }
        if (child.isGraph())
            collectTrackerNodes (child, outTrackers, outNodeIds, outNames);
    }
}

} // anonymous

/* ===================================================================== */

SessionView::SessionView()
{
    setName (EL_VIEW_SESSION_VIEW);
    setOpaque (true);
    setWantsKeyboardFocus (false);

    /* Top toolbar — global actions the user reaches for often. */
    addAndMakeVisible (stopAllBtn_);
    addAndMakeVisible (rescanBtn_);
    stopAllBtn_.onClick = [this]() { stopAllClips(); };
    rescanBtn_ .onClick = [this]() { rescanColumns(); };

    /* Footer "+" — append a scene.  Bottom-left under the scene
     * label column matches Bitwig's affordance. */
    addAndMakeVisible (addSceneBtn_);
    addSceneBtn_.onClick = [this]() { addScene(); };

    /* Seed with 8 empty scenes so the user always has a target grid
     * to click into; persistence may overwrite this. */
    for (int i = 0; i < 8; ++i)
    {
        SessionScene s;
        s.id   = Uuid();
        s.name = "Scene " + String (i + 1);
        scenes_.add (s);
    }
}

SessionView::~SessionView() = default;

/* === Lifecycle ========================================================= */

void SessionView::initializeView (Services& s)
{
    services_ = &s;
    if (auto* eng = s.context().audio().get())
        monitor_ = eng->getTransportMonitor();
    readFromSession();
}

void SessionView::didBecomeActive()
{
    rescanColumns();
    startTimerHz (30);
}

void SessionView::willBeRemoved()
{
    stopTimer();
    writeToSession();
}

void SessionView::stabilizeContent()
{
    rescanColumns();
}

void SessionView::resized()
{
    /* Toolbar layout — two buttons left-aligned, compact spacing.
     * Geometry helpers compute the rest from getLocalBounds() on
     * demand so we don't have to push it here. */
    auto tb = toolbarBounds().reduced (4, 4);
    stopAllBtn_.setBounds (tb.removeFromLeft (80)); tb.removeFromLeft (4);
    rescanBtn_ .setBounds (tb.removeFromLeft (72));

    addSceneBtn_.setBounds (addSceneButtonBounds().reduced (4, 4));
}

/* === Geometry ========================================================== */

Rectangle<int> SessionView::toolbarBounds() const noexcept
{
    return getLocalBounds().removeFromTop (kToolbarH);
}

Rectangle<int> SessionView::footerBounds() const noexcept
{
    return getLocalBounds().removeFromBottom (kSceneFooterH);
}

Rectangle<int> SessionView::addSceneButtonBounds() const noexcept
{
    return footerBounds().removeFromLeft (kSceneLabelW);
}

Rectangle<int> SessionView::headerRowBounds() const noexcept
{
    auto r = getLocalBounds();
    r.removeFromTop (kToolbarH);
    return r.removeFromTop (kHeaderH);
}

Rectangle<int> SessionView::sceneLabelStripBounds() const noexcept
{
    auto r = getLocalBounds();
    r.removeFromTop (kToolbarH + kHeaderH);
    r.removeFromBottom (kSceneFooterH);
    return r.removeFromLeft (kSceneLabelW);
}

Rectangle<int> SessionView::gridBodyBounds() const noexcept
{
    auto r = getLocalBounds();
    r.removeFromTop (kToolbarH + kHeaderH);
    r.removeFromBottom (kSceneFooterH);
    r.removeFromLeft (kSceneLabelW);
    return r;
}

Rectangle<int> SessionView::cellBounds (int sceneRow, int columnIdx) const noexcept
{
    const auto body = gridBodyBounds();
    return Rectangle<int> (body.getX() + columnIdx * kColW,
                           body.getY() + sceneRow  * kRowH,
                           kColW, kRowH);
}

Rectangle<int> SessionView::sceneLabelBounds (int sceneRow) const noexcept
{
    const auto strip = sceneLabelStripBounds();
    return Rectangle<int> (strip.getX(),
                           strip.getY() + sceneRow * kRowH,
                           strip.getWidth(), kRowH);
}

Rectangle<int> SessionView::columnHeaderBounds (int columnIdx) const noexcept
{
    const auto header = headerRowBounds();
    return Rectangle<int> (header.getX() + kSceneLabelW + columnIdx * kColW,
                           header.getY(),
                           kColW, kHeaderH);
}

Rectangle<int> SessionView::playButtonBounds (int sceneRow, int columnIdx) const noexcept
{
    /* Left ~18px of the inner cell rect — play/stop glyph. */
    auto inner = cellBounds (sceneRow, columnIdx).reduced (2, 2);
    return inner.removeFromLeft (18);
}

Rectangle<int> SessionView::editButtonBounds (int sceneRow, int columnIdx) const noexcept
{
    /* Right ~18px of the inner cell rect — opens tracker pattern popup. */
    auto inner = cellBounds (sceneRow, columnIdx).reduced (2, 2);
    return inner.removeFromRight (18);
}

bool SessionView::hitTestCell (Point<int> p, int& outRow, int& outCol) const noexcept
{
    const auto body = gridBodyBounds();
    if (! body.contains (p)) return false;
    outRow = (p.y - body.getY()) / kRowH;
    outCol = (p.x - body.getX()) / kColW;
    return outRow >= 0 && outRow < scenes_.size()
        && outCol >= 0 && outCol < columns_.size();
}

bool SessionView::hitTestSceneLabel (Point<int> p, int& outRow) const noexcept
{
    const auto strip = sceneLabelStripBounds();
    if (! strip.contains (p)) return false;
    outRow = (p.y - strip.getY()) / kRowH;
    return outRow >= 0 && outRow < scenes_.size();
}

bool SessionView::hitTestPlayButton (Point<int> p, int& outRow, int& outCol) const noexcept
{
    if (! hitTestCell (p, outRow, outCol)) return false;
    return playButtonBounds (outRow, outCol).contains (p);
}

bool SessionView::hitTestEditButton (Point<int> p, int& outRow, int& outCol) const noexcept
{
    if (! hitTestCell (p, outRow, outCol)) return false;
    return editButtonBounds (outRow, outCol).contains (p);
}

/* === Paint ============================================================= */

void SessionView::paint (Graphics& g)
{
    g.fillAll (kBgColour);

    /* Toolbar background — sits above the column-header row. */
    g.setColour (kHeaderBgColour);
    g.fillRect (toolbarBounds());
    g.setColour (kCellOutlineColour);
    g.drawHorizontalLine (toolbarBounds().getBottom() - 1,
                          0.0f, (float) getWidth());

    const auto header = headerRowBounds();
    const auto labels = sceneLabelStripBounds();
    const auto body   = gridBodyBounds();

    /* --- Header row (column names) --- */
    g.setColour (kHeaderBgColour);
    g.fillRect (header);

    g.setColour (kGutterColour);
    g.fillRect (Rectangle<int> (header.getX(), header.getY(),
                                kSceneLabelW, header.getHeight()));

    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                  kHeaderFontSize, juce::Font::bold));

    for (int c = 0; c < columns_.size(); ++c)
    {
        const auto h    = columnHeaderBounds (c);
        const auto tint = columnTint (c);

        /* Top tint band, à la trackereditor track header. */
        g.setColour (tint);
        g.fillRect (h.getX(), h.getY(), h.getWidth() - 1, 4);

        /* Body of header — translucent tint over header bg. */
        g.setColour (tint.withAlpha (0.10f));
        g.fillRect (h.getX(), h.getY() + 4, h.getWidth() - 1, h.getHeight() - 4);

        /* Column name. */
        g.setColour (tint);
        g.drawText (columns_.getReference (c).name,
                    h.reduced (8, 4),
                    juce::Justification::centredLeft, true);
    }

    /* --- Scene label strip (left column) --- */
    g.setColour (kGutterColour);
    g.fillRect (labels);

    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                  kLabelFontSize, juce::Font::plain));

    for (int r = 0; r < scenes_.size(); ++r)
    {
        const auto sb = sceneLabelBounds (r);

        /* Subtle alternating row tint, matching trackereditor's beat
         * highlight rhythm — every 4th row gets a faint stripe. */
        if ((r & 3) == 0)
        {
            g.setColour (Colour { 0xff'1f'1f'1f });
            g.fillRect (sb);
        }

        g.setColour (kLabelTextColour);
        g.drawText (scenes_.getReference (r).name,
                    sb.reduced (8, 0),
                    juce::Justification::centredLeft, true);

        /* Row divider. */
        g.setColour (kCellOutlineColour);
        g.drawHorizontalLine (sb.getBottom() - 1,
                              (float) sb.getX(),
                              (float) sb.getRight());
    }

    /* --- Grid body (cells) --- */
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                  kCellFontSize, juce::Font::plain));

    for (int r = 0; r < scenes_.size(); ++r)
    {
        for (int c = 0; c < columns_.size(); ++c)
        {
            const auto cb    = cellBounds (r, c);
            const auto inner = cb.reduced (2, 2);

            SessionClip* clip = findClip (r, c);
            if (clip == nullptr)
            {
                /* Empty cell — flat dark with thin outline. */
                g.setColour (kEmptyCellColour);
                g.fillRect (inner);
                g.setColour (kCellOutlineColour);
                g.drawRect (inner, 1);
                continue;
            }

            const bool playing = clip->state.load (std::memory_order_relaxed)
                                 == LiveState::Playing;

            /* Fill body with the clip's colour, brightened when playing. */
            const Colour fill = playing ? clip->color.brighter (0.25f)
                                        : clip->color.withMultipliedAlpha (0.85f);
            g.setColour (fill);
            g.fillRect (inner);

            const auto playR = playButtonBounds (r, c);
            const auto editR = editButtonBounds (r, c);

            /* --- Play / stop button (LEFT) --- */
            g.setColour (juce::Colours::black.withAlpha (0.20f));
            g.fillRect (playR);
            g.setColour (juce::Colours::black.withAlpha (0.85f));
            if (playing)
            {
                /* Square stop glyph. */
                g.fillRect (playR.reduced (5, 7));
            }
            else
            {
                /* Right-pointing play triangle. */
                juce::Path p;
                const float gx = (float) playR.getX() + 5.0f;
                const float gy = (float) playR.getY() + 7.0f;
                const float gh = (float) playR.getHeight() - 14.0f;
                const float gw = gh * 0.866f;
                p.addTriangle (gx,      gy,
                               gx,      gy + gh,
                               gx + gw, gy + gh * 0.5f);
                g.fillPath (p);
            }

            /* --- Edit button (RIGHT) — three horizontal lines as a
             * tracker-pattern hint glyph. */
            g.setColour (juce::Colours::black.withAlpha (0.18f));
            g.fillRect (editR);
            g.setColour (juce::Colours::black.withAlpha (0.80f));
            {
                const int cx = editR.getCentreX();
                const int cy = editR.getCentreY();
                const int lineW = 9;
                const int xL = cx - lineW / 2;
                const int xR = cx + lineW / 2;
                g.drawHorizontalLine (cy - 4, (float) xL, (float) xR);
                g.drawHorizontalLine (cy,     (float) xL, (float) xR);
                g.drawHorizontalLine (cy + 4, (float) xL, (float) xR);
            }

            /* --- Clip name (middle, between the two buttons) --- */
            const auto nameR = inner.withTrimmedLeft (playR.getWidth())
                                    .withTrimmedRight (editR.getWidth());
            g.setColour (juce::Colours::black.withAlpha (0.80f));
            g.drawText (clip->name,
                        nameR.reduced (4, 0),
                        juce::Justification::centredLeft, true);

            /* Position bar across the bottom when playing. */
            if (playing)
            {
                if (auto* trk = lookupTracker (clip->trackerNodeId))
                {
                    const int   total = juce::jmax (1, trk->getSequenceLengthRows (clip->sequenceIdx));
                    const double pos  = trk->getSequencePositionRows (clip->sequenceIdx);
                    const float frac  = (float) juce::jlimit (0.0, 1.0, pos / (double) total);
                    const int barW    = juce::jmax (1, (int) (frac * cb.getWidth()));
                    g.setColour (kPlayheadAccent.withAlpha (0.85f));
                    g.fillRect (cb.getX() + 2,
                                cb.getBottom() - 4,
                                barW - 4,
                                2);
                }
            }

            /* Outline. */
            g.setColour (kCellOutlineColour);
            g.drawRect (inner, 1);
        }
    }

    /* Footer hint strip — right of the "+ Scene" button area. */
    const auto footer = footerBounds();
    g.setColour (kHeaderBgColour);
    g.fillRect (footer);
    g.setColour (kCellOutlineColour);
    g.drawHorizontalLine (footer.getY(), 0.0f, (float) getWidth());

    g.setColour (kRowTextColour);
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                  kLabelFontSize, juce::Font::plain));
    String hint;
    if (columns_.isEmpty())
        hint = "Add a Tracker node to the graph to populate columns";
    else
        hint = String (columns_.size()) + " column"  + (columns_.size() == 1 ? "" : "s")
             + " · "
             + String (scenes_.size())  + " scene"  + (scenes_.size()  == 1 ? "" : "s")
             + " · right-click a cell or scene label for more";
    const auto hintR = footer.withTrimmedLeft (kSceneLabelW + 8);
    g.drawText (hint, hintR, juce::Justification::centredLeft, true);
}

/* === Mouse ============================================================= */

void SessionView::mouseDown (const MouseEvent& e)
{
    int row = -1, col = -1;

    if (e.mods.isPopupMenu())
    {
        if (hitTestCell (e.getPosition(), row, col))
        {
            if (auto* clip = findClip (row, col))
            {
                juce::PopupMenu m;
                m.addItem (1, "Delete clip");
                const int r = m.showAt (Rectangle<int> (e.getScreenX(), e.getScreenY(), 1, 1));
                if (r == 1) deleteClip (*clip);
            }
            else if (col < columns_.size() && lookupTracker (columns_.getReference (col).trackerNodeId) != nullptr)
            {
                juce::PopupMenu m;
                m.addItem (1, "Add Tracker pattern");
                const int r = m.showAt (Rectangle<int> (e.getScreenX(), e.getScreenY(), 1, 1));
                if (r == 1) addClipAt (row, col);
            }
            return;
        }

        if (hitTestSceneLabel (e.getPosition(), row))
        {
            juce::PopupMenu m;
            m.addItem (1, "Launch scene");
            m.addSeparator();
            m.addItem (2, "Insert scene above");
            m.addItem (3, "Insert scene below");
            m.addSeparator();
            const bool canDelete = scenes_.size() > 1;
            m.addItem (4, "Delete scene", canDelete);
            const int r = m.showAt (Rectangle<int> (e.getScreenX(), e.getScreenY(), 1, 1));
            switch (r)
            {
                case 1: bangScene   (row);     break;
                case 2: insertScene (row);     break;
                case 3: insertScene (row + 1); break;
                case 4: deleteScene (row);     break;
                default: break;
            }
        }
        return;
    }

    /* Three click zones on a filled clip cell: edit button (right),
     * play button (left), and the middle name area which is inert.
     * Test edit first, then play; if neither is hit, the click was
     * on the name strip and does nothing. */
    if (hitTestEditButton (e.getPosition(), row, col))
    {
        if (auto* clip = findClip (row, col))
        {
            openPatternEditor (*clip);
            return;
        }
    }

    if (hitTestPlayButton (e.getPosition(), row, col))
    {
        if (auto* clip = findClip (row, col))
        {
            bangClip (*clip);
            return;
        }
    }

    if (hitTestSceneLabel (e.getPosition(), row))
    {
        bangScene (row);
    }
}

/* === Engine I/O ======================================================== */

SessionView::SessionClip* SessionView::findClip (int sceneRow, int columnIdx) const
{
    for (auto* c : clips_)
        if (c->sceneRow == sceneRow && c->columnIdx == columnIdx)
            return c;
    return nullptr;
}

TrackerNode* SessionView::lookupTracker (juce::uint32 nodeId) const
{
    if (services_ == nullptr) return nullptr;
    auto sess = services_->context().session();
    if (sess == nullptr) return nullptr;
    auto graph = sess->getActiveGraph();
    if (! graph.isValid()) return nullptr;
    Node n = graph.getNodeById (nodeId);
    if (! n.isValid()) return nullptr;
    return dynamic_cast<TrackerNode*> (n.getObject());
}

void SessionView::bangClip (SessionClip& clip)
{
    auto* trk = lookupTracker (clip.trackerNodeId);
    if (trk == nullptr || clip.sequenceIdx < 0) return;

    const bool wasPlaying = trk->isSequencePlaying (clip.sequenceIdx);
    trk->setSequencePlaying (clip.sequenceIdx, ! wasPlaying);
    clip.state.store (wasPlaying ? LiveState::Stopped : LiveState::Playing,
                      std::memory_order_relaxed);
    repaint (cellBounds (clip.sceneRow, clip.columnIdx));
}

void SessionView::bangScene (int sceneRow)
{
    if (sceneRow < 0 || sceneRow >= scenes_.size()) return;
    for (auto* c : clips_)
        if (c->sceneRow == sceneRow)
            bangClip (*c);
}

void SessionView::stopAllClips()
{
    for (auto* c : clips_)
    {
        if (auto* trk = lookupTracker (c->trackerNodeId))
            trk->setSequencePlaying (c->sequenceIdx, false);
        c->state.store (LiveState::Stopped, std::memory_order_relaxed);
    }
    repaint();
}

void SessionView::addScene()
{
    SessionScene s;
    s.id   = Uuid();
    s.name = "Scene " + String (scenes_.size() + 1);
    scenes_.add (s);
    writeToSession();
    repaint();
}

void SessionView::insertScene (int beforeRow)
{
    const int idx = juce::jlimit (0, scenes_.size(), beforeRow);
    SessionScene s;
    s.id   = Uuid();
    s.name = "Scene " + String (scenes_.size() + 1);  // numeric name from total
    scenes_.insert (idx, s);

    /* Shift every clip on row >= idx down by 1. */
    for (auto* c : clips_)
        if (c->sceneRow >= idx)
            ++c->sceneRow;

    writeToSession();
    repaint();
}

void SessionView::deleteScene (int row)
{
    if (row < 0 || row >= scenes_.size()) return;
    if (scenes_.size() <= 1) return;  // always keep one scene

    /* Stop + delete every clip on this row. */
    for (int i = clips_.size(); --i >= 0;)
    {
        if (clips_[i]->sceneRow == row)
        {
            if (auto* trk = lookupTracker (clips_[i]->trackerNodeId))
                trk->setSequencePlaying (clips_[i]->sequenceIdx, false);
            clips_.remove (i);
        }
    }

    /* Shift remaining clips above the deleted row up by 1. */
    for (auto* c : clips_)
        if (c->sceneRow > row)
            --c->sceneRow;

    scenes_.remove (row);
    writeToSession();
    repaint();
}

void SessionView::addClipAt (int sceneRow, int columnIdx)
{
    if (sceneRow < 0 || sceneRow >= scenes_.size())   return;
    if (columnIdx < 0 || columnIdx >= columns_.size()) return;
    if (findClip (sceneRow, columnIdx) != nullptr)     return;  // already populated

    auto& col = columns_.getReference (columnIdx);
    auto* trk = lookupTracker (col.trackerNodeId);
    if (trk == nullptr) return;

    const int seqIdx = trk->createSequence (16);
    if (seqIdx < 0) return;

    auto* clip = new SessionClip();
    clip->id   = Uuid();
    clip->name = "Clip " + String (sceneRow + 1);
    clip->color = columnTint (columnIdx).withMultipliedSaturation (0.6f)
                                        .withMultipliedBrightness (0.85f);
    clip->trackerNodeId = col.trackerNodeId;
    clip->sequenceIdx   = seqIdx;
    clip->sceneRow      = sceneRow;
    clip->columnIdx     = columnIdx;
    clips_.add (clip);

    writeToSession();
    repaint (cellBounds (sceneRow, columnIdx));
}

void SessionView::openPatternEditor (SessionClip& clip)
{
    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    auto graph = sess->getActiveGraph();
    if (! graph.isValid()) return;
    Node n = graph.getNodeById (clip.trackerNodeId);
    if (! n.isValid()) return;
    if (dynamic_cast<TrackerNode*> (n.getObject()) == nullptr) return;

    auto* editor = new TrackerEditor (n);
    editor->setSize (820, 540);

    /* Jump to the clip's sequence index — TrackerEditor exposes only
     * a relative switchPattern (delta), so compute the offset from
     * its current pattern.  Clamp against the live pattern count to
     * avoid jumping past the end if the sequence vanished. */
    if (clip.sequenceIdx >= 0)
    {
        const int curr  = editor->getPatternIndex();
        const int total = editor->getPatternCount();
        const int target = juce::jlimit (0, juce::jmax (0, total - 1), clip.sequenceIdx);
        const int delta = target - curr;
        if (delta != 0)
            editor->switchPattern (delta);
    }

    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned (editor);
    opts.dialogTitle = "Tracker — " + clip.name;
    opts.dialogBackgroundColour = Colour { 0xff'18'18'18 };
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = true;
    opts.launchAsync();
}

void SessionView::deleteClip (SessionClip& clip)
{
    if (auto* trk = lookupTracker (clip.trackerNodeId))
        trk->setSequencePlaying (clip.sequenceIdx, false);

    /* Note: we do NOT call trk->removeSequence() — the underlying vht
     * sequence may be referenced by other clips or by the arrangement
     * view.  Phase 3 keeps the sequence behind; Phase 7+ adds a
     * "delete unreferenced sequences" sweep. */

    const int row = clip.sceneRow;
    const int col = clip.columnIdx;

    for (int i = clips_.size(); --i >= 0;)
        if (clips_[i] == &clip)
        {
            clips_.remove (i);
            break;
        }

    writeToSession();
    repaint (cellBounds (row, col));
}

/* === Topology ========================================================== */

void SessionView::rescanColumns()
{
    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    auto graph = sess->getActiveGraph();
    if (! graph.isValid()) return;

    juce::Array<TrackerNode*>  trackers;
    juce::Array<juce::uint32>  nodeIds;
    juce::Array<String>        names;
    collectTrackerNodes (graph, trackers, nodeIds, names);

    /* Build a fresh column list keyed by trackerNodeId.  Preserve any
     * persisted column ordering for nodes that still exist; append
     * new ones at the end. */
    juce::Array<SessionColumn> fresh;
    juce::Array<juce::uint32>  seen;

    for (const auto& existing : columns_)
    {
        if (nodeIds.contains (existing.trackerNodeId))
        {
            fresh.add (existing);
            seen.add (existing.trackerNodeId);
            /* Refresh name in case the node was renamed. */
            const int idx = nodeIds.indexOf (existing.trackerNodeId);
            if (idx >= 0)
                fresh.getReference (fresh.size() - 1).name = names[idx];
        }
    }

    for (int i = 0; i < nodeIds.size(); ++i)
    {
        if (seen.contains (nodeIds[i])) continue;
        SessionColumn col;
        col.id   = Uuid();
        col.name = names[i];
        col.trackerNodeId = nodeIds[i];
        fresh.add (col);
    }

    bool changed = (fresh.size() != columns_.size());
    if (! changed)
        for (int i = 0; i < fresh.size(); ++i)
            if (fresh.getReference (i).trackerNodeId != columns_.getReference (i).trackerNodeId
                || fresh.getReference (i).name      != columns_.getReference (i).name)
                { changed = true; break; }

    if (changed)
    {
        /* Drop clips whose column went away.  Rebind columnIdx for
         * clips whose column was reordered. */
        for (int i = clips_.size(); --i >= 0;)
        {
            int newCol = -1;
            for (int j = 0; j < fresh.size(); ++j)
                if (fresh.getReference (j).trackerNodeId == clips_[i]->trackerNodeId)
                    { newCol = j; break; }
            if (newCol < 0)
                clips_.remove (i);
            else
                clips_[i]->columnIdx = newCol;
        }
        columns_.swapWith (fresh);
        writeToSession();
        repaint();
    }
}

/* === Persistence ======================================================= */

juce::ValueTree SessionView::getOrCreateSessionViewTree()
{
    if (services_ == nullptr) return {};
    auto sess = services_->context().session();
    if (sess == nullptr) return {};
    return sess->data().getOrCreateChildWithName (tags::sessionView, nullptr);
}

void SessionView::readFromSession()
{
    const auto tree = getOrCreateSessionViewTree();
    if (! tree.isValid()) return;

    /* Columns. */
    columns_.clearQuick();
    const auto colsTree = tree.getChildWithName ("columns");
    if (colsTree.isValid())
    {
        for (int i = 0; i < colsTree.getNumChildren(); ++i)
        {
            const auto cn = colsTree.getChild (i);
            SessionColumn col;
            col.id            = Uuid (cn.getProperty ("id").toString());
            col.name          = cn.getProperty ("name", String());
            col.trackerNodeId = (juce::uint32) (juce::int64) cn.getProperty ("trackerNodeId", 0);
            columns_.add (col);
        }
    }

    /* Scenes. */
    const auto scenesTree = tree.getChildWithName ("scenes");
    if (scenesTree.isValid() && scenesTree.getNumChildren() > 0)
    {
        scenes_.clearQuick();
        for (int i = 0; i < scenesTree.getNumChildren(); ++i)
        {
            const auto sn = scenesTree.getChild (i);
            SessionScene sc;
            sc.id    = Uuid (sn.getProperty ("id").toString());
            sc.name  = sn.getProperty ("name", "Scene " + String (i + 1));
            sc.color = Colour::fromString (sn.getProperty ("color", "ff303030").toString());
            scenes_.add (sc);
        }
    }

    /* Clips.  Absent `kind` attribute means tracker (see
     * project_session_view_polymorphic_clip_sources memory). */
    clips_.clearQuick (true);
    const auto clipsTree = tree.getChildWithName ("clips");
    if (clipsTree.isValid())
    {
        for (int i = 0; i < clipsTree.getNumChildren(); ++i)
        {
            const auto cn = clipsTree.getChild (i);
            const String kind = cn.getProperty ("kind", "tracker").toString();
            if (kind != "tracker") continue;   // forward-compat: ignore unknown kinds

            auto* clip = new SessionClip();
            clip->id            = Uuid (cn.getProperty ("id").toString());
            clip->name          = cn.getProperty ("name", String ("Clip"));
            clip->color         = Colour::fromString (cn.getProperty ("color", "ff4a7ab5").toString());
            clip->trackerNodeId = (juce::uint32) (juce::int64) cn.getProperty ("trackerNodeId", 0);
            clip->sequenceIdx   = (int) cn.getProperty ("sequenceIdx", -1);
            clip->sceneRow      = (int) cn.getProperty ("sceneRow", 0);
            clip->columnIdx     = (int) cn.getProperty ("columnIdx", 0);
            clips_.add (clip);
        }
    }
}

void SessionView::writeToSession()
{
    auto tree = getOrCreateSessionViewTree();
    if (! tree.isValid()) return;

    tree.removeAllChildren (nullptr);

    /* Columns. */
    juce::ValueTree colsTree ("columns");
    for (const auto& c : columns_)
    {
        juce::ValueTree cn ("column");
        cn.setProperty ("id",            c.id.toString(),               nullptr);
        cn.setProperty ("name",          c.name,                        nullptr);
        cn.setProperty ("trackerNodeId", (juce::int64) c.trackerNodeId, nullptr);
        colsTree.appendChild (cn, nullptr);
    }
    tree.appendChild (colsTree, nullptr);

    /* Scenes. */
    juce::ValueTree scenesTree ("scenes");
    for (const auto& s : scenes_)
    {
        juce::ValueTree sn ("scene");
        sn.setProperty ("id",    s.id.toString(),         nullptr);
        sn.setProperty ("name",  s.name,                  nullptr);
        sn.setProperty ("color", s.color.toString(),      nullptr);
        scenesTree.appendChild (sn, nullptr);
    }
    tree.appendChild (scenesTree, nullptr);

    /* Clips. */
    juce::ValueTree clipsTree ("clips");
    for (auto* clip : clips_)
    {
        juce::ValueTree cn ("clip");
        cn.setProperty ("id",            clip->id.toString(),              nullptr);
        cn.setProperty ("name",          clip->name,                       nullptr);
        cn.setProperty ("color",         clip->color.toString(),           nullptr);
        cn.setProperty ("trackerNodeId", (juce::int64) clip->trackerNodeId, nullptr);
        cn.setProperty ("sequenceIdx",   clip->sequenceIdx,                nullptr);
        cn.setProperty ("sceneRow",      clip->sceneRow,                   nullptr);
        cn.setProperty ("columnIdx",     clip->columnIdx,                  nullptr);
        clipsTree.appendChild (cn, nullptr);
    }
    tree.appendChild (clipsTree, nullptr);
}

/* === 30 Hz state poll ================================================== */

void SessionView::timerCallback()
{
    if (! isShowing()) return;   // gated per feedback_gui_must_stay_fast

    ++pulsePhase_;

    for (auto* clip : clips_)
    {
        if (clip->sequenceIdx < 0) continue;
        auto* trk = lookupTracker (clip->trackerNodeId);
        if (trk == nullptr) continue;

        const bool playing = trk->isSequencePlaying (clip->sequenceIdx);
        const LiveState observed = playing ? LiveState::Playing : LiveState::Stopped;
        const LiveState prev = clip->state.exchange (observed, std::memory_order_relaxed);

        const int posRow = playing
            ? (int) trk->getSequencePositionRows (clip->sequenceIdx)
            : -1;

        if (prev != observed || posRow != clip->lastDrawnPosRow)
        {
            clip->lastDrawnPlaying = playing;
            clip->lastDrawnPosRow  = posRow;
            repaint (cellBounds (clip->sceneRow, clip->columnIdx));
        }
    }
}

} // namespace element
