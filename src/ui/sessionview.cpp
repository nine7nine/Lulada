// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/sessionview.hpp"

#include <limits>

#include <element/audioengine.hpp>
#include <element/context.hpp>
#include <element/node.hpp>
#include <element/session.hpp>
#include <element/tags.hpp>
#include <element/ui.hpp>
#include <element/ui/style.hpp>
#include <element/ui/standard.hpp>
#include "ui/viewhelpers.hpp"

#include "nodes/tracker.hpp"
#include "nodes/midiplayer.hpp"
#include "services/timeline/midi_note_region.hpp"
#include "ui/fontcache.hpp"
#include "nodes/trackereditor.hpp"
#include "tempo.hpp"   // BeatType::fromDivisor for scene sig overrides
#include <element/engine.hpp>
#include <element/node.h>

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
const Colour kRowTextColour     { 0xff'a8'a8'a8 };   // bumped (was 0x6a6a6a)
const Colour kLabelTextColour   { 0xff'd8'd8'd8 };   // bumped (was 0xa0a0a0)
const Colour kPlayheadAccent    { 0xff'ff'a0'40 };  // amber

/* Column-tint palette, cycling -- same hues as trackereditor's track
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
 * column-bindable source node -- TrackerNode or MidiPlayerNode -- with
 * its kind tag, display name, and node id.  Mirrors the pattern used
 * by arrangementview.cpp::collectTrackersFromGraph.  MidiPlayerNodes
 * that live inside an ArrangementTracks subgraph are skipped: they
 * belong to the arrangement, not the session view.  Top-level
 * MidiPlayerNodes are session-view column sources.  Detection: a node
 * whose parent graph's name matches the arrangement subgraph
 * convention is filtered out; we approximate via "child of root graph
 * only" by NOT recursing into subgraphs.  TrackerNodes are picked up
 * regardless of nesting (matches today's behaviour). */
void collectColumnSourceNodes (const Node&                            graph,
                                juce::Array<juce::uint32>&            outNodeIds,
                                juce::Array<String>&                  outNames,
                                juce::Array<SessionView::ClipKind>&   outKinds,
                                bool                                  isRootLevel = true)
{
    const int n = graph.getNumNodes();
    for (int i = 0; i < n; ++i)
    {
        Node child = graph.getNode (i);
        if (! child.isValid()) continue;
        if (auto* proc = child.getObject())
        {
            if (dynamic_cast<TrackerNode*> (proc) != nullptr)
            {
                outNodeIds.add (child.getNodeId());
                outNames.add (child.getName().isNotEmpty() ? child.getName()
                                                           : String ("Tracker"));
                outKinds.add (SessionView::ClipKind::Tracker);
                continue;
            }
            /* Only root-level MidiPlayerNodes count as session-view
             * columns.  ArrangementView spawns MidiPlayerNodes inside
             * the ArrangementTracks subgraph -- those belong to the
             * arrangement and must not show up as session-view
             * columns. */
            if (isRootLevel && dynamic_cast<MidiPlayerNode*> (proc) != nullptr)
            {
                outNodeIds.add (child.getNodeId());
                outNames.add (child.getName().isNotEmpty() ? child.getName()
                                                           : String ("MIDI"));
                outKinds.add (SessionView::ClipKind::Midi);
                continue;
            }
        }
        if (child.isGraph())
            collectColumnSourceNodes (child, outNodeIds, outNames, outKinds, false);
    }
}

/* Walk the graph recursively + collect every used "prefix N" suffix
 * so we can pick the lowest-free integer for a fresh node.  Mirrors
 * the arrangement-side helper -- one canonical naming scheme across
 * both views so the graph block label matches the column / lane
 * label everywhere. */
void collectUsedNumbersForPrefix (const Node&             graph,
                                  const juce::String&     prefix,
                                  juce::Array<int>&       outUsed)
{
    const int n = graph.getNumNodes();
    for (int i = 0; i < n; ++i)
    {
        Node child = graph.getNode (i);
        if (! child.isValid()) continue;
        const auto name = child.getName();
        if (name.startsWith (prefix + " "))
        {
            const auto tail = name.substring (prefix.length() + 1).trim();
            const int  num  = tail.getIntValue();
            if (num > 0 && tail.containsOnly ("0123456789"))
                outUsed.add (num);
        }
        if (child.isGraph())
            collectUsedNumbersForPrefix (child, prefix, outUsed);
    }
}

juce::String nextNumberedNodeName (const Node&         rootGraph,
                                   const juce::String& prefix)
{
    juce::Array<int> used;
    collectUsedNumbersForPrefix (rootGraph, prefix, used);
    int n = 1;
    while (used.contains (n)) ++n;
    return prefix + " " + juce::String (n);
}

} // anonymous

/* ===================================================================== */

SessionView::SessionView()
{
    setName (EL_VIEW_SESSION_VIEW);
    setOpaque (true);
    setWantsKeyboardFocus (false);

    /* Tracker-editor-styled toolbar.  All actions delegate to
     * SessionView methods; the value labels (scenes count, default
     * quant) refresh via refreshToolbarLabels(), called from the
     * 30 Hz tick on any structural change. */
    configureToolbarButton (stopAllBtn_,     "STOP ALL",
                            [this] { stopAllClips(); });
    configureToolbarButton (sceneMinusBtn_,  "-",
                            [this] { if (scenes_.size() > 1)
                                         deleteScene (scenes_.size() - 1); });
    configureToolbarButton (scenePlusBtn_,   "+",
                            [this] { addScene(); });
    configureToolbarButton (quantPrevBtn_,   "<",
                            [this] { cycleDefaultQuant (-1); });
    configureToolbarButton (quantNextBtn_,   ">",
                            [this] { cycleDefaultQuant ( 1); });
    configureToolbarButton (rescanBtn_,      "RESCAN",
                            [this] { rescanColumns(); });
    configureToolbarButton (addTrackerBtn_,  "+ TRACKER",
                            [this] { addTrackerColumn(); });
    configureToolbarButton (addMidiBtn_,     "+ MIDI",
                            [this] { addMidiColumn(); });

    configureToolbarLabel  (scenesNameLabel_,  "SCENES", false);
    configureToolbarLabel  (scenesValueLabel_, "8",      true);    // editable
    configureToolbarLabel  (quantNameLabel_,   "QUANT",  false);
    configureToolbarLabel  (quantValueLabel_,  "Bar",    true);    // popup on click

    /* Click-to-edit hooks -- match the tracker editor's pattern, but
     * with single-click editing on the SCENES value too (the toolbar
     * is a top-level chrome strip -- double-click-to-edit feels
     * over-cautious here, as user 2026-05-22 found out the hard way).
     * QUANT value: click -> popup menu (typing enum names is awkward;
     * menu is one click). */
    scenesValueLabel_.setEditable (true, true, false);

    /* Inline editor -- one shared TextEditor reused by every editable
     * field on the view (scene tempo, scene sig, future clip inspector).
     * Hidden until showInlineEditor positions + populates it. */
    inlineEditor_.setMultiLine (false);
    inlineEditor_.setReturnKeyStartsNewLine (false);
    inlineEditor_.setSelectAllWhenFocused (true);
    inlineEditor_.setFont (monoFont (
                                              12.0f, juce::Font::plain));
    inlineEditor_.setColour (juce::TextEditor::backgroundColourId,
                             juce::Colour { 0xff'18'18'18 });
    inlineEditor_.setColour (juce::TextEditor::textColourId,
                             juce::Colour { 0xff'ff'ff'ff });
    inlineEditor_.setColour (juce::TextEditor::outlineColourId,
                             juce::Colour { 0xff'40'40'40 });
    inlineEditor_.setColour (juce::TextEditor::focusedOutlineColourId,
                             juce::Colour { 0xff'ff'a0'40 });
    /* Return commits, Escape + focus-loss REVERT (per
     * feedback_inline_edit_cancel_on_click_away memory note).
     * Click-away on a value editor that the user is unsure about
     * should leave the original value alone. */
    inlineEditor_.onReturnKey = [this] {
        if (inlineEditorCommit_) inlineEditorCommit_ (inlineEditor_.getText());
        hideInlineEditor();
    };
    inlineEditor_.onEscapeKey = [this] { hideInlineEditor(); };
    inlineEditor_.onFocusLost = [this] { hideInlineEditor(); };
    addChildComponent (inlineEditor_);
    scenesValueLabel_.onTextChange = [this] {
        const auto txt = scenesValueLabel_.getText()
                            .retainCharacters ("0123456789").trim();
        if (txt.isNotEmpty()) commitScenesCount (txt.getIntValue());
        refreshToolbarLabels();
    };
    quantValueLabel_.onClick = [this] { showQuantMenu(); };

    /* Seed with 8 empty scenes so the user always has a target grid
     * to click into; persistence may overwrite this. */
    for (int i = 0; i < 8; ++i)
    {
        SessionScene s;
        s.id   = Uuid();
        s.name = "Scene " + String (i + 1);
        scenes_.add (s);
    }

    refreshToolbarLabels();
}

void SessionView::configureToolbarButton (juce::TextButton& b,
                                          const juce::String& text,
                                          std::function<void()> onClick)
{
    b.setButtonText (text);
    b.onClick = std::move (onClick);
    /* Same palette as TrackerEditor::Toolbar -- keeps the two views
     * visually adjacent. */
    b.setColour (juce::TextButton::buttonColourId,  juce::Colour { 0xff'2c'2c'2c });
    b.setColour (juce::TextButton::textColourOffId, juce::Colour { 0xff'd0'd0'd0 });
    b.setColour (juce::TextButton::textColourOnId,  juce::Colours::white);
    addAndMakeVisible (b);
}

void SessionView::configureToolbarLabel (juce::Label& l,
                                         const juce::String& text,
                                         bool editable)
{
    l.setText (text, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centred);
    l.setFont (monoFont (
                                  12.0f, editable ? juce::Font::bold : juce::Font::plain));
    l.setColour (juce::Label::textColourId,
                 editable ? juce::Colour { 0xff'ff'ff'ff }
                          : juce::Colour { 0xff'c0'c0'c0 });
    if (editable)
    {
        l.setColour (juce::Label::backgroundColourId,           juce::Colour { 0xff'18'18'18 });
        l.setColour (juce::Label::backgroundWhenEditingColourId,juce::Colour { 0xff'30'30'30 });
        l.setColour (juce::Label::outlineColourId,              juce::Colour { 0xff'40'40'40 });
    }
    addAndMakeVisible (l);
}

void SessionView::refreshToolbarLabels()
{
    scenesValueLabel_.setText (String (scenes_.size()), juce::dontSendNotification);
    quantValueLabel_ .setText (formatLaunchQuant (defaultLaunchQuant_),
                               juce::dontSendNotification);
    sceneMinusBtn_.setEnabled (scenes_.size() > 1);
}

juce::String SessionView::formatLaunchQuant (LaunchQuant q) const noexcept
{
    switch (q)
    {
        case LaunchQuant::Off:      return "Off";
        case LaunchQuant::Beat:     return "Beat";
        case LaunchQuant::Bar:      return "Bar";
        case LaunchQuant::TwoBars:  return "2 Bars";
        case LaunchQuant::FourBars: return "4 Bars";
    }
    return {};
}

void SessionView::cycleDefaultQuant (int delta)
{
    const int n = 5;
    int idx = (int) defaultLaunchQuant_;
    idx = ((idx + delta) % n + n) % n;
    defaultLaunchQuant_ = static_cast<LaunchQuant> (idx);
    refreshToolbarLabels();
    writeToSession();
}

void SessionView::showQuantMenu()
{
    juce::PopupMenu m;
    const auto q = defaultLaunchQuant_;
    m.addItem (10, "Off",     true, q == LaunchQuant::Off);
    m.addItem (11, "1 Beat",  true, q == LaunchQuant::Beat);
    m.addItem (12, "1 Bar",   true, q == LaunchQuant::Bar);
    m.addItem (13, "2 Bars",  true, q == LaunchQuant::TwoBars);
    m.addItem (14, "4 Bars",  true, q == LaunchQuant::FourBars);

    const int r = m.showAt (quantValueLabel_.getScreenBounds());
    if (r >= 10 && r <= 14)
    {
        defaultLaunchQuant_ = static_cast<LaunchQuant> (r - 10);
        refreshToolbarLabels();
        writeToSession();
    }
}

void SessionView::commitScenesCount (int target)
{
    /* Grow / shrink scenes_ to match the requested count.  Shrink
     * deletes from the end via the existing deleteScene path so any
     * clips on doomed rows get stopped + cleaned up properly. */
    target = juce::jlimit (1, 256, target);
    while (scenes_.size() < target) addScene();
    while (scenes_.size() > target) deleteScene (scenes_.size() - 1);
}

SessionView::~SessionView() = default;

/* === Lifecycle ========================================================= */

void SessionView::initializeView (Services& s)
{
    services_ = &s;
    if (auto* eng = s.context().audio().get())
        monitor_ = eng->getTransportMonitor();
    readFromSession();
    /* Seed the undo baseline against the session's on-disk state.
     * Future writeToSession calls diff against this snapshot. */
    lastCommittedSnapshot_ = getOrCreateSessionViewTree().createCopy();
}

void SessionView::didBecomeActive()
{
    rescanColumns();
    reconcileClipPlaying();
    startTimerHz (30);
}

void SessionView::willBeRemoved()
{
    stopTimer();
    /* No writeToSession here -- mutations write through it on every
     * change, so this would be redundant in the steady state.  More
     * importantly, on session reload sigSessionLoaded fires AFTER
     * the context's session pointer swaps to the new session.  The
     * outer cache-clear handler then detaches this view, willBeRemoved
     * runs, and a writeToSession at this point would overwrite the
     * freshly-loaded NEW session's clips_/scenes_/columns_ tree with
     * this OLD view's stale data -- wiping all the user's saved
     * clips.  Bug observed 2026-05-24 with test.sls reload. */
}

void SessionView::stabilizeContent()
{
    rescanColumns();
    reconcileClipPlaying();
}

void SessionView::reconcileClipPlaying()
{
    /* For each column, force any source slot not bound to a Playing /
     * WaitingToStart SessionClip to "not playing":
     *   - Tracker column: walk sequences, force playing=0.
     *   - Midi column: walk clip slots, force schedule-stop.
     *
     * This silences the rogue emit case where a source's playing flag
     * was set without a session-view launch (e.g. legacy saves, future
     * regressions, TrackerEditor experimentation).  Cost: O(columns x
     * sources x clips) per call, but only called on view activate +
     * stabilize -- not in the 30 Hz tick. */
    for (const auto& col : columns_)
    {
        if (col.kind == ClipKind::Tracker)
        {
            auto* trk = lookupTracker (col.trackerNodeId);
            if (trk == nullptr) continue;
            const int nseq = trk->numPatterns();
            for (int seqIdx = 0; seqIdx < nseq; ++seqIdx)
            {
                bool bound = false;
                for (const auto* c : clips_)
                {
                    if (c->kind != ClipKind::Tracker)          continue;
                    if (c->trackerNodeId != col.trackerNodeId) continue;
                    if (c->sequenceIdx   != seqIdx)            continue;
                    const LiveState s = c->state.load (std::memory_order_relaxed);
                    if (s == LiveState::Playing || s == LiveState::WaitingToStart)
                        { bound = true; break; }
                }
                if (! bound && trk->isSequencePlaying (seqIdx))
                    trk->setSequencePlaying (seqIdx, false);
            }
            continue;
        }

        /* Midi column. */
        auto* mp = lookupMidiPlayer (col.midiPlayerNodeId);
        if (mp == nullptr) continue;
        /* MidiPlayerNode's session API exposes a stop-all-unbound entry
         * point: it walks its own slots + drives any not-bound-to-a-
         * launch state back to Stopped.  Slot-side semantics live on
         * the node so the audio thread can observe a consistent state
         * without the message thread enumerating slot indices.  See
         * MidiPlayerNode::reconcileBoundClips. */
        juce::Array<juce::Uuid> boundIds;
        for (const auto* c : clips_)
        {
            if (c->kind != ClipKind::Midi)              continue;
            if (c->midiPlayerNodeId != col.midiPlayerNodeId) continue;
            const LiveState s = c->state.load (std::memory_order_relaxed);
            if (s == LiveState::Playing || s == LiveState::WaitingToStart)
                boundIds.add (c->midiSourceId);
        }
        mp->reconcileBoundClips (boundIds);
    }
}

void SessionView::resized()
{
    /* Toolbar layout -- tracker-editor-styled groups, left-aligned.
     * Each group: [name label] [value label] [- btn] [+ btn].  6 px
     * gutters at the toolbar edges, 8 px gap between groups, 2 px
     * gap within a group. */
    auto tb = toolbarBounds();
    int x  = tb.getX() + 6;
    const int y  = tb.getY() + 4;
    const int h  = tb.getHeight() - 8;
    const int btnW   = 22;
    const int nameW  = 56;
    const int valueW = 64;
    const int gap    = 2;
    const int sep    = 10;

    auto place = [&] (juce::Component& c, int w)
    {
        c.setBounds (x, y, w, h);
        x += w + gap;
    };

    place (stopAllBtn_, 80);
    x += sep;

    place (scenesNameLabel_,  nameW);
    place (scenesValueLabel_, 32);
    place (sceneMinusBtn_,    btnW);
    place (scenePlusBtn_,     btnW);
    x += sep;

    place (quantNameLabel_,   nameW);
    place (quantValueLabel_,  valueW);
    place (quantPrevBtn_,     btnW);
    place (quantNextBtn_,     btnW);
    x += sep;

    place (rescanBtn_,     64);
    x += sep;

    place (addTrackerBtn_, 92);
    place (addMidiBtn_,    72);
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
    r.removeFromRight (kMasterColW);
    return r;
}

Rectangle<int> SessionView::masterColumnBounds() const noexcept
{
    auto r = getLocalBounds();
    r.removeFromTop (kToolbarH + kHeaderH);
    r.removeFromBottom (kSceneFooterH);
    return r.removeFromRight (kMasterColW);
}

Rectangle<int> SessionView::columnNameLabelBounds (int columnIdx) const noexcept
{
    /* Top half of the column header -- track name + tint band. */
    const auto h = columnHeaderBounds (columnIdx);
    return Rectangle<int> (h.getX(), h.getY(), h.getWidth(),
                           h.getHeight() / 2);
}

/* Column header bottom-half layout: two equal-width buttons with
 * a 2 px gap.  MUTE | SOLO.  STOP was dropped (STOP ALL handles the
 * global case; per-clip click handles individual stops; per-column
 * STOP was the redundant affordance). */
static int columnFooterButtonWidth (int innerW) noexcept
{
    return (innerW - 2) / 2;   // 1 gap of 2 px
}

Rectangle<int> SessionView::columnStopButtonBounds (int) const noexcept
{
    /* STOP no longer has a visible affordance in the column header.
     * Returned rect is empty so hit-tests fail and paint is skipped. */
    return {};
}

Rectangle<int> SessionView::columnMuteButtonBounds (int columnIdx) const noexcept
{
    const auto h = columnHeaderBounds (columnIdx);
    const int topH = h.getHeight() / 2;
    const int btnAreaH = h.getHeight() - topH - 2;
    const int btnAreaY = h.getY() + topH;
    const int innerW = h.getWidth() - 4;
    const int w = columnFooterButtonWidth (innerW);
    return Rectangle<int> (h.getX() + 2, btnAreaY, w, btnAreaH);
}

Rectangle<int> SessionView::columnSoloButtonBounds (int columnIdx) const noexcept
{
    const auto h = columnHeaderBounds (columnIdx);
    const int topH = h.getHeight() / 2;
    const int btnAreaH = h.getHeight() - topH - 2;
    const int btnAreaY = h.getY() + topH;
    const int innerW = h.getWidth() - 4;
    const int w = columnFooterButtonWidth (innerW);
    return Rectangle<int> (h.getX() + 2 + w + 2, btnAreaY, w, btnAreaH);
}

Rectangle<int> SessionView::masterCellBounds (int sceneRow) const noexcept
{
    const auto col = masterColumnBounds();
    return Rectangle<int> (col.getX(),
                           col.getY() + sceneRow * kRowH - gridScrollY_,
                           col.getWidth(), kRowH);
}

Rectangle<int> SessionView::masterLaunchButtonBounds (int sceneRow) const noexcept
{
    auto inner = masterCellBounds (sceneRow).reduced (3, 3);
    return inner.removeFromLeft (22);
}

Rectangle<int> SessionView::masterTempoFieldBounds (int sceneRow) const noexcept
{
    /* Master cell layout: [launch] [tempo field] [sig field] [edit].
     * Reserve 22 px each for launch (left) and edit (right) with
     * 4 px gaps; tempo + sig split the rest 55/45 (BPM strings are
     * typically wider). */
    auto inner = masterCellBounds (sceneRow).reduced (3, 3);
    inner.removeFromLeft  (22 + 4);
    inner.removeFromRight (22 + 4);
    const int remaining = inner.getWidth();
    const int tempoW = (remaining - 4) * 11 / 20;
    return inner.removeFromLeft (tempoW);
}

Rectangle<int> SessionView::masterSigFieldBounds (int sceneRow) const noexcept
{
    auto inner = masterCellBounds (sceneRow).reduced (3, 3);
    inner.removeFromLeft  (22 + 4);
    inner.removeFromRight (22 + 4);
    const int remaining = inner.getWidth();
    const int tempoW = (remaining - 4) * 11 / 20;
    inner.removeFromLeft (tempoW + 4);
    return inner;
}

Rectangle<int> SessionView::masterEditButtonBounds (int sceneRow) const noexcept
{
    /* Rightmost 22 px of the master cell -- opens the Scene View
     * popup, mirroring the clip cell's edit button which opens the
     * tracker pattern editor. */
    auto inner = masterCellBounds (sceneRow).reduced (3, 3);
    return inner.removeFromRight (22);
}

Rectangle<int> SessionView::cellBounds (int sceneRow, int columnIdx) const noexcept
{
    /* gridScrollY_ shifts all rows up; off-screen cells get a y
     * outside gridBodyBounds and naturally fail hit-tests / clip
     * out of the paint region. */
    const auto body = gridBodyBounds();
    return Rectangle<int> (body.getX() + columnIdx * kColW,
                           body.getY() + sceneRow  * kRowH - gridScrollY_,
                           kColW, kRowH);
}

Rectangle<int> SessionView::sceneLabelBounds (int sceneRow) const noexcept
{
    const auto strip = sceneLabelStripBounds();
    return Rectangle<int> (strip.getX(),
                           strip.getY() + sceneRow * kRowH - gridScrollY_,
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
    /* Left ~18px of the inner cell rect -- play/stop glyph. */
    auto inner = cellBounds (sceneRow, columnIdx).reduced (2, 2);
    return inner.removeFromLeft (18);
}

Rectangle<int> SessionView::editButtonBounds (int sceneRow, int columnIdx) const noexcept
{
    /* Right ~18px of the inner cell rect -- opens tracker pattern popup. */
    auto inner = cellBounds (sceneRow, columnIdx).reduced (2, 2);
    return inner.removeFromRight (18);
}

bool SessionView::hitTestCell (Point<int> p, int& outRow, int& outCol) const noexcept
{
    const auto body = gridBodyBounds();
    if (! body.contains (p)) return false;
    outRow = (p.y - body.getY() + gridScrollY_) / kRowH;
    outCol = (p.x - body.getX()) / kColW;
    return outRow >= 0 && outRow < scenes_.size()
        && outCol >= 0 && outCol < columns_.size();
}

bool SessionView::hitTestSceneLabel (Point<int> p, int& outRow) const noexcept
{
    const auto strip = sceneLabelStripBounds();
    if (! strip.contains (p)) return false;
    outRow = (p.y - strip.getY() + gridScrollY_) / kRowH;
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

bool SessionView::hitTestColumnStop (Point<int> p, int& outCol) const noexcept
{
    /* STOP + MUTE now live in the column header, not a separate
     * footer row.  Quickly scope down by checking the header strip
     * before pixel-perfect bounds testing. */
    const auto header = headerRowBounds();
    if (! header.contains (p)) return false;
    if (p.x < header.getX() + kSceneLabelW) return false;
    outCol = (p.x - header.getX() - kSceneLabelW) / kColW;
    if (outCol < 0 || outCol >= columns_.size()) return false;
    return columnStopButtonBounds (outCol).contains (p);
}

bool SessionView::hitTestColumnMute (Point<int> p, int& outCol) const noexcept
{
    const auto header = headerRowBounds();
    if (! header.contains (p)) return false;
    if (p.x < header.getX() + kSceneLabelW) return false;
    outCol = (p.x - header.getX() - kSceneLabelW) / kColW;
    if (outCol < 0 || outCol >= columns_.size()) return false;
    return columnMuteButtonBounds (outCol).contains (p);
}

bool SessionView::hitTestColumnSolo (Point<int> p, int& outCol) const noexcept
{
    const auto header = headerRowBounds();
    if (! header.contains (p)) return false;
    if (p.x < header.getX() + kSceneLabelW) return false;
    outCol = (p.x - header.getX() - kSceneLabelW) / kColW;
    if (outCol < 0 || outCol >= columns_.size()) return false;
    return columnSoloButtonBounds (outCol).contains (p);
}

bool SessionView::hitTestMasterCell (Point<int> p, int& outRow) const noexcept
{
    const auto col = masterColumnBounds();
    if (! col.contains (p)) return false;
    outRow = (p.y - col.getY() + gridScrollY_) / kRowH;
    return outRow >= 0 && outRow < scenes_.size();
}

bool SessionView::hitTestMasterLaunch (Point<int> p, int& outRow) const noexcept
{
    if (! hitTestMasterCell (p, outRow)) return false;
    return masterLaunchButtonBounds (outRow).contains (p);
}

bool SessionView::hitTestMasterTempo (Point<int> p, int& outRow) const noexcept
{
    if (! hitTestMasterCell (p, outRow)) return false;
    return masterTempoFieldBounds (outRow).contains (p);
}

bool SessionView::hitTestMasterSig (Point<int> p, int& outRow) const noexcept
{
    if (! hitTestMasterCell (p, outRow)) return false;
    return masterSigFieldBounds (outRow).contains (p);
}

bool SessionView::hitTestMasterEdit (Point<int> p, int& outRow) const noexcept
{
    if (! hitTestMasterCell (p, outRow)) return false;
    return masterEditButtonBounds (outRow).contains (p);
}

int SessionView::maxGridScrollY() const noexcept
{
    const int totalH = scenes_.size() * kRowH;
    const int viewH  = gridBodyBounds().getHeight();
    return juce::jmax (0, totalH - viewH);
}

void SessionView::clampScrollOffset()
{
    gridScrollY_ = juce::jlimit (0, maxGridScrollY(), gridScrollY_);
}

/* === Paint ============================================================= */

void SessionView::paint (Graphics& g)
{
    g.fillAll (kBgColour);

    /* Toolbar background -- sits above the column-header row. */
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

    g.setFont (monoFont (
                                  kHeaderFontSize, juce::Font::bold));

    for (int c = 0; c < columns_.size(); ++c)
    {
        const auto h     = columnHeaderBounds (c);
        const auto tint  = columnTint (c);
        const auto nameR = columnNameLabelBounds (c);
        const auto muteR = columnMuteButtonBounds (c);
        const auto soloR = columnSoloButtonBounds (c);

        /* Top tint band, a la trackereditor track header. */
        g.setColour (tint);
        g.fillRect (h.getX(), h.getY(), h.getWidth() - 1, 4);

        /* Top-half background -- translucent tint over header bg.
         * Bottom-half (where the buttons live) stays plain so the
         * button shapes read clearly. */
        g.setColour (tint.withAlpha (0.10f));
        g.fillRect (h.getX(), h.getY() + 4, h.getWidth() - 1, nameR.getHeight() - 4);

        /* Column name -- top half of the header. */
        g.setFont (monoFont (
                                      kHeaderFontSize, juce::Font::bold));
        g.setColour (tint);
        g.drawText (columns_.getReference (c).name,
                    nameR.reduced (8, 0),
                    juce::Justification::centredLeft, true);

        const bool muted  = isColumnMuted  (c);
        const bool soloed = isColumnSoloed (c);
        const Colour btnTint = tint.withMultipliedSaturation (0.6f)
                                   .withMultipliedBrightness (0.55f);

        g.setFont (monoFont (
                                      9.0f, juce::Font::bold));

        /* MUTE -- amber-red when user-muted (NOT when muted by
         * solo elsewhere, since the visual should reflect intent). */
        g.setColour (muted ? Colour { 0xff'c0'30'30 } : btnTint);
        g.fillRect (muteR);
        g.setColour (kCellOutlineColour);
        g.drawRect (muteR, 1);
        g.setColour (muted ? juce::Colours::white
                           : juce::Colours::white.withAlpha (0.70f));
        g.drawText ("MUTE", muteR, juce::Justification::centred);

        /* SOLO -- yellow when active; matches mixing-desk convention. */
        g.setColour (soloed ? Colour { 0xff'd5'b0'30 } : btnTint);
        g.fillRect (soloR);
        g.setColour (kCellOutlineColour);
        g.drawRect (soloR, 1);
        g.setColour (soloed ? juce::Colours::black
                            : juce::Colours::white.withAlpha (0.70f));
        g.drawText ("SOLO", soloR, juce::Justification::centred);
    }

    /* Column drag visual feedback.  Source column gets a translucent
     * overlay; hover column gets a vertical amber drop-indicator at
     * its left edge so the user sees where the release will land. */
    if (columnDragActive_)
    {
        if (columnDragSource_ >= 0 && columnDragSource_ < columns_.size())
        {
            const auto src = columnHeaderBounds (columnDragSource_);
            g.setColour (juce::Colours::black.withAlpha (0.35f));
            g.fillRect (src);
        }
        if (columnDragHoverCol_ >= 0 && columnDragHoverCol_ < columns_.size()
            && columnDragHoverCol_ != columnDragSource_)
        {
            const auto h = columnHeaderBounds (columnDragHoverCol_);
            const bool dropOnLeft = (columnDragHoverCol_ < columnDragSource_);
            const int xLine = dropOnLeft ? h.getX() : h.getRight() - 2;
            g.setColour (kPlayheadAccent);
            g.fillRect (xLine, header.getY(), 2, header.getHeight());
        }
    }

    /* Everything from here through the cell loop draws inside the
     * scrollable strip (scene labels + grid body + master column).
     * Clip to that union so scroll-shifted rows can't bleed into
     * the column header / column-stop area.  Explicit save/restore
     * so we can close the clip region at a known point further down.
     *
     * NOTE: master column MUST be in this union -- earlier it was
     * left out and the column drew into a clipped-away region,
     * making it invisible. */
    const auto masterCol = masterColumnBounds();
    const auto scrollableArea = labels.getUnion (body).getUnion (masterCol);
    g.saveState();
    g.reduceClipRegion (scrollableArea);

    /* --- Scene label strip (left column) --- */
    g.setColour (kGutterColour);
    g.fillRect (labels);

    g.setFont (monoFont (
                                  kLabelFontSize, juce::Font::plain));

    for (int r = 0; r < scenes_.size(); ++r)
    {
        const auto sb = sceneLabelBounds (r);

        /* Subtle alternating row tint, matching trackereditor's beat
         * highlight rhythm -- every 4th row gets a faint stripe. */
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
    g.setFont (monoFont (
                                  kCellFontSize, juce::Font::plain));

    /* Build sparse (scene, column) -> SessionClip* lookup once per
     * paint so the inner cell loop drops from O(R*C*N) linear
     * findClip scans to O(R*C) array indexes.  At 16 scenes x 8
     * columns x 20 clips this cuts ~2560 compares per paint to one
     * pass of the clips_ array. */
    juce::Array<juce::Array<SessionClip*>> clipByCell;
    clipByCell.ensureStorageAllocated (scenes_.size());
    for (int r = 0; r < scenes_.size(); ++r)
    {
        juce::Array<SessionClip*> row;
        row.insertMultiple (0, nullptr, columns_.size());
        clipByCell.add (std::move (row));
    }
    for (auto* clipPtr : clips_)
    {
        if (clipPtr == nullptr) continue;
        if (clipPtr->sceneRow >= 0 && clipPtr->sceneRow < scenes_.size()
            && clipPtr->columnIdx >= 0 && clipPtr->columnIdx < columns_.size())
            clipByCell.getReference (clipPtr->sceneRow).set (clipPtr->columnIdx, clipPtr);
    }

    /* Viewport-clip skip: only cells intersecting the dirty rect do
     * any work.  Saves the per-cell fillRect + drawRect overhead
     * (and per-playing-cell playhead bar + name draw) when a
     * partial repaint covers a slice of the grid. */
    const auto cellPaintClip = g.getClipBounds();

    for (int r = 0; r < scenes_.size(); ++r)
    {
        for (int c = 0; c < columns_.size(); ++c)
        {
            const auto cb    = cellBounds (r, c);
            if (! cellPaintClip.intersects (cb))
                continue;
            const auto inner = cb.reduced (2, 2);

            SessionClip* clip = clipByCell.getReference (r).getReference (c);
            if (clip == nullptr)
            {
                /* Empty cell -- flat dark with thin outline. */
                g.setColour (kEmptyCellColour);
                g.fillRect (inner);
                g.setColour (kCellOutlineColour);
                g.drawRect (inner, 1);
                continue;
            }

            const LiveState clipState = clip->state.load (std::memory_order_relaxed);
            const bool playing    = (clipState == LiveState::Playing);
            const bool waitStart  = (clipState == LiveState::WaitingToStart);
            const bool waitStop   = (clipState == LiveState::WaitingToStop);

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
                /* Proper centred square stop glyph -- the prior
                 * reduced(5,7) made it a narrow rectangle because
                 * playR's width and height differ.  Use a fixed
                 * 8x8 centred square so it visually matches the
                 * equilateral play triangle. */
                g.fillRect (playR.withSizeKeepingCentre (8, 8));
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

            /* --- Edit button (RIGHT) -- three horizontal lines as a
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
                double frac = 0.0;
                bool haveFrac = false;
                if (clip->kind == ClipKind::Tracker)
                {
                    if (auto* trk = lookupTracker (clip->trackerNodeId))
                    {
                        const int    total = juce::jmax (1, trk->getSequenceLengthRows (clip->sequenceIdx));
                        const double pos   = trk->getSequencePositionRows (clip->sequenceIdx);
                        frac = juce::jlimit (0.0, 1.0, pos / (double) total);
                        haveFrac = true;
                    }
                }
                else
                {
                    if (auto* mp = lookupMidiPlayer (clip->midiPlayerNodeId))
                    {
                        const double total = juce::jmax (1.0e-6,
                                                         mp->getSessionClipLengthBeats (clip->midiSourceId));
                        const double pos   = mp->getSessionClipPositionBeats (clip->midiSourceId);
                        frac = juce::jlimit (0.0, 1.0, pos / total);
                        haveFrac = true;
                    }
                }
                if (haveFrac)
                {
                    const int barW    = juce::jmax (1, (int) (frac * cb.getWidth()));
                    /* Translucent track underneath so the playhead reads
                     * as a moving fill on a dim runway. */
                    g.setColour (juce::Colours::black.withAlpha (0.30f));
                    g.fillRect (cb.getX() + 2,
                                cb.getBottom() - 5,
                                cb.getWidth() - 4,
                                3);
                    g.setColour (kPlayheadAccent.withAlpha (0.95f));
                    g.fillRect (cb.getX() + 2,
                                cb.getBottom() - 5,
                                barW - 4,
                                3);
                }
            }

            /* Outline.  WaitingTo* states pulse the border so the user
             * can see the clip is queued.  Pulse alpha runs sin-wave
             * off pulsePhase_, repainted each timer tick (gated in
             * timerCallback). */
            if (waitStart || waitStop)
            {
                /* 30 Hz timer; one full pulse cycle every ~0.7 s. */
                const float t = (float) (pulsePhase_ % 24) / 24.0f;
                const float a = 0.50f + 0.45f * std::sin (t * juce::MathConstants<float>::twoPi);
                const Colour pulseCol = waitStart
                    ? kPlayheadAccent          // amber for "waiting to start"
                    : Colour { 0xff'e0'40'40 }; // red for "waiting to stop"
                g.setColour (pulseCol.withAlpha (a));
                g.drawRect (inner, 2);
            }
            else
            {
                g.setColour (kCellOutlineColour);
                g.drawRect (inner, 1);
            }
        }
    }

    /* Clip-drag drop-target highlight -- drawn over an empty cell that
     * the cursor is hovering during an active drag.  Same-column
     * drops are accepted; cross-column ones are silently rejected
     * by moveClip, but the highlight still appears so the user gets
     * feedback on where their cursor is.  Source cell dims so the
     * "lift" is visible. */
    if (dragActive_ && dragSource_ != nullptr)
    {
        const auto srcRect = cellBounds (dragSource_->sceneRow, dragSource_->columnIdx);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRect (srcRect.reduced (2, 2));

        if (dragHoverRow_ >= 0 && dragHoverCol_ >= 0
            && (dragHoverRow_ != dragSource_->sceneRow
             || dragHoverCol_ != dragSource_->columnIdx))
        {
            const auto target = cellBounds (dragHoverRow_, dragHoverCol_);
            const bool empty  = (findClip (dragHoverRow_, dragHoverCol_) == nullptr);
            /* Same-host check: drop is only valid within the source's
             * own column (cross-host migration is deferred for both
             * tracker + midi kinds).  Branch on the drag source's
             * kind. */
            bool sameTracker = false;
            if (dragHoverCol_ < columns_.size())
            {
                const auto& hc = columns_.getReference (dragHoverCol_);
                if (hc.kind == dragSource_->kind)
                {
                    if (dragSource_->kind == ClipKind::Tracker)
                        sameTracker = (hc.trackerNodeId == dragSource_->trackerNodeId);
                    else
                        sameTracker = (hc.midiPlayerNodeId == dragSource_->midiPlayerNodeId);
                }
            }
            const Colour outline = (empty && sameTracker)
                ? kPlayheadAccent.withAlpha (0.95f)
                : Colour { 0xff'e0'40'40 }.withAlpha (0.85f);
            g.setColour (outline);
            g.drawRect (target.reduced (2, 2), 2);
        }
    }

    /* Scene-reorder insertion indicator -- thin amber line at the row
     * boundary the source scene will land on. */
    if (sceneDragActive_ && sceneDragSource_ >= 0 && sceneDragHoverRow_ >= 0
        && sceneDragHoverRow_ != sceneDragSource_)
    {
        const auto target = sceneLabelBounds (sceneDragHoverRow_);
        const int y = (sceneDragHoverRow_ > sceneDragSource_)
                          ? target.getBottom()
                          : target.getY();
        g.setColour (kPlayheadAccent);
        g.fillRect (target.getX() + 4, y - 1, target.getWidth() - 8, 3);
    }

    /* --- Master column (rightmost, scrolls with grid) ------------
     * Ableton-style "scene master" -- each row shows a launch button
     * + tempo + signature.  Drawn inside the scrollable clip region
     * so it scrolls in lock-step with grid rows.  See
     * project_session_view_ableton_scene_master_column memory. */
    {
        const auto masterCol = masterColumnBounds();
        /* Master column shares the main session-view background --
         * one continuous surface so it doesn't read as a separate
         * panel.  Same kBgColour, same kCellOutlineColour family. */
        g.setColour (kBgColour);
        g.fillRect (masterCol);
        g.setColour (kCellOutlineColour);
        g.drawVerticalLine (masterCol.getX(),
                            (float) masterCol.getY(),
                            (float) masterCol.getBottom());

        g.setFont (monoFont (
                                      kCellFontSize, juce::Font::plain));

        /* Current session-wide tempo + signature -- shown dimmed in
         * scenes that don't carry an override so the user can see
         * what the scene would inherit on launch.  Matches Ableton's
         * Master column convention. */
        double curTempo = 120.0;
        int    curBpb   = 4;
        int    curBd    = 4;
        if (services_ != nullptr)
        {
            if (auto sess = services_->context().session())
            {
                curTempo = (double) sess->getProperty (tags::tempo,       120.0);
                curBpb   = (int)    sess->getProperty (tags::beatsPerBar, 4);
                curBd    = (int)    sess->getProperty (tags::beatDivisor, 4);
            }
        }

        for (int r = 0; r < scenes_.size(); ++r)
        {
            const auto cell    = masterCellBounds (r);
            const auto launchR = masterLaunchButtonBounds (r);
            const auto tempoR  = masterTempoFieldBounds (r);
            const auto sigR    = masterSigFieldBounds (r);
            const auto& sc     = scenes_.getReference (r);

            /* Row alternation hint mirrors the scene-label strip's
             * every-4th-row stripe so they line up. */
            if ((r & 3) == 0)
            {
                g.setColour (Colour { 0xff'1f'1f'1f });
                g.fillRect (cell);
            }

            /* Launch button.  Three states:
             *   - Playing : solid amber bg + amber glyph.
             *   - WaitingToStart only (no Playing yet) : amber bg +
             *     glyph pulsed on pulsePhase_ so the user gets
             *     immediate feedback during the quant gap.
             *   - else : gray bg + neutral glyph. */
            const bool sceneActive = sceneHasActiveClip (r);
            const bool sceneQueued = (! sceneActive) && sceneHasQueuedClip (r);

            float queuedAlpha = 1.0f;
            if (sceneQueued)
            {
                const float t = (float) (pulsePhase_ % 24) / 24.0f;
                queuedAlpha = 0.50f + 0.45f
                            * std::sin (t * juce::MathConstants<float>::twoPi);
            }

            if (sceneActive)
                g.setColour (kPlayheadAccent.withAlpha (0.35f));
            else if (sceneQueued)
                g.setColour (kPlayheadAccent.withAlpha (0.35f * queuedAlpha));
            else
                g.setColour (Colour { 0xff'2a'2a'2a });
            g.fillRect (launchR);

            if (sceneActive)
                g.setColour (kPlayheadAccent);
            else if (sceneQueued)
                g.setColour (kPlayheadAccent.withAlpha (queuedAlpha));
            else
                g.setColour (Colour { 0xff'd0'd0'd0 });
            {
                juce::Path p;
                const float gx = (float) launchR.getX() + 6.0f;
                const float gy = (float) launchR.getY() + 6.0f;
                const float gh = (float) launchR.getHeight() - 12.0f;
                const float gw = gh * 0.866f;
                p.addTriangle (gx,      gy,
                               gx,      gy + gh,
                               gx + gw, gy + gh * 0.5f);
                g.fillPath (p);
            }

            /* Tempo + sig fields -- match the empty-cell visual
             * family (kEmptyCellColour bg + kCellOutlineColour
             * outline) so they look like editable inputs that share
             * the session-view widget vocabulary. */
            auto drawField = [&] (const Rectangle<int>& rr, const String& text,
                                  bool overrideSet)
            {
                g.setColour (kEmptyCellColour);
                g.fillRect (rr);
                g.setColour (kCellOutlineColour);
                g.drawRect (rr, 1);
                g.setColour (overrideSet ? Colour { 0xff'ff'ff'ff }
                                         : kRowTextColour);
                g.drawText (text, rr.reduced (4, 0),
                            juce::Justification::centred, true);
            };

            /* Show the OVERRIDE value bright when set; otherwise
             * the current session value dimmed (inherit-on-launch).
             * Ableton convention -- the user can see what the scene
             * would do without having to click in. */
            drawField (tempoR,
                       sc.tempoOverride > 0.0 ? String (sc.tempoOverride, 2)
                                              : String (curTempo, 2),
                       sc.tempoOverride > 0.0);
            drawField (sigR,
                       (sc.beatsPerBar > 0 && sc.beatDivisor > 0)
                            ? String (sc.beatsPerBar) + "/" + String (sc.beatDivisor)
                            : String (curBpb) + "/" + String (curBd),
                       sc.beatsPerBar > 0 && sc.beatDivisor > 0);

            /* Edit button -- three horizontal lines glyph (same as
             * clip cell edit button) opens the Scene View popup. */
            const auto editR = masterEditButtonBounds (r);
            g.setColour (juce::Colours::black.withAlpha (0.20f));
            g.fillRect (editR);
            g.setColour (juce::Colour { 0xff'd0'd0'd0 });
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

            /* Row divider. */
            g.setColour (kCellOutlineColour);
            g.drawHorizontalLine (cell.getBottom() - 1,
                                  (float) cell.getX(),
                                  (float) cell.getRight());
        }
    }

    /* Master scene row indicator -- translucent amber overlay across
     * the entire row (scene label + clip cells + master cell).  Bound
     * to the master scene identity (currentSceneRow_, set on bang).
     * Solid amber while the master scene has at least one Playing
     * clip; pulsed amber while clips are still queued
     * (WaitingToStart) so the bang has immediate visual feedback
     * during the launch-quant gap.  No highlight on other scenes
     * whose clips happen to be playing.  Drawn AFTER all per-cell
     * paint so it tints them uniformly. */
    if (currentSceneRow_ >= 0 && currentSceneRow_ < scenes_.size())
    {
        const bool rowActive = sceneHasActiveClip (currentSceneRow_);
        const bool rowQueued = (! rowActive)
                             && sceneHasQueuedClip (currentSceneRow_);
        if (rowActive || rowQueued)
        {
            float alpha = 1.0f;
            if (rowQueued)
            {
                const float t = (float) (pulsePhase_ % 24) / 24.0f;
                alpha = 0.50f + 0.45f
                      * std::sin (t * juce::MathConstants<float>::twoPi);
            }
            const auto sl = sceneLabelBounds (currentSceneRow_);
            const Rectangle<int> rowR (0, sl.getY(), getWidth(), kRowH);
            g.setColour (kPlayheadAccent.withAlpha (0.10f * alpha));
            g.fillRect (rowR);
            g.setColour (kPlayheadAccent.withAlpha (0.80f * alpha));
            g.drawHorizontalLine (rowR.getY(),         0.0f, (float) getWidth());
            g.drawHorizontalLine (rowR.getBottom() - 1, 0.0f, (float) getWidth());
        }
    }

    /* Close the scrollable-area clip region -- everything after this
     * draws into the fixed (non-scrolling) chrome strips. */
    g.restoreState();

    /* Footer hint strip. */
    const auto footer = footerBounds();
    g.setColour (kHeaderBgColour);
    g.fillRect (footer);
    g.setColour (kCellOutlineColour);
    g.drawHorizontalLine (footer.getY(), 0.0f, (float) getWidth());

    g.setColour (kRowTextColour);
    g.setFont (monoFont (
                                  kLabelFontSize, juce::Font::plain));
    String hint;
    if (columns_.isEmpty())
        hint = "Add a Tracker node to the graph to populate columns";
    else
        hint = String (columns_.size()) + " column"  + (columns_.size() == 1 ? "" : "s")
             + " | "
             + String (scenes_.size())  + " scene"  + (scenes_.size()  == 1 ? "" : "s")
             + " | right-click a cell or scene label for more";
    const auto hintR = footer.withTrimmedLeft (kSceneLabelW + 8);
    g.drawText (hint, hintR, juce::Justification::centredLeft, true);
}

/* === Mouse ============================================================= */

void SessionView::mouseDown (const MouseEvent& e)
{
    int row = -1, col = -1;

    if (e.mods.isPopupMenu())
    {
        /* Column header right-click -- track-level actions.  Tested
         * before the cell hit so a click on the header strip never
         * falls through to the per-clip context menu. */
        for (int hc = 0; hc < columns_.size(); ++hc)
        {
            if (! columnHeaderBounds (hc).contains (e.getPosition())) continue;

            juce::PopupMenu m;
            m.addItem (1, "Move left",  hc > 0);
            m.addItem (2, "Move right", hc < columns_.size() - 1);
            m.addSeparator();
            m.addItem (3, "Delete track");
            const int r = m.showAt (Rectangle<int> (e.getScreenX(), e.getScreenY(), 1, 1));
            switch (r)
            {
                case 1: moveColumn   (hc, -1); break;
                case 2: moveColumn   (hc, +1); break;
                case 3: removeColumn (hc);     break;
                default: break;
            }
            return;
        }

        /* Master column right-click -- scene transport overrides. */
        if (hitTestMasterCell (e.getPosition(), row))
        {
            const auto& sc = scenes_.getReference (row);
            juce::PopupMenu m;
            m.addItem (1, "Launch scene");
            m.addSeparator();
            m.addItem (10, "Set tempo...");
            m.addItem (11, "Clear tempo override", sc.tempoOverride > 0.0);
            m.addSeparator();
            m.addItem (12, "Set signature...");
            m.addItem (13, "Clear signature override",
                       sc.beatsPerBar > 0 && sc.beatDivisor > 0);
            const int r = m.showAt (Rectangle<int> (e.getScreenX(), e.getScreenY(), 1, 1));
            switch (r)
            {
                case 1:  bangScene           (row); break;
                case 10: editSceneTempo      (row); break;
                case 11: clearSceneTempo     (row); break;
                case 12: editSceneSignature  (row); break;
                case 13: clearSceneSignature (row); break;
                default: break;
            }
            return;
        }

        if (hitTestCell (e.getPosition(), row, col))
        {
            if (auto* clip = findClip (row, col))
            {
                /* Right-click clip menu is now STRUCTURAL only:
                 *   - Clip properties... (opens Clip View popup with
                 *     Name + Colour + Launch quant + Follow action)
                 *   - Delete clip
                 * Per-property edits live inside the Clip View popup. */
                juce::PopupMenu m;
                m.addItem (5, "Clip properties...");
                m.addSeparator();
                m.addItem (1, "Delete clip");

                const int r = m.showAt (Rectangle<int> (e.getScreenX(), e.getScreenY(), 1, 1));
                switch (r)
                {
                    case 1: deleteClip   (*clip); break;
                    case 5: openClipView (*clip); break;
                    default: break;
                }
            }
            else if (col < columns_.size())
            {
                const auto& column = columns_.getReference (col);
                if (column.kind == ClipKind::Tracker)
                {
                    auto* trk = lookupTracker (column.trackerNodeId);
                    if (trk == nullptr) return;

                    juce::PopupMenu assign;
                    const int patternCount = trk->numPatterns();
                    for (int i = 0; i < patternCount; ++i)
                    {
                        /* Item id 100+i so it never collides with the
                         * fixed top-level items below. */
                        assign.addItem (100 + i, "Pattern " + String (i + 1));
                    }

                    juce::PopupMenu m;
                    m.addItem (1, "Add new pattern");
                    if (patternCount > 0)
                        m.addSubMenu ("Assign existing pattern", assign);

                    const int r = m.showAt (Rectangle<int> (e.getScreenX(), e.getScreenY(), 1, 1));
                    if (r == 1) addClipAt (row, col);
                    else if (r >= 100 && r < 100 + patternCount)
                        assignExistingPattern (row, col, r - 100);
                }
                else /* Midi column -- only "Add new MIDI clip" */
                {
                    juce::PopupMenu m;
                    m.addItem (1, "Add new MIDI clip");

                    const int r = m.showAt (Rectangle<int> (e.getScreenX(), e.getScreenY(), 1, 1));
                    if (r == 1) addClipAt (row, col);
                }
            }
            return;
        }

        if (hitTestSceneLabel (e.getPosition(), row))
        {
            juce::PopupMenu m;
            m.addItem (1, "Launch scene");
            m.addSeparator();
            m.addItem (5, "Scene properties...");
            m.addSeparator();
            m.addItem (2, "Insert scene above");
            m.addItem (3, "Insert scene below");
            m.addSeparator();
            const bool canDelete = scenes_.size() > 1;
            m.addItem (4, "Delete scene", canDelete);
            const int r = m.showAt (Rectangle<int> (e.getScreenX(), e.getScreenY(), 1, 1));
            switch (r)
            {
                case 1: bangScene     (row);     break;
                case 2: insertScene   (row);     break;
                case 3: insertScene   (row + 1); break;
                case 4: deleteScene   (row);     break;
                case 5: openSceneView (row);     break;
                default: break;
            }
        }
        return;
    }

    /* Column-header MUTE / SOLO.  Tested before cell hits so a click
     * here never stages a clip drag.  STOP is no longer surfaced --
     * STOP ALL covers the global, per-clip click stops individual
     * clips, and MUTE silences the column without stopping clips. */
    if (hitTestColumnMute (e.getPosition(), col))
    {
        toggleColumnMute (col);
        return;
    }
    if (hitTestColumnSolo (e.getPosition(), col))
    {
        toggleColumnSolo (col);
        return;
    }

    /* Column-header drag staging.  Tested AFTER M/S so direct button
     * clicks still fire.  Activates only past the drag threshold in
     * mouseDrag; a stationary click is a no-op. */
    for (int hc = 0; hc < columns_.size(); ++hc)
    {
        if (! columnHeaderBounds (hc).contains (e.getPosition())) continue;
        columnDragSource_   = hc;
        dragStart_          = e.getPosition();
        columnDragActive_   = false;
        columnDragHoverCol_ = hc;
        return;
    }

    /* Master column launch button -> bang scene.
     * Master column tempo/sig fields -> click-to-edit prompts. */
    if (hitTestMasterLaunch (e.getPosition(), row))
    {
        bangScene (row);
        return;
    }
    if (hitTestMasterEdit (e.getPosition(), row))
    {
        openSceneView (row);
        return;
    }
    if (hitTestMasterTempo (e.getPosition(), row))
    {
        editSceneTempo (row);
        return;
    }
    if (hitTestMasterSig (e.getPosition(), row))
    {
        editSceneSignature (row);
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
            if (clip->kind == ClipKind::Midi)
                openPianoRollForClip (*clip);
            else
                openTrackerDockForClip (*clip);
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

    /* Middle of a filled cell -- stage a potential clip drag. */
    if (hitTestCell (e.getPosition(), row, col))
    {
        if (auto* clip = findClip (row, col))
        {
            dragSource_   = clip;
            dragStart_    = e.getPosition();
            dragActive_   = false;
            dragHoverRow_ = -1;
            dragHoverCol_ = -1;
            return;
        }
    }

    /* Scene label click -- stage a potential reorder.  mouseUp fires
     * bangScene if the click never moved past the drag threshold;
     * otherwise it performs the reorder. */
    if (hitTestSceneLabel (e.getPosition(), row))
    {
        sceneDragSource_   = row;
        dragStart_         = e.getPosition();
        sceneDragActive_   = false;
        sceneDragHoverRow_ = row;
    }
}

void SessionView::mouseDrag (const MouseEvent& e)
{
    /* --- Clip drag --- */
    if (dragSource_ != nullptr)
    {
        if (! dragActive_)
        {
            if (e.getPosition().getDistanceFrom (dragStart_) < 6) return;
            dragActive_ = true;
            setMouseCursor (e.mods.isShiftDown() ? juce::MouseCursor::CopyingCursor
                                                 : juce::MouseCursor::DraggingHandCursor);
        }

        int row = -1, col = -1;
        const bool overCell = hitTestCell (e.getPosition(), row, col);
        if (! overCell) { row = -1; col = -1; }
        if (row != dragHoverRow_ || col != dragHoverCol_)
        {
            /* Diff-gated repaint: only redraw the old + new target
             * cells so we don't burn paint cycles on every drag pixel. */
            if (dragHoverRow_ >= 0 && dragHoverCol_ >= 0)
                repaint (cellBounds (dragHoverRow_, dragHoverCol_));
            dragHoverRow_ = row;
            dragHoverCol_ = col;
            if (row >= 0 && col >= 0)
                repaint (cellBounds (row, col));
        }
        return;
    }

    /* --- Scene drag --- */
    if (sceneDragSource_ >= 0)
    {
        if (! sceneDragActive_)
        {
            if (e.getPosition().getDistanceFrom (dragStart_) < 6) return;
            sceneDragActive_ = true;
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        }

        int row = -1;
        if (! hitTestSceneLabel (e.getPosition(), row)) row = -1;
        if (row != sceneDragHoverRow_)
        {
            sceneDragHoverRow_ = row;
            repaint (sceneLabelStripBounds());
        }
        return;
    }

    /* --- Column-header drag --- */
    if (columnDragSource_ >= 0)
    {
        if (! columnDragActive_)
        {
            if (e.getPosition().getDistanceFrom (dragStart_) < 6) return;
            columnDragActive_ = true;
            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        }

        /* Resolve hover column under cursor.  Use the cursor's x
         * against each column's header bounds; -1 if none. */
        int hoverCol = -1;
        for (int c = 0; c < columns_.size(); ++c)
        {
            if (columnHeaderBounds (c).contains (e.getPosition()))
                { hoverCol = c; break; }
        }
        if (hoverCol != columnDragHoverCol_)
        {
            columnDragHoverCol_ = hoverCol;
            repaint (headerRowBounds());
        }
    }
}

void SessionView::mouseUp (const MouseEvent& e)
{
    /* --- Clip drag drop --- */
    if (dragSource_ != nullptr)
    {
        if (dragActive_)
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);

            int row = -1, col = -1;
            if (hitTestCell (e.getPosition(), row, col))
            {
                const bool sameSpot = (row == dragSource_->sceneRow
                                    && col == dragSource_->columnIdx);
                const bool targetEmpty = (findClip (row, col) == nullptr);

                if (! sameSpot && targetEmpty
                    && col >= 0 && col < columns_.size())
                {
                    if (e.mods.isShiftDown())
                        copyClip (*dragSource_, row, col);
                    else
                        moveClip (*dragSource_, row, col);
                }
            }
        }

        dragSource_   = nullptr;
        dragActive_   = false;
        dragHoverRow_ = -1;
        dragHoverCol_ = -1;
        repaint();
        return;
    }

    /* --- Scene drag drop OR scene-label click --- */
    if (sceneDragSource_ >= 0)
    {
        if (sceneDragActive_)
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);

            int row = -1;
            if (hitTestSceneLabel (e.getPosition(), row)
                && row != sceneDragSource_)
            {
                reorderScene (sceneDragSource_, row);
            }
        }
        else
        {
            /* Static click -- bang the scene. */
            bangScene (sceneDragSource_);
        }

        sceneDragSource_   = -1;
        sceneDragActive_   = false;
        sceneDragHoverRow_ = -1;
        repaint();
        return;
    }

    /* --- Column-header drag drop --- */
    if (columnDragSource_ >= 0)
    {
        if (columnDragActive_)
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);

            if (columnDragHoverCol_ >= 0
                && columnDragHoverCol_ < columns_.size()
                && columnDragHoverCol_ != columnDragSource_)
            {
                /* Step the column source toward the hover target one
                 * swap at a time so clip columnIdx remapping stays
                 * coherent across multi-position moves. */
                int src = columnDragSource_;
                const int dst = columnDragHoverCol_;
                while (src != dst)
                {
                    const int step = (dst > src) ? +1 : -1;
                    moveColumn (src, step);
                    src += step;
                }
            }
        }

        columnDragSource_   = -1;
        columnDragActive_   = false;
        columnDragHoverCol_ = -1;
        repaint (headerRowBounds());
    }
}

void SessionView::mouseWheelMove (const MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    juce::ignoreUnused (e);
    if (maxGridScrollY() <= 0) return;
    /* Convert wheel delta to pixels.  3 rows per "tick" feels natural
     * -- matches the usual JUCE default for list scrolling. */
    const int delta = (int) std::round (-wheel.deltaY * kRowH * 3.0f);
    const int prev  = gridScrollY_;
    gridScrollY_ = juce::jlimit (0, maxGridScrollY(), gridScrollY_ + delta);
    if (gridScrollY_ != prev)
        repaint();
}

void SessionView::mouseDoubleClick (const MouseEvent& e)
{
    int row = -1, col = -1;

    /* Double-click on the middle of a clip cell opens the Clip View
     * popup.  The play + edit buttons swallow their own clicks via
     * mouseDown, so the double event only reaches the cell body. */
    if (hitTestCell (e.getPosition(), row, col))
    {
        if (auto* clip = findClip (row, col))
        {
            if (! playButtonBounds (row, col).contains (e.getPosition())
             && ! editButtonBounds (row, col).contains (e.getPosition()))
            {
                /* Default double-click action: open the clip in the
                 * matching editor dock.  Tracker -> right-side tracker
                 * dock.  Midi -> bottom-attached piano-roll dock.
                 * Mirrors the edit-button affordance + arrangement-
                 * clip double-click behaviour. */
                if (clip->kind == ClipKind::Midi)
                    openPianoRollForClip (*clip);
                else
                    openTrackerDockForClip (*clip);
                return;
            }
        }
    }

    /* Double-click a scene label to rename it. */
    if (hitTestSceneLabel (e.getPosition(), row))
        renameScene (row);
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

MidiPlayerNode* SessionView::lookupMidiPlayer (juce::uint32 nodeId) const
{
    if (services_ == nullptr) return nullptr;
    auto sess = services_->context().session();
    if (sess == nullptr) return nullptr;
    auto graph = sess->getActiveGraph();
    if (! graph.isValid()) return nullptr;
    Node n = graph.getNodeById (nodeId);
    if (! n.isValid()) return nullptr;
    return dynamic_cast<MidiPlayerNode*> (n.getObject());
}

MidiNoteRegion* SessionView::findMidiClipRegion (const juce::Uuid& sourceId) noexcept
{
    if (sourceId.isNull()) return nullptr;
    for (const auto* c : clips_)
    {
        if (c->kind != ClipKind::Midi)   continue;
        if (c->midiSourceId != sourceId) continue;
        if (auto* mp = lookupMidiPlayer (c->midiPlayerNodeId))
            return mp->getClipRegion (sourceId);
        return nullptr;
    }
    return nullptr;
}

int SessionView::beatsPerBar() const
{
    if (services_ == nullptr) return 4;
    auto sess = services_->context().session();
    if (sess == nullptr) return 4;
    return juce::jmax (1, (int) sess->getProperty (tags::beatsPerBar, 4));
}

double SessionView::currentTransportBeat() const
{
    if (monitor_ == nullptr) return 0.0;
    return (double) monitor_->getPositionBeats();
}

double SessionView::computeTargetBeat (double curBeat, LaunchQuant q) const
{
    double qb = 0.0;
    switch (q)
    {
        case LaunchQuant::Off:      return -1.0;
        case LaunchQuant::Beat:     qb = 1.0;                          break;
        case LaunchQuant::Bar:      qb = (double) beatsPerBar();       break;
        case LaunchQuant::TwoBars:  qb = 2.0 * (double) beatsPerBar(); break;
        case LaunchQuant::FourBars: qb = 4.0 * (double) beatsPerBar(); break;
    }
    if (qb <= 0.0) return -1.0;

    /* Strictly future boundary.  If curBeat sits exactly on the
     * boundary, jump to the NEXT one -- pressing bang on the bar
     * line should land on the next bar, not "right now" with
     * zero waiting time (UX gives the user a moment of feedback). */
    constexpr double kEps = 1e-6;
    return std::ceil ((curBeat + kEps) / qb) * qb;
}

void SessionView::transitionClip (SessionClip& clip, double targetBeat, bool sceneLaunch)
{
    /* Validate the clip's binding against the live host node.  Stale
     * bindings (saved session whose source got removed / shrunk) silently
     * decline to schedule; the visual state stays Stopped. */
    TrackerNode*    trk = nullptr;
    MidiPlayerNode* mp  = nullptr;
    if (clip.kind == ClipKind::Tracker)
    {
        trk = lookupTracker (clip.trackerNodeId);
        if (trk == nullptr || clip.sequenceIdx < 0) return;
        /* B.4 + E.4: validate the clip's sequenceIdx against the live
         * tracker pattern count.  Stale clips from a saved session whose
         * tracker has fewer sequences than at save time would otherwise
         * write a valid=true slot in pendingActions_ that never drains. */
        if (clip.sequenceIdx >= trk->numPatterns()) return;
    }
    else /* ClipKind::Midi */
    {
        mp = lookupMidiPlayer (clip.midiPlayerNodeId);
        if (mp == nullptr) return;
        if (clip.midiSourceId.isNull()) return;
        /* Validate the slot still exists on the player.  A user could
         * have undone the clip-add or the node could have setState'd
         * to a different region inventory. */
        if (mp->getClipRegion (clip.midiSourceId) == nullptr) return;
    }

    /* Per-kind launch primitive -- captures the right node + key. */
    auto schedule = [&] (double beat, bool on) {
        if (clip.kind == ClipKind::Tracker)
            trk->schedulePlaying (clip.sequenceIdx, beat, on);
        else
            mp->schedulePlayingClip (clip.midiSourceId, beat, on);
    };

    const LiveState cur = clip.state.load (std::memory_order_relaxed);

    /* WaitingToStart: queued for launch.
     *   - sceneLaunch: already going to be on; leave alone.
     *   - single click: cancel the queued launch (toggle semantic). */
    if (cur == LiveState::WaitingToStart)
    {
        if (sceneLaunch) return;
        schedule (-1.0, false);
        clip.state.store (LiveState::Stopped, std::memory_order_relaxed);
        repaint (cellBounds (clip.sceneRow, clip.columnIdx));
        return;
    }

    /* WaitingToStop: queued for stop.  Both scene launch and single
     * click want it to stay playing -- cancel the queued stop. */
    if (cur == LiveState::WaitingToStop)
    {
        schedule (-1.0, true);
        clip.state.store (LiveState::Playing, std::memory_order_relaxed);
        repaint (cellBounds (clip.sceneRow, clip.columnIdx));
        return;
    }

    /* Playing -> sceneLaunch leaves it playing (force-start semantic);
     * single click toggles it off (toggle semantic).  Mutual exclusion
     * stays column-local. */
    if (sceneLaunch && cur == LiveState::Playing) return;

    const bool wantPlaying = (cur != LiveState::Playing);

    /* Same-column mutual exclusion at the shared targetBeat -- A->stop
     * and B->start hit the audio thread in the same render block, so
     * the flip is atomic.  Skip self.  For tracker clips, also skip
     * clips that point at the SAME underlying sequence as us: two
     * clips sharing a sequence are functionally one engine-level
     * voice.  For MIDI clips, every clip has a unique midiSourceId
     * (no sharing) so the same-source-skip never fires; the column-
     * scoped sibling stop applies unconditionally. */
    if (wantPlaying)
    {
        for (auto* other : clips_)
        {
            if (other == &clip) continue;
            if (other->columnIdx != clip.columnIdx) continue;
            /* Same kind only -- one-kind-per-column means this is
             * always true; defensive against rescanColumns races. */
            if (other->kind != clip.kind) continue;
            if (clip.kind == ClipKind::Tracker)
            {
                if (other->trackerNodeId != clip.trackerNodeId) continue;
                if (other->sequenceIdx   == clip.sequenceIdx)   continue;
            }
            else
            {
                if (other->midiPlayerNodeId != clip.midiPlayerNodeId) continue;
                /* MIDI clips don't share sources -- siblings always
                 * have different midiSourceId.  No equivalent skip. */
            }
            const LiveState os = other->state.load (std::memory_order_relaxed);
            if (os != LiveState::Playing && os != LiveState::WaitingToStart) continue;

            if (clip.kind == ClipKind::Tracker)
            {
                if (auto* otrk = lookupTracker (other->trackerNodeId))
                    otrk->schedulePlaying (other->sequenceIdx, targetBeat, false);
            }
            else
            {
                if (auto* omp = lookupMidiPlayer (other->midiPlayerNodeId))
                    omp->schedulePlayingClip (other->midiSourceId, targetBeat, false);
            }

            other->state.store (targetBeat < 0.0 ? LiveState::Stopped
                                                 : LiveState::WaitingToStop,
                                std::memory_order_relaxed);
            repaint (cellBounds (other->sceneRow, other->columnIdx));
        }
    }

    schedule (targetBeat, wantPlaying);

    const LiveState next = (targetBeat < 0.0)
        ? (wantPlaying ? LiveState::Playing       : LiveState::Stopped)
        : (wantPlaying ? LiveState::WaitingToStart : LiveState::WaitingToStop);
    clip.state.store (next, std::memory_order_relaxed);
    repaint (cellBounds (clip.sceneRow, clip.columnIdx));
}

void SessionView::bangClip (SessionClip& clip)
{
    /* Sketch Mode -- match bangScene: a single-clip click when the
     * transport is stopped auto-starts it so the clip actually
     * fires AND subsequent clip launches quantise to the next bar
     * (Ableton / Bitwig convention).  Without this, the very first
     * click on a stopped transport fired immediately (targetBeat
     * stayed -1.0) AND left the transport stopped, so every
     * follow-up clip also fired immediately -- the
     * "triggers on the spot, feels buggy" symptom.  After this
     * starts the transport, immediately re-evaluate `transportPlaying`
     * below so this same click already aligns to the next bar
     * instead of the first being immediate and only subsequent
     * clicks quantising. */
    if (monitor_ != nullptr && ! monitor_->playing.get())
    {
        if (services_ != nullptr)
            if (auto* eng = services_->context().audio().get())
                eng->setPlaying (true);
    }

    const bool transportPlaying = (monitor_ != nullptr && monitor_->playing.get());
    const double targetBeat = transportPlaying
        ? computeTargetBeat (currentTransportBeat(), clip.launchQuant)
        : -1.0;
    transitionClip (clip, targetBeat);
}

void SessionView::applyFollowAction (SessionClip& clip)
{
    /* Called from the UI timer when a playing clip wraps past its
     * end.  The actions schedule through the audio-thread FIFO; no
     * direct engine mutation here. */
    auto stopImmediate = [this, &clip] () {
        if (clip.kind == ClipKind::Tracker)
        {
            if (auto* t = lookupTracker (clip.trackerNodeId))
                t->schedulePlaying (clip.sequenceIdx, -1.0, false);
        }
        else
        {
            if (auto* mp = lookupMidiPlayer (clip.midiPlayerNodeId))
                mp->schedulePlayingClip (clip.midiSourceId, -1.0, false);
        }
        clip.state.store (LiveState::Stopped, std::memory_order_relaxed);
        repaint (cellBounds (clip.sceneRow, clip.columnIdx));
    };

    auto restartClip = [this, &clip] () {
        /* For tracker: schedulePlaying(_, true) rewinds pos to 0 even
         * when already playing.  For MIDI: schedulePlayingClip(_, true)
         * applies the same rewind via applyPendingClipForBlock. */
        if (clip.kind == ClipKind::Tracker)
        {
            if (auto* t = lookupTracker (clip.trackerNodeId))
                t->schedulePlaying (clip.sequenceIdx, -1.0, true);
        }
        else
        {
            if (auto* mp = lookupMidiPlayer (clip.midiPlayerNodeId))
                mp->schedulePlayingClip (clip.midiSourceId, -1.0, true);
        }
    };

    switch (clip.followAction)
    {
        case FollowAction::None:
            break;

        case FollowAction::Stop:
            stopImmediate();
            break;

        case FollowAction::RestartClip:
            restartClip();
            break;

        case FollowAction::NextClip:
        {
            SessionClip* nextClip = nullptr;
            int bestRow = std::numeric_limits<int>::max();
            for (auto* c : clips_)
            {
                if (c == &clip) continue;
                if (c->columnIdx != clip.columnIdx) continue;
                if (c->sceneRow > clip.sceneRow && c->sceneRow < bestRow)
                    { bestRow = c->sceneRow; nextClip = c; }
            }
            if (nextClip != nullptr)
                bangClip (*nextClip);   // mutual-exclusion stops this one
            else
                stopImmediate();        // no further clip -- fall through to Stop
            break;
        }

        case FollowAction::FirstClip:
        {
            SessionClip* firstClip = nullptr;
            int bestRow = std::numeric_limits<int>::max();
            for (auto* c : clips_)
            {
                if (c == &clip) continue;
                if (c->columnIdx != clip.columnIdx) continue;
                if (c->sceneRow < bestRow)
                    { bestRow = c->sceneRow; firstClip = c; }
            }
            if (firstClip != nullptr)
                bangClip (*firstClip);
            else
                stopImmediate();        // C.4: same fall-through as NextClip
            break;
        }
    }
}

void SessionView::bangScene (int sceneRow)
{
    if (sceneRow < 0 || sceneRow >= scenes_.size()) return;

    /* Apply Ableton-style scene-level transport overrides (tempo +
     * time signature) BEFORE launching clips so they latch the new
     * tempo immediately when their beat-target is computed. */
    applySceneOverridesToTransport (scenes_.getReference (sceneRow));

    /* "Sketch Mode" -- if the transport is stopped, launching a
     * scene also starts the transport so the clips actually fire.
     * Without this, schedulePlaying queues entries that never get
     * drained (engine doesn't advance with the transport stopped).
     * Matches Ableton's behaviour where clicking a scene launch
     * triangle plays even if the global transport isn't already
     * running. */
    if (monitor_ != nullptr && ! monitor_->playing.get())
    {
        if (services_ != nullptr)
            if (auto* eng = services_->context().audio().get())
                eng->setPlaying (true);
    }

    /* Pick the slowest quant among this scene's clips so they all
     * snap to the same target beat -- Bitwig convention.  Empty
     * scenes fall back to the toolbar's default quant: launching
     * an empty scene is the user's way of saying "stop everything
     * else at this boundary," and we still want the boundary. */
    LaunchQuant slowest = LaunchQuant::Off;
    bool any = false;
    for (auto* c : clips_)
    {
        if (c->sceneRow != sceneRow) continue;
        any = true;
        if ((int) c->launchQuant > (int) slowest)
            slowest = c->launchQuant;
    }
    if (! any) slowest = defaultLaunchQuant_;

    const bool transportPlaying = (monitor_ != nullptr && monitor_->playing.get());
    const double targetBeat = transportPlaying
        ? computeTargetBeat (currentTransportBeat(), slowest)
        : -1.0;

    /* Scene launch is column-wise.  For each column:
     *   - new scene has a clip here -> launch it via transitionClip
     *     (same-column mutual exclusion stops any prior clip on the
     *     column);
     *   - new scene has an EMPTY slot here -> stop whatever was
     *     playing on this column.  Empty slots act as "stop the
     *     track" in Ableton / Bitwig, so a row with sparse clips
     *     still represents a coherent musical state.
     * Columns the user hasn't authored a clip on are left alone. */
    for (int col = 0; col < columns_.size(); ++col)
    {
        if (auto* newClip = findClip (sceneRow, col))
        {
            /* sceneLaunch=true: force-start semantic.  An already-
             * Playing clip in this scene stays playing -- without
             * this gate the toggle semantic would stop it. */
            transitionClip (*newClip, targetBeat, true);
            continue;
        }

        /* Empty slot -- stop active clips on this column at the
         * same targetBeat so the switch is atomic in one block.
         * Both clip kinds share this empty-launch-stop semantic. */
        for (auto* c : clips_)
        {
            if (c->columnIdx != col) continue;
            const LiveState s = c->state.load (std::memory_order_relaxed);
            if (s != LiveState::Playing && s != LiveState::WaitingToStart) continue;
            if (c->kind == ClipKind::Tracker)
            {
                if (auto* trk = lookupTracker (c->trackerNodeId))
                    trk->schedulePlaying (c->sequenceIdx, targetBeat, false);
            }
            else
            {
                if (auto* mp = lookupMidiPlayer (c->midiPlayerNodeId))
                    mp->schedulePlayingClip (c->midiSourceId, targetBeat, false);
            }
            c->state.store (targetBeat < 0.0 ? LiveState::Stopped
                                             : LiveState::WaitingToStop,
                            std::memory_order_relaxed);
            repaint (cellBounds (c->sceneRow, c->columnIdx));
        }
    }

    /* Track the most-recently launched scene so the entire row can
     * be drawn as the "current" scene (Ableton convention).  Repaint
     * the old + new row strips across the full width. */
    if (currentSceneRow_ != sceneRow)
    {
        const int prev = currentSceneRow_;
        currentSceneRow_ = sceneRow;
        auto repaintRow = [this] (int row)
        {
            if (row < 0 || row >= scenes_.size()) return;
            const auto sl = sceneLabelBounds (row);
            repaint (Rectangle<int> (0, sl.getY(), getWidth(), kRowH));
        };
        repaintRow (prev);
        repaintRow (sceneRow);
    }
}

void SessionView::applySceneOverridesToTransport (const SessionScene& s)
{
    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;

    /* Session::setProperty is protected -- go through the public
     * data() ValueTree directly.  Same effect: the audio engine's
     * ValueTree::Listener picks up the change and routes it to the
     * Transport monitor. */
    auto tree = sess->data();
    if (! tree.isValid()) return;
    auto* eng = services_->context().audio().get();

    /* Tempo: writing tags::tempo on the session ValueTree DOES
     * propagate to the audio engine -- AudioEngine has a
     * juce::Value listener bound to that property (see
     * audioengine.cpp connectSessionValues + valueChanged). */
    if (s.tempoOverride > 0.0)
        tree.setProperty (tags::tempo, s.tempoOverride, nullptr);

    /* Time signature: NO Value listener on tags::beatsPerBar or
     * tags::beatDivisor -- AudioEngine reads them ONCE in
     * connectSessionValues and never re-syncs.  So we have to call
     * AudioEngine::setMeter directly to actually move the transport.
     *
     * Also: the session XML uses BeatType encoding for beatDivisor
     * (0=whole, 2=quarter, 4=sixteenth) while our SessionScene
     * stores the user-meaningful denominator (4 = quarter, 8 =
     * eighth).  Convert via BeatType::fromDivisor. */
    if (s.beatsPerBar > 0 && s.beatDivisor > 0)
    {
        const int bd = BeatType::fromDivisor (s.beatDivisor);
        tree.setProperty (tags::beatsPerBar, s.beatsPerBar, nullptr);
        tree.setProperty (tags::beatDivisor, bd,            nullptr);
        if (eng != nullptr)
            eng->setMeter (s.beatsPerBar, bd);
    }
}

void SessionView::editSceneTempo (int sceneRow)
{
    /* Inline editor for fast in-place edits.  Scene View popup
     * (opened via the master cell edit button) is the full editor
     * for name + tempo + sig together. */
    if (sceneRow < 0 || sceneRow >= scenes_.size()) return;
    const auto& sc = scenes_.getReference (sceneRow);
    const auto initial = sc.tempoOverride > 0.0
                            ? juce::String (sc.tempoOverride, 2)
                            : juce::String();
    showInlineEditor (
        masterTempoFieldBounds (sceneRow),
        initial,
        [this, sceneRow] (const juce::String& txt)
        {
            if (sceneRow >= scenes_.size()) return;
            auto& s = scenes_.getReference (sceneRow);
            const auto trimmed = txt.trim();
            if (trimmed.isEmpty()) s.tempoOverride = -1.0;
            else
            {
                const double v = trimmed.getDoubleValue();
                s.tempoOverride = (v > 0.0) ? juce::jlimit (20.0, 999.0, v) : -1.0;
            }
            notifySceneEdited (sceneRow);
        });
}

void SessionView::editSceneSignature (int sceneRow)
{
    if (sceneRow < 0 || sceneRow >= scenes_.size()) return;
    const auto& sc = scenes_.getReference (sceneRow);
    const auto initial = (sc.beatsPerBar > 0 && sc.beatDivisor > 0)
                            ? juce::String (sc.beatsPerBar) + "/" + juce::String (sc.beatDivisor)
                            : juce::String();
    showInlineEditor (
        masterSigFieldBounds (sceneRow),
        initial,
        [this, sceneRow] (const juce::String& txt)
        {
            if (sceneRow >= scenes_.size()) return;
            auto& s = scenes_.getReference (sceneRow);
            const auto trimmed = txt.trim();
            if (trimmed.isEmpty())
            {
                s.beatsPerBar = 0;
                s.beatDivisor = 0;
            }
            else
            {
                const int slash = trimmed.indexOfChar ('/');
                int num = 0, den = 0;
                if (slash > 0)
                {
                    num = trimmed.substring (0, slash).getIntValue();
                    den = trimmed.substring (slash + 1).getIntValue();
                }
                else
                {
                    num = trimmed.getIntValue();
                    den = 4;
                }
                s.beatsPerBar = juce::jlimit (0, 32, num);
                s.beatDivisor = juce::jlimit (0, 32, den);
            }
            notifySceneEdited (sceneRow);
        });
}

void SessionView::notifySceneEdited (int sceneRow)
{
    /* Persist the mutation + repaint the visual surfaces that mirror
     * scene data (left scene label + master cell). */
    if (sceneRow < 0 || sceneRow >= scenes_.size()) return;
    writeToSession();
    refreshToolbarLabels();
    repaint (sceneLabelBounds  (sceneRow));
    repaint (masterCellBounds  (sceneRow));
}

SessionView::SessionScene* SessionView::sceneAt (int sceneRow) noexcept
{
    if (sceneRow < 0 || sceneRow >= scenes_.size()) return nullptr;
    return &scenes_.getReference (sceneRow);
}

void SessionView::transportTogglePlay() noexcept
{
    if (services_ == nullptr) return;
    if (auto* eng = services_->context().audio().get())
        eng->togglePlayPause();
}

void SessionView::bangClipByUuid (const juce::Uuid& clipId)
{
    for (auto* c : clips_)
    {
        if (c->id == clipId)
        {
            bangClip (*c);
            return;
        }
    }
}

bool SessionView::sceneHasActiveClip (int sceneRow) const noexcept
{
    /* "Active" here means CURRENTLY emitting -- only the Playing
     * state, not WaitingToStart or WaitingToStop.  Scenes whose
     * clips are queued to stop on the next bar shouldn't read as
     * "active" -- they're on their way out. */
    for (auto* c : clips_)
    {
        if (c->sceneRow != sceneRow) continue;
        if (c->state.load (std::memory_order_relaxed) == LiveState::Playing)
            return true;
    }
    return false;
}

bool SessionView::sceneHasQueuedClip (int sceneRow) const noexcept
{
    /* "Queued" = WaitingToStart -- clicked, waiting for the launch
     * quant boundary to fire.  Used to drive the pulsing visual on
     * the master button + row band so the user gets immediate
     * feedback on bang during the quant gap (Bar at 120 BPM 4/4 can
     * be up to 2 s of wait). */
    for (auto* c : clips_)
    {
        if (c->sceneRow != sceneRow) continue;
        if (c->state.load (std::memory_order_relaxed) == LiveState::WaitingToStart)
            return true;
    }
    return false;
}

void SessionView::showInlineEditor (juce::Rectangle<int> bounds,
                                    const juce::String& initial,
                                    std::function<void (const juce::String&)> commit)
{
    inlineEditorCommit_ = std::move (commit);
    inlineEditor_.setBounds (bounds);
    inlineEditor_.setText (initial, juce::dontSendNotification);
    inlineEditor_.setVisible (true);
    inlineEditor_.toFront (true);
    inlineEditor_.grabKeyboardFocus();
    inlineEditor_.selectAll();
}

void SessionView::hideInlineEditor()
{
    inlineEditorCommit_ = nullptr;
    inlineEditor_.setVisible (false);
}

void SessionView::clearSceneTempo (int sceneRow)
{
    if (sceneRow < 0 || sceneRow >= scenes_.size()) return;
    scenes_.getReference (sceneRow).tempoOverride = -1.0;
    writeToSession();
    repaint (masterCellBounds (sceneRow));
}

void SessionView::clearSceneSignature (int sceneRow)
{
    if (sceneRow < 0 || sceneRow >= scenes_.size()) return;
    auto& s = scenes_.getReference (sceneRow);
    s.beatsPerBar = 0;
    s.beatDivisor = 0;
    writeToSession();
    repaint (masterCellBounds (sceneRow));
}

void SessionView::stopAllClips()
{
    /* Immediate stop -- bypass quantisation.  Schedules through the
     * audio-thread FIFO with beatTarget=-1 so every clip stops on
     * the next render block.  Resets message-thread state to Stopped
     * straight away; UI tick will confirm.
     *
     * C.3: skip clips already at Stopped to avoid burning FIFO slots
     * on redundant stops. */
    for (auto* c : clips_)
    {
        if (c->state.load (std::memory_order_relaxed) == LiveState::Stopped) continue;
        if (c->kind == ClipKind::Tracker)
        {
            if (auto* trk = lookupTracker (c->trackerNodeId))
                trk->schedulePlaying (c->sequenceIdx, -1.0, false);
        }
        else
        {
            if (auto* mp = lookupMidiPlayer (c->midiPlayerNodeId))
                mp->schedulePlayingClip (c->midiSourceId, -1.0, false);
        }
        c->state.store (LiveState::Stopped, std::memory_order_relaxed);
    }
    repaint();
}

void SessionView::stopColumn (int columnIdx)
{
    /* Immediate stop for every active clip on a single column.
     * Same audio-thread path as stopAllClips, scoped to a column. */
    if (columnIdx < 0 || columnIdx >= columns_.size()) return;
    for (auto* c : clips_)
    {
        if (c->columnIdx != columnIdx) continue;
        const LiveState s = c->state.load (std::memory_order_relaxed);
        if (s == LiveState::Stopped) continue;
        if (c->kind == ClipKind::Tracker)
        {
            if (auto* trk = lookupTracker (c->trackerNodeId))
                trk->schedulePlaying (c->sequenceIdx, -1.0, false);
        }
        else
        {
            if (auto* mp = lookupMidiPlayer (c->midiPlayerNodeId))
                mp->schedulePlayingClip (c->midiSourceId, -1.0, false);
        }
        c->state.store (LiveState::Stopped, std::memory_order_relaxed);
    }
    repaint();
}

bool SessionView::isColumnMuted (int columnIdx) const noexcept
{
    /* Returns USER-asserted mute (the explicit press), not the
     * effective engine state.  Lives on the source node so the
     * tracker / piano-roll editor popups query the same state. */
    if (columnIdx < 0 || columnIdx >= columns_.size()) return false;
    const auto& col = columns_.getReference (columnIdx);
    if (col.kind == ClipKind::Tracker)
    {
        if (auto* trk = lookupTracker (col.trackerNodeId)) return trk->getUserMuted();
    }
    else
    {
        if (auto* mp = lookupMidiPlayer (col.midiPlayerNodeId)) return mp->getUserMuted();
    }
    return false;
}

bool SessionView::isColumnSoloed (int columnIdx) const noexcept
{
    if (columnIdx < 0 || columnIdx >= columns_.size()) return false;
    const auto& col = columns_.getReference (columnIdx);
    if (col.kind == ClipKind::Tracker)
    {
        if (auto* trk = lookupTracker (col.trackerNodeId)) return trk->getSoloed();
    }
    else
    {
        if (auto* mp = lookupMidiPlayer (col.midiPlayerNodeId)) return mp->getSoloed();
    }
    return false;
}

void SessionView::applyMuteAndSoloState()
{
    /* Scan source nodes via the columns array, decide if any are
     * soloed, then reconcile each node's effective mute from
     * (any-solo ? !this-soloed : userMuted).  Engine mute is the
     * EFFECTIVE state; user-intent flags live on the node. */
    bool anySolo = false;
    for (int i = 0; i < columns_.size(); ++i)
    {
        const auto& col = columns_.getReference (i);
        if (col.kind == ClipKind::Tracker)
        {
            if (auto* trk = lookupTracker (col.trackerNodeId))
                if (trk->getSoloed()) { anySolo = true; break; }
        }
        else
        {
            if (auto* mp = lookupMidiPlayer (col.midiPlayerNodeId))
                if (mp->getSoloed()) { anySolo = true; break; }
        }
    }

    for (int c = 0; c < columns_.size(); ++c)
    {
        const auto& col = columns_.getReference (c);
        if (col.kind == ClipKind::Tracker)
        {
            if (auto* trk = lookupTracker (col.trackerNodeId))
            {
                const bool effectiveMute = anySolo ? ! trk->getSoloed()
                                                   : trk->getUserMuted();
                if (trk->isMuted() != effectiveMute)
                    trk->setMuted (effectiveMute);
            }
        }
        else
        {
            if (auto* mp = lookupMidiPlayer (col.midiPlayerNodeId))
            {
                const bool effectiveMute = anySolo ? ! mp->getSoloed()
                                                   : mp->getUserMuted();
                if (mp->isMuted() != effectiveMute)
                    mp->setMuted (effectiveMute);
            }
        }
    }
}

void SessionView::toggleColumnMute (int columnIdx)
{
    if (columnIdx < 0 || columnIdx >= columns_.size()) return;
    const auto& col = columns_.getReference (columnIdx);
    if (col.kind == ClipKind::Tracker)
    {
        if (auto* trk = lookupTracker (col.trackerNodeId))
            trk->setUserMuted (! trk->getUserMuted());
    }
    else
    {
        if (auto* mp = lookupMidiPlayer (col.midiPlayerNodeId))
            mp->setUserMuted (! mp->getUserMuted());
    }
    applyMuteAndSoloState();
    repaint (headerRowBounds());
}

void SessionView::toggleColumnSolo (int columnIdx)
{
    if (columnIdx < 0 || columnIdx >= columns_.size()) return;
    const auto& col = columns_.getReference (columnIdx);
    if (col.kind == ClipKind::Tracker)
    {
        if (auto* trk = lookupTracker (col.trackerNodeId))
            trk->setSoloed (! trk->getSoloed());
    }
    else
    {
        if (auto* mp = lookupMidiPlayer (col.midiPlayerNodeId))
            mp->setSoloed (! mp->getSoloed());
    }
    applyMuteAndSoloState();
    repaint (headerRowBounds());
}

void SessionView::addScene()
{
    SessionScene s;
    s.id   = Uuid();
    s.name = "Scene " + String (scenes_.size() + 1);
    scenes_.add (s);
    writeToSession();
    refreshToolbarLabels();
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
    refreshToolbarLabels();
    repaint();
}

void SessionView::renameScene (int row)
{
    /* Rename routes through the Scene View popup, per user request:
     * one window for all scene properties instead of three separate
     * AlertWindow / inline-editor flows. */
    openSceneView (row);
}

void SessionView::renameClip (SessionClip& clip)
{
    auto* aw = new juce::AlertWindow ("Rename clip",
                                      "New name for clip:",
                                      juce::AlertWindow::NoIcon);
    aw->addTextEditor ("name", clip.name);
    aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    SessionClip* clipPtr = &clip;
    aw->enterModalState (true,
        juce::ModalCallbackFunction::create (
            [this, clipPtr, aw] (int result) {
                /* Verify the clip pointer is still in clips_ -- user
                 * may have deleted it while the dialog was open. */
                bool stillValid = false;
                for (auto* c : clips_)
                    if (c == clipPtr) { stillValid = true; break; }
                if (result == 1 && stillValid)
                {
                    const auto newName = aw->getTextEditorContents ("name").trim();
                    if (newName.isNotEmpty())
                    {
                        clipPtr->name = newName;
                        writeToSession();
                        repaint (cellBounds (clipPtr->sceneRow, clipPtr->columnIdx));
                    }
                }
            }),
        true);
}

void SessionView::cycleClipColor (SessionClip& clip)
{
    /* Cycle through the tracker-tint palette (8 colours, defined at
     * the top of this file).  v1 leaves full ColourSelector for a
     * later polish pass -- the cycle gives the user enough colour
     * differentiation between clips without juggling a modal. */
    int curIdx = -1;
    for (int i = 0; i < 8; ++i)
        if (kColumnTints[i].getARGB() == clip.color.getARGB())
        {
            curIdx = i;
            break;
        }
    const Colour next = kColumnTints[(curIdx + 1) % 8];
    clip.color = next.withMultipliedSaturation (0.6f).withMultipliedBrightness (0.85f);
    writeToSession();
    repaint (cellBounds (clip.sceneRow, clip.columnIdx));
}

void SessionView::moveClip (SessionClip& clip, int newSceneRow, int newColumnIdx)
{
    if (newSceneRow == clip.sceneRow && newColumnIdx == clip.columnIdx) return;
    if (newSceneRow < 0 || newSceneRow >= scenes_.size()) return;
    if (newColumnIdx < 0 || newColumnIdx >= columns_.size()) return;
    if (findClip (newSceneRow, newColumnIdx) != nullptr) return;  // target busy

    const auto& targetCol = columns_.getReference (newColumnIdx);
    /* Reject cross-kind moves: a tracker clip can't land on a MIDI
     * column and vice versa.  The source data shapes are not
     * interchangeable. */
    if (targetCol.kind != clip.kind) return;
    if (clip.kind == ClipKind::Tracker)
    {
        /* Cross-tracker migration would require re-parenting a vht
         * sequence; deferred.  v1: silently reject cross-tracker drops. */
        if (targetCol.trackerNodeId != clip.trackerNodeId) return;
    }
    else
    {
        /* Cross-MidiPlayer move would require re-parenting the
         * MidiNoteRegion to a different player; deferred. */
        if (targetCol.midiPlayerNodeId != clip.midiPlayerNodeId) return;
    }

    const int oldRow = clip.sceneRow;
    const int oldCol = clip.columnIdx;
    clip.sceneRow  = newSceneRow;
    clip.columnIdx = newColumnIdx;

    writeToSession();
    repaint (cellBounds (oldRow,        oldCol));
    repaint (cellBounds (newSceneRow,   newColumnIdx));
}

void SessionView::copyClip (SessionClip& src, int newSceneRow, int newColumnIdx)
{
    if (newSceneRow < 0 || newSceneRow >= scenes_.size()) return;
    if (newColumnIdx < 0 || newColumnIdx >= columns_.size()) return;
    if (findClip (newSceneRow, newColumnIdx) != nullptr) return;

    const auto& targetCol = columns_.getReference (newColumnIdx);
    if (targetCol.kind != src.kind) return;

    auto* clip = new SessionClip();
    clip->id            = juce::Uuid();
    clip->name          = src.name;
    clip->color         = src.color;
    clip->kind          = src.kind;
    clip->sceneRow      = newSceneRow;
    clip->columnIdx     = newColumnIdx;
    clip->launchQuant   = src.launchQuant;
    clip->followAction  = src.followAction;

    if (src.kind == ClipKind::Tracker)
    {
        /* Cross-tracker copy needs deferred adoptSequence API. */
        if (targetCol.trackerNodeId != src.trackerNodeId) { delete clip; return; }
        auto* trk = lookupTracker (src.trackerNodeId);
        if (trk == nullptr) { delete clip; return; }
        const int newIdx = trk->cloneSequence (src.sequenceIdx);
        if (newIdx < 0) { delete clip; return; }
        clip->trackerNodeId = src.trackerNodeId;
        clip->sequenceIdx   = newIdx;
    }
    else
    {
        /* Cross-player copy needs region adoption; deferred. */
        if (targetCol.midiPlayerNodeId != src.midiPlayerNodeId)
            { delete clip; return; }
        auto* mp = lookupMidiPlayer (src.midiPlayerNodeId);
        if (mp == nullptr) { delete clip; return; }
        /* Create a fresh slot, then deep-clone the source region's
         * notes into it.  MidiNoteRegion::clone() preserves the
         * region id; we want a NEW id for the new clip, so we copy
         * notes via setNotes after creating. */
        const Uuid newId = mp->createSessionClip();
        if (newId.isNull()) { delete clip; return; }
        if (auto* dst = mp->getClipRegion (newId))
        {
            if (auto* sr = mp->getClipRegion (src.midiSourceId))
            {
                dst->positionBeats   = sr->positionBeats;
                dst->lengthBeats     = sr->lengthBeats;
                dst->startBeats      = sr->startBeats;
                dst->looped          = sr->looped;
                dst->loopLengthBeats = sr->loopLengthBeats;
                dst->colour          = sr->colour;
                dst->name            = sr->name;
                if (const auto* snap = sr->loadSnapshot())
                    dst->setNotes (MidiNoteRegion::NoteList (*snap));
            }
        }
        clip->midiPlayerNodeId = src.midiPlayerNodeId;
        clip->midiSourceId     = newId;
    }

    clips_.add (clip);

    writeToSession();
    repaint (cellBounds (newSceneRow, newColumnIdx));
}

void SessionView::reorderScene (int fromRow, int toRow)
{
    if (fromRow < 0 || fromRow >= scenes_.size()) return;
    if (toRow   < 0 || toRow   >= scenes_.size()) return;
    if (fromRow == toRow) return;

    /* Move the scene struct; remap every clip's sceneRow so they
     * stay attached to their visual row. */
    auto moved = scenes_.removeAndReturn (fromRow);
    scenes_.insert (toRow, moved);

    /* Remap clip sceneRows.  Whatever was at fromRow now lives at
     * toRow; rows between shift.  Easier to just rebuild each
     * clip's sceneRow by remapping using the old -> new permutation. */
    if (fromRow < toRow)
    {
        for (auto* c : clips_)
        {
            if (c->sceneRow == fromRow) c->sceneRow = toRow;
            else if (c->sceneRow > fromRow && c->sceneRow <= toRow) --c->sceneRow;
        }
    }
    else
    {
        for (auto* c : clips_)
        {
            if (c->sceneRow == fromRow) c->sceneRow = toRow;
            else if (c->sceneRow >= toRow && c->sceneRow < fromRow) ++c->sceneRow;
        }
    }

    writeToSession();
    repaint();
}

void SessionView::assignExistingPattern (int sceneRow, int columnIdx, int sequenceIdx)
{
    if (sceneRow < 0 || sceneRow >= scenes_.size()) return;
    if (columnIdx < 0 || columnIdx >= columns_.size()) return;
    if (findClip (sceneRow, columnIdx) != nullptr) return;
    auto* trk = lookupTracker (columns_.getReference (columnIdx).trackerNodeId);
    if (trk == nullptr) return;
    if (sequenceIdx < 0 || sequenceIdx >= trk->numPatterns()) return;

    auto* clip = new SessionClip();
    clip->id            = juce::Uuid();
    clip->name          = "Clip " + String (clips_.size() + 1);
    clip->color         = columnTint (columnIdx).withMultipliedSaturation (0.6f)
                                                .withMultipliedBrightness (0.85f);
    clip->trackerNodeId = columns_.getReference (columnIdx).trackerNodeId;
    clip->sequenceIdx   = sequenceIdx;
    clip->sceneRow      = sceneRow;
    clip->columnIdx     = columnIdx;
    clip->launchQuant   = defaultLaunchQuant_;
    clips_.add (clip);

    writeToSession();
    repaint (cellBounds (sceneRow, columnIdx));
}

void SessionView::pickClipColour (SessionClip& clip)
{
    /* Bind this clip to the picker for the duration of the modal.
     * ColourSelector broadcasts changes via ChangeBroadcaster; our
     * changeListenerCallback applies them live. */
    colourPickerClip_ = &clip;

    auto* cs = new juce::ColourSelector (juce::ColourSelector::showColourspace
                                         | juce::ColourSelector::showSliders
                                         | juce::ColourSelector::showAlphaChannel);
    cs->setSize (320, 360);
    cs->setCurrentColour (clip.color);
    cs->addChangeListener (this);

    /* Show in a CallOutBox anchored on the clip cell, in screen
     * coords.  CallOutBox takes ownership of the selector and is
     * self-deleting on dismiss. */
    const auto screenAnchor = localAreaToGlobal (cellBounds (clip.sceneRow, clip.columnIdx));
    juce::CallOutBox::launchAsynchronously (
        std::unique_ptr<juce::Component> (cs),
        screenAnchor,
        nullptr);
}

void SessionView::changeListenerCallback (juce::ChangeBroadcaster* src)
{
    if (colourPickerClip_ == nullptr) return;
    auto* cs = dynamic_cast<juce::ColourSelector*> (src);
    if (cs == nullptr) return;

    /* Confirm the clip is still alive -- user may have deleted it
     * while the picker was open. */
    bool stillValid = false;
    for (auto* c : clips_)
        if (c == colourPickerClip_) { stillValid = true; break; }
    if (! stillValid) { colourPickerClip_ = nullptr; return; }

    colourPickerClip_->color = cs->getCurrentColour();
    writeToSession();
    repaint (cellBounds (colourPickerClip_->sceneRow,
                         colourPickerClip_->columnIdx));
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
                trk->schedulePlaying (clips_[i]->sequenceIdx, -1.0, false);
            clips_.remove (i);
        }
    }

    /* Shift remaining clips above the deleted row up by 1. */
    for (auto* c : clips_)
        if (c->sceneRow > row)
            --c->sceneRow;

    scenes_.remove (row);
    writeToSession();
    refreshToolbarLabels();
    repaint();
}

void SessionView::addClipAt (int sceneRow, int columnIdx)
{
    if (sceneRow < 0 || sceneRow >= scenes_.size())   return;
    if (columnIdx < 0 || columnIdx >= columns_.size()) return;
    if (findClip (sceneRow, columnIdx) != nullptr)     return;  // already populated

    auto& col = columns_.getReference (columnIdx);
    auto* clip = new SessionClip();
    clip->id   = Uuid();
    clip->name = "Clip " + String (sceneRow + 1);
    clip->color = columnTint (columnIdx).withMultipliedSaturation (0.6f)
                                        .withMultipliedBrightness (0.85f);
    clip->kind          = col.kind;
    clip->sceneRow      = sceneRow;
    clip->columnIdx     = columnIdx;
    clip->launchQuant   = defaultLaunchQuant_;

    if (col.kind == ClipKind::Tracker)
    {
        auto* trk = lookupTracker (col.trackerNodeId);
        if (trk == nullptr) { delete clip; return; }
        const int seqIdx = trk->createSequence (16);
        if (seqIdx < 0) { delete clip; return; }
        clip->trackerNodeId = col.trackerNodeId;
        clip->sequenceIdx   = seqIdx;
    }
    else /* Midi */
    {
        auto* mp = lookupMidiPlayer (col.midiPlayerNodeId);
        if (mp == nullptr) { delete clip; return; }
        const Uuid clipSourceId = mp->createSessionClip();
        if (clipSourceId.isNull()) { delete clip; return; }
        clip->midiPlayerNodeId = col.midiPlayerNodeId;
        clip->midiSourceId     = clipSourceId;
    }

    clips_.add (clip);

    writeToSession();
    repaint (cellBounds (sceneRow, columnIdx));
}

/* Self-deleting non-modal floating window for the pattern editor.
 * juce::DialogWindow::launchAsync enters a modal state that steals
 * keyboard focus from the main app -- user can't hit spacebar for
 * transport or interact with the underlying SessionView.  A plain
 * DocumentWindow with the close button wired to delete-self keeps
 * the editor floating without capturing focus. */
class TrackerPatternWindow : public juce::DocumentWindow
{
public:
    TrackerPatternWindow (juce::Component::SafePointer<SessionView> view,
                          const juce::Uuid& clipId,
                          juce::Component* content,
                          const juce::String& title)
        : juce::DocumentWindow (title,
                                juce::Colour { 0xff'18'18'18 },
                                juce::DocumentWindow::allButtons),
          owner_  (view),
          clipId_ (clipId)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content, true);
        setResizable (true, false);
        centreWithSize (820, 540);
        setWantsKeyboardFocus (true);
        setVisible (true);
    }
    void closeButtonPressed() override { delete this; }

    /* Transport keyboard hooks while the tracker popup is focused:
     *   Space       -> global transport play / pause toggle
     *   Ctrl+Space  -> Sketch-Mode launch of THIS clip (bangClip starts
     *                  the transport if stopped, fires the clip
     *                  through the audio FIFO).  Both are no-ops if
     *                  the underlying SessionView has been torn down. */
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (auto* v = owner_.getComponent())
        {
            if (key == juce::KeyPress::spaceKey
                && key.getModifiers().isCtrlDown())
            {
                v->bangClipByUuid (clipId_);
                return true;
            }
            if (key == juce::KeyPress::spaceKey)
            {
                v->transportTogglePlay();
                return true;
            }
        }
        return juce::DocumentWindow::keyPressed (key);
    }

private:
    juce::Component::SafePointer<SessionView> owner_;
    juce::Uuid clipId_;
};

/* === SceneView popup ===================================================
 * Dedicated floating window for editing scene properties (name, tempo
 * override, time-signature override).  Replaces the inline tempo/sig
 * editors AND the rename AlertWindow with a single non-modal surface
 * the user opens once and edits in.
 *
 * Per feedback_inline_edit_cancel_on_click_away: Return commits a
 * field, Escape + focus-loss revert.  Per the dedicated-window pattern
 * the popup is non-modal so the user can interact with the main app
 * (transport, other scenes) while it's open.  Window is self-deleting
 * via the close button.  SafePointer<SessionView> guards against the
 * view being torn down while the window is open. */
class SceneViewContent : public juce::Component,
                         private juce::TextEditor::Listener
{
public:
    SceneViewContent (juce::Component::SafePointer<SessionView> view, int sceneRow)
        : view_ (view), sceneRow_ (sceneRow)
    {
        setSize (320, 200);

        configureLabel (titleLabel_,  "Scene " + juce::String (sceneRow + 1),
                        14.0f, juce::Font::bold);
        configureLabel (nameLabel_,   "Name",      11.0f, juce::Font::plain);
        configureLabel (tempoLabel_,  "Tempo",     11.0f, juce::Font::plain);
        configureLabel (sigLabel_,    "Signature", 11.0f, juce::Font::plain);
        configureLabel (sigSlashLbl_, "/",         12.0f, juce::Font::bold);
        configureLabel (hintLabel_,
            "Tempo / sig left blank = inherit session value.",
            10.0f, juce::Font::plain);
        hintLabel_.setColour (juce::Label::textColourId, juce::Colour { 0xff'70'70'70 });

        configureEditor (nameEditor_);
        configureEditor (tempoEditor_);
        configureEditor (sigNumEditor_);
        configureEditor (sigDenEditor_);

        nameEditor_  .setInputRestrictions (0);
        tempoEditor_ .setInputRestrictions (8,   "0123456789.");
        sigNumEditor_.setInputRestrictions (2,   "0123456789");
        sigDenEditor_.setInputRestrictions (2,   "0123456789");

        refreshFromScene();
    }

    void resized() override
    {
        const int margin = 12;
        const int rowH   = 24;
        const int labelW = 78;
        const int colGap = 8;

        auto r = getLocalBounds().reduced (margin);
        titleLabel_.setBounds (r.removeFromTop (rowH));
        r.removeFromTop (6);

        auto layoutRow = [&] (juce::Label& lab, juce::Component& field) {
            auto row = r.removeFromTop (rowH);
            lab.setBounds (row.removeFromLeft (labelW));
            row.removeFromLeft (colGap);
            field.setBounds (row);
            r.removeFromTop (4);
        };

        layoutRow (nameLabel_,  nameEditor_);
        layoutRow (tempoLabel_, tempoEditor_);

        /* Signature row -- two narrow fields with a / between. */
        {
            auto row = r.removeFromTop (rowH);
            sigLabel_.setBounds (row.removeFromLeft (labelW));
            row.removeFromLeft (colGap);
            const int fieldW = 46;
            sigNumEditor_.setBounds (row.removeFromLeft (fieldW));
            row.removeFromLeft (6);
            sigSlashLbl_.setBounds (row.removeFromLeft (10));
            row.removeFromLeft (6);
            sigDenEditor_.setBounds (row.removeFromLeft (fieldW));
            r.removeFromTop (4);
        }

        r.removeFromTop (6);
        hintLabel_.setBounds (r.removeFromTop (rowH));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour { 0xff'18'18'18 });
    }

private:
    void configureLabel (juce::Label& l, const juce::String& text,
                         float pt, int style)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (monoFont (
                                      pt, style));
        l.setColour (juce::Label::textColourId, juce::Colour { 0xff'c0'c0'c0 });
        addAndMakeVisible (l);
    }

    void configureEditor (juce::TextEditor& e)
    {
        e.setMultiLine (false);
        e.setReturnKeyStartsNewLine (false);
        e.setSelectAllWhenFocused (true);
        e.setFont (monoFont (
                                      12.0f, juce::Font::plain));
        e.setColour (juce::TextEditor::backgroundColourId, juce::Colour { 0xff'20'20'20 });
        e.setColour (juce::TextEditor::textColourId,       juce::Colour { 0xff'ff'ff'ff });
        e.setColour (juce::TextEditor::outlineColourId,    juce::Colour { 0xff'3a'3a'3a });
        e.setColour (juce::TextEditor::focusedOutlineColourId,
                                                            juce::Colour { 0xff'ff'a0'40 });
        e.addListener (this);
        addAndMakeVisible (e);
    }

    void refreshFromScene()
    {
        auto* v = view_.getComponent();
        if (v == nullptr) return;
        auto* sc = v->sceneAt (sceneRow_);
        if (sc == nullptr) return;

        nameEditor_.setText (sc->name, juce::dontSendNotification);
        tempoEditor_.setText (sc->tempoOverride > 0.0
                                  ? juce::String (sc->tempoOverride, 2)
                                  : juce::String(),
                              juce::dontSendNotification);
        sigNumEditor_.setText (sc->beatsPerBar > 0
                                   ? juce::String (sc->beatsPerBar)
                                   : juce::String(),
                               juce::dontSendNotification);
        sigDenEditor_.setText (sc->beatDivisor > 0
                                   ? juce::String (sc->beatDivisor)
                                   : juce::String(),
                               juce::dontSendNotification);

    }

    void textEditorReturnKeyPressed (juce::TextEditor& e) override
    {
        commit (e);
    }

    void textEditorEscapeKeyPressed (juce::TextEditor& e) override
    {
        juce::ignoreUnused (e);
        refreshFromScene();   // revert
    }

    void textEditorFocusLost (juce::TextEditor& e) override
    {
        /* Per feedback_inline_edit_cancel_on_click_away: focus loss
         * REVERTS, doesn't commit.  User has to press Return to lock
         * the value in. */
        juce::ignoreUnused (e);
        refreshFromScene();
    }

    void commit (juce::TextEditor& e)
    {
        auto* v = view_.getComponent();
        if (v == nullptr) return;
        auto* sc = v->sceneAt (sceneRow_);
        if (sc == nullptr) return;

        if (&e == &nameEditor_)
        {
            const auto n = e.getText().trim();
            if (n.isNotEmpty()) sc->name = n;
        }
        else if (&e == &tempoEditor_)
        {
            const auto t = e.getText().trim();
            if (t.isEmpty()) { sc->tempoOverride = -1.0; }
            else
            {
                const double bpm = t.getDoubleValue();
                sc->tempoOverride = (bpm > 0.0)
                    ? juce::jlimit (20.0, 999.0, bpm)
                    : -1.0;
            }
        }
        else if (&e == &sigNumEditor_)
        {
            const auto n = e.getText().trim();
            sc->beatsPerBar = n.isEmpty() ? 0
                                          : juce::jlimit (0, 32, n.getIntValue());
        }
        else if (&e == &sigDenEditor_)
        {
            const auto d = e.getText().trim();
            sc->beatDivisor = d.isEmpty() ? 0
                                          : juce::jlimit (0, 32, d.getIntValue());
        }

        v->notifySceneEdited (sceneRow_);
        refreshFromScene();
    }

    juce::Component::SafePointer<SessionView> view_;
    int sceneRow_;

    juce::Label titleLabel_, nameLabel_, tempoLabel_, sigLabel_, sigSlashLbl_, hintLabel_;
    juce::TextEditor nameEditor_, tempoEditor_, sigNumEditor_, sigDenEditor_;
};

class SceneViewWindow : public juce::DocumentWindow
{
public:
    SceneViewWindow (juce::Component* content, const juce::String& title)
        : juce::DocumentWindow (title,
                                juce::Colour { 0xff'18'18'18 },
                                juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content, true);
        setResizable (false, false);
        centreWithSize (340, 220);
        setVisible (true);
    }
    void closeButtonPressed() override { delete this; }
};

/* === ClipView popup ====================================================
 * Mirror of SceneView for clips.  Replaces the right-click Rename /
 * Cycle colour / Colour... / Launch quantisation submenu / Follow
 * action submenu items with a single dedicated window.  Right-click
 * still carries the STRUCTURAL ops (Add new / Assign existing /
 * Delete clip).
 *
 * SafePointer<SessionView> + clip Uuid for lifetime safety; per
 * feedback_inline_edit_cancel_on_click_away the name TextEditor
 * commits on Return, reverts on Escape + focus loss. */
class ClipViewContent : public juce::Component,
                        private juce::TextEditor::Listener,
                        private juce::ChangeListener
{
public:
    ClipViewContent (juce::Component::SafePointer<SessionView> view,
                     const juce::Uuid& clipId)
        : view_ (view), clipId_ (clipId)
    {
        setSize (340, 240);

        configureLabel (titleLabel_, "Clip", 14.0f, juce::Font::bold);
        configureLabel (nameLabel_,   "Name",       11.0f, juce::Font::plain);
        configureLabel (colourLabel_, "Colour",     11.0f, juce::Font::plain);
        configureLabel (quantLabel_,  "Quant",      11.0f, juce::Font::plain);
        configureLabel (followLabel_, "Follow",     11.0f, juce::Font::plain);
        configureLabel (hintLabel_,
            "Click the colour swatch to pick a new colour.",
            10.0f, juce::Font::plain);
        hintLabel_.setColour (juce::Label::textColourId, juce::Colour { 0xff'70'70'70 });

        configureEditor (nameEditor_);
        nameEditor_.setInputRestrictions (0);

        configureCombo (quantCombo_,  { "Off", "1 Beat", "1 Bar", "2 Bars", "4 Bars" });
        configureCombo (followCombo_, { "None (loop)", "Stop", "Restart", "Next clip", "First clip" });

        addAndMakeVisible (swatch_);
        swatch_.onClick = [this] { openColourPicker(); };

        refreshFromClip();
    }

    ~ClipViewContent() override { detachColourPicker(); }

    void resized() override
    {
        const int margin = 12;
        const int rowH   = 24;
        const int labelW = 78;
        const int colGap = 8;

        auto r = getLocalBounds().reduced (margin);
        titleLabel_.setBounds (r.removeFromTop (rowH));
        r.removeFromTop (6);

        auto layoutRow = [&] (juce::Label& lab, juce::Component& field) {
            auto row = r.removeFromTop (rowH);
            lab.setBounds (row.removeFromLeft (labelW));
            row.removeFromLeft (colGap);
            field.setBounds (row);
            r.removeFromTop (4);
        };

        layoutRow (nameLabel_,   nameEditor_);

        /* Colour row: label + swatch (24x16) + room. */
        {
            auto row = r.removeFromTop (rowH);
            colourLabel_.setBounds (row.removeFromLeft (labelW));
            row.removeFromLeft (colGap);
            swatch_.setBounds (row.removeFromLeft (60).withSizeKeepingCentre (60, 18));
            r.removeFromTop (4);
        }

        layoutRow (quantLabel_,  quantCombo_);
        layoutRow (followLabel_, followCombo_);

        r.removeFromTop (6);
        hintLabel_.setBounds (r.removeFromTop (rowH));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour { 0xff'18'18'18 });
    }

private:
    void configureLabel (juce::Label& l, const juce::String& text,
                         float pt, int style)
    {
        l.setText (text, juce::dontSendNotification);
        l.setFont (monoFont (
                                      pt, style));
        l.setColour (juce::Label::textColourId, juce::Colour { 0xff'c0'c0'c0 });
        addAndMakeVisible (l);
    }

    void configureEditor (juce::TextEditor& e)
    {
        e.setMultiLine (false);
        e.setReturnKeyStartsNewLine (false);
        e.setSelectAllWhenFocused (true);
        e.setFont (monoFont (
                                      12.0f, juce::Font::plain));
        e.setColour (juce::TextEditor::backgroundColourId, juce::Colour { 0xff'20'20'20 });
        e.setColour (juce::TextEditor::textColourId,       juce::Colour { 0xff'ff'ff'ff });
        e.setColour (juce::TextEditor::outlineColourId,    juce::Colour { 0xff'3a'3a'3a });
        e.setColour (juce::TextEditor::focusedOutlineColourId,
                                                            juce::Colour { 0xff'ff'a0'40 });
        e.addListener (this);
        addAndMakeVisible (e);
    }

    void configureCombo (juce::ComboBox& cb, const juce::StringArray& items)
    {
        for (int i = 0; i < items.size(); ++i)
            cb.addItem (items[i], i + 1);     // 1-based
        cb.setColour (juce::ComboBox::backgroundColourId, juce::Colour { 0xff'20'20'20 });
        cb.setColour (juce::ComboBox::textColourId,       juce::Colour { 0xff'ff'ff'ff });
        cb.setColour (juce::ComboBox::outlineColourId,    juce::Colour { 0xff'3a'3a'3a });
        cb.onChange = [this, &cb] { commitCombo (cb); };
        addAndMakeVisible (cb);
    }

    void refreshFromClip()
    {
        auto* v = view_.getComponent();
        if (v == nullptr) return;
        auto* clip = v->clipByUuid (clipId_);
        if (clip == nullptr) return;

        titleLabel_.setText ("Clip - " + clip->name, juce::dontSendNotification);
        nameEditor_.setText (clip->name, juce::dontSendNotification);
        swatch_.setSwatchColour (clip->color);
        quantCombo_ .setSelectedId ((int) clip->launchQuant  + 1, juce::dontSendNotification);
        followCombo_.setSelectedId ((int) clip->followAction + 1, juce::dontSendNotification);
    }

    /* TextEditor::Listener */
    void textEditorReturnKeyPressed (juce::TextEditor& e) override
    {
        if (&e != &nameEditor_) return;
        if (auto* v = view_.getComponent())
            if (auto* clip = v->clipByUuid (clipId_))
            {
                const auto n = e.getText().trim();
                if (n.isNotEmpty()) clip->name = n;
                v->notifyClipEdited (*clip);
            }
        refreshFromClip();
    }
    void textEditorEscapeKeyPressed (juce::TextEditor&) override { refreshFromClip(); }
    void textEditorFocusLost (juce::TextEditor&) override         { refreshFromClip(); }

    /* ComboBox commit */
    void commitCombo (juce::ComboBox& cb)
    {
        auto* v = view_.getComponent();
        if (v == nullptr) return;
        auto* clip = v->clipByUuid (clipId_);
        if (clip == nullptr) return;
        const int idx = cb.getSelectedId() - 1;
        if (idx < 0) return;
        if (&cb == &quantCombo_)
            clip->launchQuant = static_cast<SessionView::LaunchQuant> (juce::jlimit (0, 4, idx));
        else if (&cb == &followCombo_)
            clip->followAction = static_cast<SessionView::FollowAction> (juce::jlimit (0, 4, idx));
        v->notifyClipEdited (*clip);
    }

    /* --- Colour picker ----------------------------------------------- */

    class Swatch : public juce::Component
    {
    public:
        std::function<void()> onClick;
        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour { 0xff'10'10'10 });
            g.setColour (current_);
            g.fillRect (getLocalBounds().reduced (2));
            g.setColour (juce::Colour { 0xff'3a'3a'3a });
            g.drawRect (getLocalBounds().reduced (2), 1);
        }
        void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
        void setSwatchColour (juce::Colour c) { current_ = c; repaint(); }
    private:
        juce::Colour current_ { 0xff'4a'7a'b5 };
    };

    void openColourPicker()
    {
        detachColourPicker();
        auto* cs = new juce::ColourSelector (juce::ColourSelector::showColourspace
                                             | juce::ColourSelector::showSliders
                                             | juce::ColourSelector::showAlphaChannel);
        cs->setSize (300, 360);
        if (auto* v = view_.getComponent())
            if (auto* clip = v->clipByUuid (clipId_))
                cs->setCurrentColour (clip->color);
        cs->addChangeListener (this);
        activeColourSel_ = cs;
        juce::CallOutBox::launchAsynchronously (
            std::unique_ptr<juce::Component> (cs),
            swatch_.getScreenBounds(),
            nullptr);
    }

    void detachColourPicker()
    {
        if (activeColourSel_ != nullptr)
            activeColourSel_->removeChangeListener (this);
        activeColourSel_ = nullptr;
    }

    void changeListenerCallback (juce::ChangeBroadcaster* src) override
    {
        auto* cs = dynamic_cast<juce::ColourSelector*> (src);
        if (cs == nullptr) return;
        auto* v = view_.getComponent();
        if (v == nullptr) return;
        auto* clip = v->clipByUuid (clipId_);
        if (clip == nullptr) return;
        clip->color = cs->getCurrentColour();
        swatch_.setSwatchColour (clip->color);
        v->notifyClipEdited (*clip);
    }

    juce::Component::SafePointer<SessionView> view_;
    juce::Uuid clipId_;

    juce::Label titleLabel_, nameLabel_, colourLabel_, quantLabel_, followLabel_, hintLabel_;
    juce::TextEditor nameEditor_;
    juce::ComboBox quantCombo_, followCombo_;
    Swatch swatch_;
    juce::ColourSelector* activeColourSel_ = nullptr;
};

class ClipViewWindow : public juce::DocumentWindow
{
public:
    ClipViewWindow (juce::Component* content, const juce::String& title)
        : juce::DocumentWindow (title,
                                juce::Colour { 0xff'18'18'18 },
                                juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content, true);
        setResizable (false, false);
        centreWithSize (360, 270);
        setVisible (true);
    }
    void closeButtonPressed() override { delete this; }
};

/* openSceneView + openClipView are defined here so their content +
 * window classes are complete types at the point of `new`.  All
 * other SessionView method definitions live above. */

void SessionView::openClipView (SessionClip& clip)
{
    auto* content = new ClipViewContent (this, clip.id);
    auto* win = new ClipViewWindow (content, "Clip - " + clip.name);
    juce::ignoreUnused (win);
}

void SessionView::notifyClipEdited (SessionClip& clip)
{
    writeToSession();
    repaint (cellBounds (clip.sceneRow, clip.columnIdx));
}

SessionView::SessionClip* SessionView::clipByUuid (const juce::Uuid& clipId) noexcept
{
    for (auto* c : clips_)
        if (c->id == clipId)
            return c;
    return nullptr;
}

void SessionView::openSceneView (int sceneRow)
{
    if (sceneRow < 0 || sceneRow >= scenes_.size()) return;
    auto* content = new SceneViewContent (this, sceneRow);
    auto* win = new SceneViewWindow (content,
                                     "Scene " + juce::String (sceneRow + 1));
    juce::ignoreUnused (win);   // self-deletes on close
}

void SessionView::openTrackerDockForClip (SessionClip& clip)
{
    /* Route to the right-side TrackerSideDock + jump to the clip's
     * pattern.  Used by single-click on the clip's edit button and
     * by double-click on the clip body.  The floating
     * TrackerPatternWindow (openPatternEditor) is reserved for an
     * explicit "open in window" action that we may add to the
     * right-click menu later. */
    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    auto graph = sess->getActiveGraph();
    if (! graph.isValid()) return;
    Node n = graph.getNodeById (clip.trackerNodeId);
    if (! n.isValid()) return;
    if (dynamic_cast<TrackerNode*> (n.getObject()) == nullptr) return;

    if (auto* sc = dynamic_cast<StandardContent*> (
            ViewHelpers::findContentComponent (this)))
        sc->showTrackerDockForNode (n.getUuid(), clip.sequenceIdx);
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

    /* Jump to the clip's sequence index -- TrackerEditor exposes only
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

    /* ASCII hyphen in the title -- native title bars on some systems
     * (Wine here, plus a few Linux WMs) garble multi-byte UTF-8 chars
     * when the title is set as a system call.  The em-dash showed up
     * as "a" in testing. */
    auto* win = new TrackerPatternWindow (this, clip.id, editor,
                                          "Tracker - " + clip.name);
    juce::ignoreUnused (win);   // self-deletes on close
}

void SessionView::addTrackerColumn()
{
    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    auto graph = sess->getActiveGraph();
    if (! graph.isValid()) return;
    auto* engine = services_->find<EngineService>();
    if (engine == nullptr) return;

    juce::PluginDescription desc;
    desc.fileOrIdentifier = EL_NODE_ID_MIDI_SEQUENCER;   // TrackerNode id
    desc.pluginFormatName = EL_NODE_FORMAT_NAME;
    desc.name             = "Tracker";

    /* addPlugin(graph, desc) lands the node on the supplied graph at
     * a default position.  rescanColumns will pick it up + create the
     * matching column on the next call (from our timer / activate). */
    Node node = engine->addPlugin (graph, desc);
    if (node.isValid())
        node.setName (nextNumberedNodeName (graph, "TrkSeq"));
    rescanColumns();
}

void SessionView::moveColumn (int columnIdx, int delta)
{
    if (columnIdx < 0 || columnIdx >= columns_.size()) return;
    const int target = columnIdx + delta;
    if (target < 0 || target >= columns_.size()) return;
    if (target == columnIdx) return;

    columns_.swap (columnIdx, target);
    /* Remap clips that referenced either swapped column. */
    for (auto* c : clips_)
    {
        if      (c->columnIdx == columnIdx) c->columnIdx = target;
        else if (c->columnIdx == target)    c->columnIdx = columnIdx;
    }

    writeToSession();
    repaint();
}

void SessionView::removeColumn (int columnIdx)
{
    if (columnIdx < 0 || columnIdx >= columns_.size()) return;
    if (services_ == nullptr) return;
    auto* engine = services_->find<EngineService>();
    if (engine == nullptr) return;

    /* Resolve the host node uuid first.  removeNode by uint32 nodeId
     * is the public surface; convert via the active graph lookup. */
    const auto& col = columns_.getReference (columnIdx);
    const juce::uint32 nodeId = (col.kind == ClipKind::Midi)
        ? col.midiPlayerNodeId
        : col.trackerNodeId;
    if (nodeId == 0) return;

    /* Schedule-stop any clips on this column before tearing down the
     * host node, so the audio thread emits proper NoteOffs before the
     * node disappears.  stopColumn dispatches the per-kind FIFO stop;
     * the audio thread drains + flushes held notes on its next render. */
    stopColumn (columnIdx);

    /* Remove the graph node.  Both ArrangementView's rescanLaneTargets
     * + SessionView's rescanColumns will pick up the absence on their
     * next pass + auto-drop column / lane bindings in lockstep. */
    engine->removeNode (nodeId);

    rescanColumns();
    repaint();
}

void SessionView::addMidiColumn()
{
    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    auto graph = sess->getActiveGraph();
    if (! graph.isValid()) return;
    auto* engine = services_->find<EngineService>();
    if (engine == nullptr) return;

    juce::PluginDescription desc;
    desc.fileOrIdentifier = EL_NODE_ID_MIDI_PLAYER;
    desc.pluginFormatName = EL_NODE_FORMAT_NAME;
    desc.name             = "MIDI Player";

    Node node = engine->addPlugin (graph, desc);
    if (node.isValid())
        node.setName (nextNumberedNodeName (graph, "MidiSeq"));
    rescanColumns();
}

void SessionView::openPianoRollForClip (SessionClip& clip)
{
    /* Route MIDI clip into the bottom-attached PianoRollView dock.
     * StandardContent::showPianoRollForRegion installs the resolver
     * that walks ArrangementView; for session-view clips we want the
     * resolver to also probe SessionView::findMidiClipRegion.  The
     * dock-installed resolver is patched in StandardContent. */
    if (clip.kind != ClipKind::Midi)        return;
    if (clip.midiSourceId.isNull())         return;
    if (services_ == nullptr)               return;
    if (auto* sc = dynamic_cast<StandardContent*> (
            ViewHelpers::findContentComponent (this)))
        sc->showPianoRollForRegion (clip.midiSourceId);
}

void SessionView::deleteClip (SessionClip& clip)
{
    if (clip.kind == ClipKind::Tracker)
    {
        if (auto* trk = lookupTracker (clip.trackerNodeId))
            trk->schedulePlaying (clip.sequenceIdx, -1.0, false);
        /* Note: we do NOT call trk->removeSequence() -- the underlying
         * vht sequence may be referenced by other clips or by the
         * arrangement view.  Phase 7+ adds a "delete unreferenced
         * sequences" sweep. */
    }
    else /* Midi */
    {
        if (auto* mp = lookupMidiPlayer (clip.midiPlayerNodeId))
            mp->removeSessionClip (clip.midiSourceId);
        /* Unlike tracker sequences, MIDI clip regions are NOT shared
         * (each clip owns its region), so removeSessionClip both
         * tombstones the slot AND schedules the held-notes flush. */
    }

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
    /* Suppress undo tracking: graph-driven column reconciliation is
     * a side effect of node add / remove in the graph view, which
     * already pushes its own undo action for the underlying user
     * action.  We don't want the user's Cmd+Z to also unwind the
     * session view's column reflow. */
    juce::ScopedValueSetter<bool> suppressGuard (applyingUndoAction_, true);

    if (services_ == nullptr) return;
    auto sess = services_->context().session();
    if (sess == nullptr) return;
    auto graph = sess->getActiveGraph();
    if (! graph.isValid()) return;

    juce::Array<juce::uint32>  nodeIds;
    juce::Array<String>        names;
    juce::Array<ClipKind>      kinds;
    collectColumnSourceNodes (graph, nodeIds, names, kinds);

    /* Auto-rename: any clip-source node still wearing its default
     * factory name ("Tracker" or "MIDI Player") gets bumped to the
     * canonical "TrkSeq N" / "MidiSeq N" so the graph block + column
     * label both read consistently.  User-customised names are
     * preserved -- the rename triggers ONLY on exact default match. */
    for (int i = 0; i < nodeIds.size(); ++i)
    {
        const auto& nm = names.getReference (i);
        const bool needsRename =
            (kinds[i] == ClipKind::Tracker && nm == "Tracker")
         || (kinds[i] == ClipKind::Midi    && nm == "MIDI Player");
        if (! needsRename) continue;
        Node n = graph.getNodeById (nodeIds[i]);
        if (! n.isValid()) continue;
        const auto canonical = nextNumberedNodeName (graph,
            kinds[i] == ClipKind::Midi ? "MidiSeq" : "TrkSeq");
        n.setName (canonical);
        names.set (i, canonical);
    }

    /* Resolve a column to its identifying node id, accounting for
     * kind.  Tracker columns use trackerNodeId; Midi columns use
     * midiPlayerNodeId.  Returns 0 if the kind / id pair is invalid. */
    auto columnNodeId = [] (const SessionColumn& c) -> juce::uint32 {
        return c.kind == ClipKind::Midi ? c.midiPlayerNodeId
                                        : c.trackerNodeId;
    };

    /* Build a fresh column list keyed by (kind, nodeId).  Preserve any
     * persisted column ordering for nodes that still exist; append
     * new ones at the end. */
    juce::Array<SessionColumn> fresh;
    juce::Array<juce::uint32>  seen;

    for (const auto& existing : columns_)
    {
        const auto existingId = columnNodeId (existing);
        const int idx = nodeIds.indexOf (existingId);
        /* Only carry the column over if a node with that id exists
         * AND its kind matches what the column was persisted as. */
        if (idx >= 0 && kinds[idx] == existing.kind)
        {
            fresh.add (existing);
            seen.add (existingId);
            fresh.getReference (fresh.size() - 1).name = names[idx];
        }
    }

    for (int i = 0; i < nodeIds.size(); ++i)
    {
        if (seen.contains (nodeIds[i])) continue;
        SessionColumn col;
        col.id   = Uuid();
        col.name = names[i];
        col.kind = kinds[i];
        if (col.kind == ClipKind::Midi)
            col.midiPlayerNodeId = nodeIds[i];
        else
            col.trackerNodeId    = nodeIds[i];
        fresh.add (col);
    }

    bool changed = (fresh.size() != columns_.size());
    if (! changed)
        for (int i = 0; i < fresh.size(); ++i)
        {
            const auto& a = fresh.getReference (i);
            const auto& b = columns_.getReference (i);
            if (a.kind != b.kind
                || columnNodeId (a) != columnNodeId (b)
                || a.name != b.name)
                { changed = true; break; }
        }

    if (changed)
    {
        /* Drop clips whose column went away.  Rebind columnIdx for
         * clips whose column was reordered.  Match clips by kind +
         * kind-specific host node id. */
        for (int i = clips_.size(); --i >= 0;)
        {
            int newCol = -1;
            const auto* clip = clips_[i];
            for (int j = 0; j < fresh.size(); ++j)
            {
                const auto& fc = fresh.getReference (j);
                if (fc.kind != clip->kind) continue;
                if (clip->kind == ClipKind::Midi)
                {
                    if (fc.midiPlayerNodeId != 0
                     && fc.midiPlayerNodeId == clip->midiPlayerNodeId)
                        { newCol = j; break; }
                }
                else
                {
                    if (fc.trackerNodeId != 0
                     && fc.trackerNodeId == clip->trackerNodeId)
                        { newCol = j; break; }
                }
            }
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

    /* Toolbar prefs at the sessionView root.  C.6: use enum-max
     * sentinel so adding a sixth enum value tomorrow doesn't
     * silently truncate fresh saves through this clamp. */
    defaultLaunchQuant_ = static_cast<LaunchQuant> (
        juce::jlimit (0, (int) LaunchQuant::FourBars,
                      (int) tree.getProperty ("defaultLaunchQuant",
                                              (int) LaunchQuant::Bar)));
    refreshToolbarLabels();

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
            /* `kind` defaults to "tracker" so existing sessions
             * deserialise unchanged.  Unknown kinds fall back to
             * tracker too -- the column will detach from any node
             * on the next rescanColumns (no matching kind found). */
            const String kindStr = cn.getProperty ("kind", "tracker").toString();
            col.kind          = (kindStr == "midi") ? ClipKind::Midi : ClipKind::Tracker;
            col.trackerNodeId    = (juce::uint32) (juce::int64) cn.getProperty ("trackerNodeId", 0);
            col.midiPlayerNodeId = (juce::uint32) (juce::int64) cn.getProperty ("midiPlayerNodeId", 0);
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
            sc.id            = Uuid (sn.getProperty ("id").toString());
            sc.name          = sn.getProperty ("name", "Scene " + String (i + 1));
            sc.color         = Colour::fromString (sn.getProperty ("color", "ff303030").toString());
            sc.tempoOverride = (double) sn.getProperty ("tempoOverride", -1.0);
            sc.beatsPerBar   = (int)    sn.getProperty ("beatsPerBar", 0);
            sc.beatDivisor   = (int)    sn.getProperty ("beatDivisor", 0);
            scenes_.add (sc);
        }
    }

    /* Clips.  Absent `kind` attribute means tracker so pre-MIDI
     * sessions deserialise unchanged.  Unknown kinds are skipped --
     * forward-compat for any future fourth clip kind. */
    clips_.clearQuick (true);
    const auto clipsTree = tree.getChildWithName ("clips");
    if (clipsTree.isValid())
    {
        for (int i = 0; i < clipsTree.getNumChildren(); ++i)
        {
            const auto cn = clipsTree.getChild (i);
            const String kindStr = cn.getProperty ("kind", "tracker").toString();
            ClipKind kind;
            if      (kindStr == "tracker") kind = ClipKind::Tracker;
            else if (kindStr == "midi")    kind = ClipKind::Midi;
            else                           continue;   // unknown -- skip

            auto* clip = new SessionClip();
            clip->kind          = kind;
            clip->id            = Uuid (cn.getProperty ("id").toString());
            clip->name          = cn.getProperty ("name", String ("Clip"));
            clip->color         = Colour::fromString (cn.getProperty ("color", "ff4a7ab5").toString());
            clip->trackerNodeId    = (juce::uint32) (juce::int64) cn.getProperty ("trackerNodeId", 0);
            clip->sequenceIdx      = (int) cn.getProperty ("sequenceIdx", -1);
            clip->midiPlayerNodeId = (juce::uint32) (juce::int64) cn.getProperty ("midiPlayerNodeId", 0);
            const String midiSrc   = cn.getProperty ("midiSourceId", String()).toString();
            clip->midiSourceId     = midiSrc.isNotEmpty() ? Uuid (midiSrc)
                                                          : Uuid::null();
            clip->sceneRow      = (int) cn.getProperty ("sceneRow", 0);
            clip->columnIdx     = (int) cn.getProperty ("columnIdx", 0);
            clip->launchQuant   = static_cast<LaunchQuant> (
                juce::jlimit (0, (int) LaunchQuant::FourBars,
                              (int) cn.getProperty ("launchQuant",
                                                    (int) LaunchQuant::Bar)));
            clip->followAction  = static_cast<FollowAction> (
                juce::jlimit (0, (int) FollowAction::FirstClip,
                              (int) cn.getProperty ("followAction",
                                                    (int) FollowAction::None)));
            clips_.add (clip);
        }
    }
}

/* Undoable snapshot action for SessionView mutations.  Stored in the
 * global GuiService::UndoManager.  Holds independent copies of the
 * sessionView ValueTree on either side of one user mutation;
 * perform() / undo() swap via SessionView::applySessionSnapshot.
 * SafePointer guards the cached-view-destroyed case (session reload
 * clears the cache + undo history together, but defence in depth). */
class SessionViewSnapshotAction : public juce::UndoableAction
{
public:
    SessionViewSnapshotAction (juce::Component::SafePointer<SessionView> v,
                                juce::ValueTree before,
                                juce::ValueTree after)
        : view_ (v),
          before_ (std::move (before)),
          after_  (std::move (after))
    {}

    bool perform() override
    {
        if (auto* v = view_.getComponent())
        {
            v->applySessionSnapshot (after_);
            return true;
        }
        return false;
    }

    bool undo() override
    {
        if (auto* v = view_.getComponent())
        {
            v->applySessionSnapshot (before_);
            return true;
        }
        return false;
    }

private:
    juce::Component::SafePointer<SessionView> view_;
    juce::ValueTree before_;
    juce::ValueTree after_;
};

void SessionView::writeToSession()
{
    auto tree = getOrCreateSessionViewTree();
    if (! tree.isValid()) return;

    /* Toolbar prefs at the sessionView root. */
    tree.setProperty ("defaultLaunchQuant", (int) defaultLaunchQuant_, nullptr);

    tree.removeAllChildren (nullptr);

    /* Columns. */
    juce::ValueTree colsTree ("columns");
    for (const auto& c : columns_)
    {
        juce::ValueTree cn ("column");
        cn.setProperty ("id",            c.id.toString(),               nullptr);
        cn.setProperty ("name",          c.name,                        nullptr);
        cn.setProperty ("kind",
            c.kind == ClipKind::Midi ? String ("midi") : String ("tracker"),
            nullptr);
        /* Sparse-write the host node id: only the field relevant to
         * this column's kind survives.  Keeps the XML diff readable
         * and prevents stale id ghosts from leaking across kind
         * changes (which shouldn't happen, but defence in depth). */
        if (c.kind == ClipKind::Midi)
            cn.setProperty ("midiPlayerNodeId", (juce::int64) c.midiPlayerNodeId, nullptr);
        else
            cn.setProperty ("trackerNodeId",    (juce::int64) c.trackerNodeId,    nullptr);
        colsTree.appendChild (cn, nullptr);
    }
    tree.appendChild (colsTree, nullptr);

    /* Scenes. */
    juce::ValueTree scenesTree ("scenes");
    for (const auto& s : scenes_)
    {
        juce::ValueTree sn ("scene");
        sn.setProperty ("id",            s.id.toString(),    nullptr);
        sn.setProperty ("name",          s.name,             nullptr);
        sn.setProperty ("color",         s.color.toString(), nullptr);
        sn.setProperty ("tempoOverride", s.tempoOverride,    nullptr);
        sn.setProperty ("beatsPerBar",   s.beatsPerBar,      nullptr);
        sn.setProperty ("beatDivisor",   s.beatDivisor,      nullptr);
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
        /* Sparse-write `kind` -- omit when tracker so existing saves
         * stay byte-identical for the dominant case.  Readers default
         * to tracker on the missing attribute. */
        if (clip->kind == ClipKind::Midi)
        {
            cn.setProperty ("kind", "midi", nullptr);
            cn.setProperty ("midiPlayerNodeId",
                            (juce::int64) clip->midiPlayerNodeId, nullptr);
            cn.setProperty ("midiSourceId",
                            clip->midiSourceId.toString(), nullptr);
        }
        else
        {
            cn.setProperty ("trackerNodeId", (juce::int64) clip->trackerNodeId, nullptr);
            cn.setProperty ("sequenceIdx",   clip->sequenceIdx,                 nullptr);
        }
        cn.setProperty ("sceneRow",      clip->sceneRow,                   nullptr);
        cn.setProperty ("columnIdx",     clip->columnIdx,                  nullptr);
        cn.setProperty ("launchQuant",   (int) clip->launchQuant,          nullptr);
        cn.setProperty ("followAction",  (int) clip->followAction,         nullptr);
        clipsTree.appendChild (cn, nullptr);
    }
    tree.appendChild (clipsTree, nullptr);

    /* Push an undo step.  writeToSession is the natural transaction
     * choke point -- every clip add / remove, scene add / remove,
     * column add / remove, scene rename / colour edit, ARM / MUTE /
     * SOLO toggle, and quant setting writes through here.  Skip
     * during undo replay (applyingUndoAction_) and during the
     * initial first-write (lastCommittedSnapshot_ invalid) so the
     * baseline doesn't generate a stray action. */
    if (! applyingUndoAction_ && lastCommittedSnapshot_.isValid() && services_ != nullptr)
    {
        auto currentCopy = tree.createCopy();
        if (auto* gui = services_->find<GuiService>())
        {
            auto& undo = gui->getUndoManager();
            undo.beginNewTransaction();
            undo.perform (new SessionViewSnapshotAction (this,
                                                          lastCommittedSnapshot_,
                                                          currentCopy));
        }
        lastCommittedSnapshot_ = std::move (currentCopy);
    }
    else
    {
        lastCommittedSnapshot_ = tree.createCopy();
    }
}

void SessionView::applySessionSnapshot (const juce::ValueTree& snap)
{
    /* Re-entrancy guard mirrors ArrangementView::applyLaneSnapshot --
     * writeToSession runs inside this method (for persistence via
     * subsequent mutation paths) and must skip the action push. */
    juce::ScopedValueSetter<bool> guard (applyingUndoAction_, true);

    auto tree = getOrCreateSessionViewTree();
    if (! tree.isValid()) return;

    /* Replace tree contents with the snapshot: copy props + children
     * in-place so listeners remain bound to the same VT identity.
     * Removing-and-re-adding the entire sessionView child would
     * invalidate any external listeners. */
    tree.removeAllProperties (nullptr);
    tree.removeAllChildren (nullptr);
    for (int i = 0; i < snap.getNumProperties(); ++i)
    {
        const auto& propName = snap.getPropertyName (i);
        tree.setProperty (propName, snap.getProperty (propName), nullptr);
    }
    for (int i = 0; i < snap.getNumChildren(); ++i)
        tree.appendChild (snap.getChild (i).createCopy(), nullptr);

    readFromSession();
    lastCommittedSnapshot_ = tree.createCopy();
    repaint();
}

/* === 30 Hz state poll ================================================== */

void SessionView::timerCallback()
{
    if (! isShowing()) return;   // gated per feedback_gui_must_stay_fast

    ++pulsePhase_;

    /* Reconcile effective Processor::setMuted from each tracker's
     * user-mute + solo flags.  Cheap (O(columns) bool comparisons +
     * a setMuted call only when the effective state changed).  Runs
     * every tick so changes from the tracker editor popup (separate
     * window) propagate back into the session view. */
    applyMuteAndSoloState();

    /* Diff-gated repaint of the master column when the session
     * tempo / signature changes outside this view (e.g. user
     * tweaks the main toolbar BPM).  Scenes without explicit
     * overrides display the current value dimmed, so they need
     * to refresh on transport changes. */
    if (services_ != nullptr)
    {
        if (auto sess = services_->context().session())
        {
            const double t   = (double) sess->getProperty (tags::tempo,       120.0);
            const int    bpb = (int)    sess->getProperty (tags::beatsPerBar, 4);
            const int    bd  = (int)    sess->getProperty (tags::beatDivisor, 4);
            if (std::abs (t - lastSessionTempo_) > 0.01
                || bpb != lastSessionBpb_
                || bd  != lastSessionBd_)
            {
                lastSessionTempo_ = t;
                lastSessionBpb_   = bpb;
                lastSessionBd_    = bd;
                repaint (masterColumnBounds());
            }
        }
    }

    for (auto* clip : clips_)
    {
        /* Per-kind binding validity + per-kind engine-playing probe.
         * Both tracker and midi clips publish "is the audio thread
         * currently emitting this clip" through the same boolean
         * shape; downstream reconciliation logic is kind-agnostic. */
        bool enginePlaying = false;
        if (clip->kind == ClipKind::Tracker)
        {
            if (clip->sequenceIdx < 0) continue;
            auto* trk = lookupTracker (clip->trackerNodeId);
            if (trk == nullptr) continue;
            enginePlaying = trk->isSequencePlaying (clip->sequenceIdx);
        }
        else
        {
            if (clip->midiSourceId.isNull()) continue;
            auto* mp = lookupMidiPlayer (clip->midiPlayerNodeId);
            if (mp == nullptr) continue;
            enginePlaying = mp->isSessionClipPlaying (clip->midiSourceId);
        }
        const LiveState cur = clip->state.load (std::memory_order_relaxed);

        /* State reconciliation -- SessionClip.state is authoritative
         * for "did the user launch this from session view".  We do NOT
         * auto-flip Stopped -> Playing from engine state: vht keeps
         * each tracker's curr_seq->playing = 1 by default so it can
         * emit MIDI at all (installDefaultPattern / setState), and a
         * naive engine-mirror would light up every first-clip as
         * "active" on session open without any user input.
         *
         * Allowed transitions:
         *   WaitingToStart -> Playing  when audio thread fires the launch
         *   WaitingToStop  -> Stopped  when audio thread fires the stop
         *   Playing        -> Stopped  ONLY after we have observed engine
         *                              playing at least once for this
         *                              clip (lastDrawnPlaying was true).
         *                              This lets us catch engine-side
         *                              kills (one-shot patterns, kill
         *                              triggers) while protecting against
         *                              the brief race where we set state
         *                              to Playing immediately on bang
         *                              but the audio thread hasn't fired
         *                              yet (isSequencePlaying false). */
        LiveState next = cur;
        switch (cur)
        {
            case LiveState::Stopped:
                break;
            case LiveState::WaitingToStart:
                if (enginePlaying) next = LiveState::Playing;
                break;
            case LiveState::Playing:
                if (! enginePlaying && clip->lastDrawnPlaying)
                    next = LiveState::Stopped;
                break;
            case LiveState::WaitingToStop:
                if (! enginePlaying) next = LiveState::Stopped;
                break;
        }

        int posRow = -1;
        if (enginePlaying)
        {
            if (clip->kind == ClipKind::Tracker)
            {
                if (auto* trk = lookupTracker (clip->trackerNodeId))
                    posRow = (int) trk->getSequencePositionRows (clip->sequenceIdx);
            }
            else
            {
                /* For MIDI clips we sentinel-translate localBeatPos
                 * to a row-ish int (beats * 100, truncated).  The
                 * diff-gate paint logic only cares about "did the
                 * value change", not its absolute scale. */
                if (auto* mp = lookupMidiPlayer (clip->midiPlayerNodeId))
                    posRow = (int) (mp->getSessionClipPositionBeats (clip->midiSourceId)
                                    * 100.0);
            }
        }

        /* Repaint conditions: state change, position bar moved while
         * playing, OR clip is in a WaitingTo* state (so the pulsing
         * border animates each tick). */
        const bool isWaiting = (cur == LiveState::WaitingToStart
                             || cur == LiveState::WaitingToStop);
        const bool stateChanged = (next != cur);
        const bool needRepaint = stateChanged
                              || (posRow != clip->lastDrawnPosRow)
                              || isWaiting;

        if (stateChanged)
            clip->state.store (next, std::memory_order_relaxed);
        clip->lastDrawnPlaying = enginePlaying;
        clip->lastDrawnPosRow  = posRow;

        if (needRepaint)
            repaint (cellBounds (clip->sceneRow, clip->columnIdx));

        /* Master launch button + scene row amber both derive from
         * whether the scene has any Playing or WaitingToStart clip.
         * Repaint conditions:
         *   - clip transitioned into / out of Playing : reflect the
         *     solid-amber boundary on the master cell + row band.
         *   - clip transitioned into / out of WaitingToStart : same
         *     reason, on the pulsed boundary.
         *   - clip is currently WaitingToStart : drive the pulse
         *     animation each tick. */
        const bool wasWaitingStart  = (cur  == LiveState::WaitingToStart);
        const bool willWaitingStart = (next == LiveState::WaitingToStart);
        const bool needMasterPaint =
            (stateChanged
             && (cur  == LiveState::Playing
              || next == LiveState::Playing
              || wasWaitingStart
              || willWaitingStart))
            || wasWaitingStart;

        if (needMasterPaint)
        {
            repaint (masterCellBounds (clip->sceneRow));
            if (clip->sceneRow == currentSceneRow_)
            {
                const auto sl = sceneLabelBounds (clip->sceneRow);
                repaint (Rectangle<int> (0, sl.getY(), getWidth(), kRowH));
            }
        }

        /* Follow-action edge: only check after state reconciliation
         * so a freshly-launched clip doesn't immediately fire its
         * follow action on the first poll.  ...WrappedSinceLastQuery
         * consumes the wrap edge -- repeated calls in the same wrap
         * window return false. */
        if (next == LiveState::Playing && clip->followAction != FollowAction::None)
        {
            bool wrapped = false;
            if (clip->kind == ClipKind::Tracker)
            {
                if (auto* trk = lookupTracker (clip->trackerNodeId))
                    wrapped = trk->sequenceWrappedSinceLastQuery (clip->sequenceIdx);
            }
            else
            {
                if (auto* mp = lookupMidiPlayer (clip->midiPlayerNodeId))
                    wrapped = mp->sessionClipWrappedSinceLastQuery (clip->midiSourceId);
            }
            if (wrapped) applyFollowAction (*clip);
        }
    }
}

} // namespace element
