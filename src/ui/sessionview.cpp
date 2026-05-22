// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/sessionview.hpp"

#include <limits>

#include <element/audioengine.hpp>
#include <element/context.hpp>
#include <element/node.hpp>
#include <element/session.hpp>
#include <element/tags.hpp>
#include <element/ui/style.hpp>

#include "nodes/tracker.hpp"
#include "nodes/trackereditor.hpp"
#include "tempo.hpp"   // BeatType::fromDivisor for scene sig overrides

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
    inlineEditor_.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
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
    l.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
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

    place (rescanBtn_, 64);
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

/* Column header bottom-half layout: three equal-width buttons with
 * 2 px gaps.  STOP | MUTE | SOLO. */
static int columnFooterButtonWidth (int innerW) noexcept
{
    return (innerW - 4) / 3;   // 2 gaps of 2 px each
}

Rectangle<int> SessionView::columnStopButtonBounds (int columnIdx) const noexcept
{
    const auto h = columnHeaderBounds (columnIdx);
    const int topH = h.getHeight() / 2;
    const int btnAreaH = h.getHeight() - topH - 2;
    const int btnAreaY = h.getY() + topH;
    const int innerW = h.getWidth() - 4;
    const int w = columnFooterButtonWidth (innerW);
    return Rectangle<int> (h.getX() + 2, btnAreaY, w, btnAreaH);
}

Rectangle<int> SessionView::columnMuteButtonBounds (int columnIdx) const noexcept
{
    const auto h = columnHeaderBounds (columnIdx);
    const int topH = h.getHeight() / 2;
    const int btnAreaH = h.getHeight() - topH - 2;
    const int btnAreaY = h.getY() + topH;
    const int innerW = h.getWidth() - 4;
    const int w = columnFooterButtonWidth (innerW);
    return Rectangle<int> (h.getX() + 2 + w + 2, btnAreaY, w, btnAreaH);
}

Rectangle<int> SessionView::columnSoloButtonBounds (int columnIdx) const noexcept
{
    const auto h = columnHeaderBounds (columnIdx);
    const int topH = h.getHeight() / 2;
    const int btnAreaH = h.getHeight() - topH - 2;
    const int btnAreaY = h.getY() + topH;
    const int innerW = h.getWidth() - 4;
    const int w = columnFooterButtonWidth (innerW);
    return Rectangle<int> (h.getX() + 2 + (w + 2) * 2, btnAreaY, w, btnAreaH);
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
    /* Master cell layout: [launch button] [tempo field] [sig field]
     * Tempo + sig get a balanced split of the remaining width with a
     * 4 px gap; tempo gets slightly more because BPM strings ("120.00")
     * are typically wider than sig strings ("4/4"). */
    auto inner = masterCellBounds (sceneRow).reduced (3, 3);
    inner.removeFromLeft (22 + 4);            // launch button + spacing
    const int remaining = inner.getWidth();
    const int tempoW = (remaining - 4) * 11 / 20;  // ~55%
    return inner.removeFromLeft (tempoW);
}

Rectangle<int> SessionView::masterSigFieldBounds (int sceneRow) const noexcept
{
    auto inner = masterCellBounds (sceneRow).reduced (3, 3);
    inner.removeFromLeft (22 + 4);
    const int remaining = inner.getWidth();
    const int tempoW = (remaining - 4) * 11 / 20;
    inner.removeFromLeft (tempoW + 4);        // skip tempo + gap
    return inner;
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

    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                  kHeaderFontSize, juce::Font::bold));

    for (int c = 0; c < columns_.size(); ++c)
    {
        const auto h     = columnHeaderBounds (c);
        const auto tint  = columnTint (c);
        const auto nameR = columnNameLabelBounds (c);
        const auto stopR = columnStopButtonBounds (c);
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
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      kHeaderFontSize, juce::Font::bold));
        g.setColour (tint);
        g.drawText (columns_.getReference (c).name,
                    nameR.reduced (8, 0),
                    juce::Justification::centredLeft, true);

        bool active = false;
        for (auto* clip : clips_)
            if (clip->columnIdx == c)
            {
                const LiveState s = clip->state.load (std::memory_order_relaxed);
                if (s != LiveState::Stopped) { active = true; break; }
            }
        const bool muted  = isColumnMuted  (c);
        const bool soloed = isColumnSoloed (c);
        const Colour btnTint = tint.withMultipliedSaturation (0.6f)
                                   .withMultipliedBrightness (0.55f);

        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      9.0f, juce::Font::bold));

        /* STOP -- lights up brighter when the column has an active
         * clip; the click stops every active clip on this column. */
        g.setColour (active ? btnTint.withMultipliedBrightness (1.6f) : btnTint);
        g.fillRect (stopR);
        g.setColour (kCellOutlineColour);
        g.drawRect (stopR, 1);
        g.setColour (active ? juce::Colours::white
                            : juce::Colours::white.withAlpha (0.70f));
        g.drawText ("STOP", stopR, juce::Justification::centred);

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

    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
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
                if (auto* trk = lookupTracker (clip->trackerNodeId))
                {
                    const int   total = juce::jmax (1, trk->getSequenceLengthRows (clip->sequenceIdx));
                    const double pos  = trk->getSequencePositionRows (clip->sequenceIdx);
                    const float frac  = (float) juce::jlimit (0.0, 1.0, pos / (double) total);
                    const int barW    = juce::jmax (1, (int) (frac * cb.getWidth()));
                    /* Translucent track underneath so the playhead reads
                     * as a moving fill on a dim runway -- easier to spot
                     * at a glance than a bare 3 px line over the cell
                     * colour. */
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
            const bool sameTracker = (dragHoverCol_ < columns_.size()
                && columns_.getReference (dragHoverCol_).trackerNodeId
                   == dragSource_->trackerNodeId);
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

        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
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

            /* Launch button -- styled to match clip play buttons so
             * the user reads it as the same gesture.  Lights up
             * amber when any clip in the scene is currently active. */
            const bool sceneActive = sceneHasActiveClip (r);
            g.setColour (sceneActive
                            ? kPlayheadAccent.withAlpha (0.30f)
                            : juce::Colours::black.withAlpha (0.20f));
            g.fillRect (launchR);
            g.setColour (sceneActive ? kPlayheadAccent
                                     : Colour { 0xff'd0'd0'd0 });
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
                g.setColour (overrideSet ? Colour { 0xff'd4'd4'd4 }
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

            /* Row divider. */
            g.setColour (kCellOutlineColour);
            g.drawHorizontalLine (cell.getBottom() - 1,
                                  (float) cell.getX(),
                                  (float) cell.getRight());
        }
    }

    /* Active-scene row indicator -- translucent amber overlay across
     * the entire row (scene label + clip cells + master cell) so the
     * user can see the whole "current scene" in one visual sweep.
     * Drawn AFTER all per-cell paint so it tints them uniformly.
     * Top/bottom edges accented at higher alpha for definition. */
    if (currentSceneRow_ >= 0 && currentSceneRow_ < scenes_.size())
    {
        const auto sl = sceneLabelBounds (currentSceneRow_);
        const Rectangle<int> rowR (0, sl.getY(), getWidth(), kRowH);
        g.setColour (kPlayheadAccent.withAlpha (0.10f));
        g.fillRect (rowR);
        g.setColour (kPlayheadAccent.withAlpha (0.80f));
        g.drawHorizontalLine (rowR.getY(),         0.0f, (float) getWidth());
        g.drawHorizontalLine (rowR.getBottom() - 1, 0.0f, (float) getWidth());
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
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
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
                juce::PopupMenu quant;
                const auto cq = clip->launchQuant;
                quant.addItem (10, "Off",     true, cq == LaunchQuant::Off);
                quant.addItem (11, "1 Beat",  true, cq == LaunchQuant::Beat);
                quant.addItem (12, "1 Bar",   true, cq == LaunchQuant::Bar);
                quant.addItem (13, "2 Bars",  true, cq == LaunchQuant::TwoBars);
                quant.addItem (14, "4 Bars",  true, cq == LaunchQuant::FourBars);

                juce::PopupMenu follow;
                const auto cf = clip->followAction;
                follow.addItem (20, "None (loop)",      true, cf == FollowAction::None);
                follow.addItem (21, "Stop",             true, cf == FollowAction::Stop);
                follow.addItem (22, "Restart",          true, cf == FollowAction::RestartClip);
                follow.addItem (23, "Next clip",        true, cf == FollowAction::NextClip);
                follow.addItem (24, "First clip",       true, cf == FollowAction::FirstClip);

                juce::PopupMenu m;
                m.addItem (2, "Rename...");
                m.addItem (3, "Cycle colour");
                m.addItem (4, "Colour...");
                m.addSeparator();
                m.addSubMenu ("Launch quantisation", quant);
                m.addSubMenu ("Follow action",       follow);
                m.addSeparator();
                m.addItem (1, "Delete clip");

                const int r = m.showAt (Rectangle<int> (e.getScreenX(), e.getScreenY(), 1, 1));
                switch (r)
                {
                    case 1: deleteClip (*clip); break;
                    case 2: renameClip (*clip); break;
                    case 3: cycleClipColor (*clip); break;
                    case 4: pickClipColour (*clip); break;
                    case 10: clip->launchQuant = LaunchQuant::Off;       writeToSession(); break;
                    case 11: clip->launchQuant = LaunchQuant::Beat;      writeToSession(); break;
                    case 12: clip->launchQuant = LaunchQuant::Bar;       writeToSession(); break;
                    case 13: clip->launchQuant = LaunchQuant::TwoBars;   writeToSession(); break;
                    case 14: clip->launchQuant = LaunchQuant::FourBars;  writeToSession(); break;
                    case 20: clip->followAction = FollowAction::None;        writeToSession(); break;
                    case 21: clip->followAction = FollowAction::Stop;        writeToSession(); break;
                    case 22: clip->followAction = FollowAction::RestartClip; writeToSession(); break;
                    case 23: clip->followAction = FollowAction::NextClip;    writeToSession(); break;
                    case 24: clip->followAction = FollowAction::FirstClip;   writeToSession(); break;
                    default: break;
                }
            }
            else if (col < columns_.size())
            {
                auto* trk = lookupTracker (columns_.getReference (col).trackerNodeId);
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

    /* Per-column footer -- STOP / MUTE.  Tested before cell hits
     * so a click here never stages a drag. */
    if (hitTestColumnStop (e.getPosition(), col))
    {
        stopColumn (col);
        return;
    }
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

    /* Master column launch button -> bang scene.
     * Master column tempo/sig fields -> click-to-edit prompts. */
    if (hitTestMasterLaunch (e.getPosition(), row))
    {
        bangScene (row);
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

    /* Double-click on the middle of a clip cell renames it.  The play
     * and edit buttons swallow their own clicks via mouseDown, so the
     * double event only reaches the cell body. */
    if (hitTestCell (e.getPosition(), row, col))
    {
        if (auto* clip = findClip (row, col))
        {
            if (! playButtonBounds (row, col).contains (e.getPosition())
             && ! editButtonBounds (row, col).contains (e.getPosition()))
            {
                renameClip (*clip);
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

void SessionView::transitionClip (SessionClip& clip, double targetBeat)
{
    auto* trk = lookupTracker (clip.trackerNodeId);
    if (trk == nullptr || clip.sequenceIdx < 0) return;

    const LiveState cur = clip.state.load (std::memory_order_relaxed);

    /* === Cancel rules: re-banging a queued clip un-queues it. */
    if (cur == LiveState::WaitingToStart)
    {
        trk->schedulePlaying (clip.sequenceIdx, -1.0, false);
        clip.state.store (LiveState::Stopped, std::memory_order_relaxed);
        repaint (cellBounds (clip.sceneRow, clip.columnIdx));
        return;
    }
    if (cur == LiveState::WaitingToStop)
    {
        trk->schedulePlaying (clip.sequenceIdx, -1.0, true);
        clip.state.store (LiveState::Playing, std::memory_order_relaxed);
        repaint (cellBounds (clip.sceneRow, clip.columnIdx));
        return;
    }

    const bool wantPlaying = (cur != LiveState::Playing);

    /* Same-column mutual exclusion at the shared targetBeat -- A->stop
     * and B->start hit the audio thread in the same render block, so
     * the flip is atomic.  Skip self.  Also skip clips that point at
     * the SAME underlying sequence as us: two clips sharing a
     * sequence are functionally one engine-level voice, so banging
     * one should not stop the other (would cause "stuck WaitingTo*"
     * states from contradictory FIFO requests). */
    if (wantPlaying)
    {
        for (auto* other : clips_)
        {
            if (other == &clip) continue;
            if (other->columnIdx     != clip.columnIdx)     continue;
            if (other->trackerNodeId != clip.trackerNodeId) continue;
            if (other->sequenceIdx   == clip.sequenceIdx)   continue;
            const LiveState os = other->state.load (std::memory_order_relaxed);
            if (os != LiveState::Playing && os != LiveState::WaitingToStart) continue;

            if (auto* otrk = lookupTracker (other->trackerNodeId))
                otrk->schedulePlaying (other->sequenceIdx, targetBeat, false);

            other->state.store (targetBeat < 0.0 ? LiveState::Stopped
                                                 : LiveState::WaitingToStop,
                                std::memory_order_relaxed);
            repaint (cellBounds (other->sceneRow, other->columnIdx));
        }
    }

    trk->schedulePlaying (clip.sequenceIdx, targetBeat, wantPlaying);

    const LiveState next = (targetBeat < 0.0)
        ? (wantPlaying ? LiveState::Playing       : LiveState::Stopped)
        : (wantPlaying ? LiveState::WaitingToStart : LiveState::WaitingToStop);
    clip.state.store (next, std::memory_order_relaxed);
    repaint (cellBounds (clip.sceneRow, clip.columnIdx));
}

void SessionView::bangClip (SessionClip& clip)
{
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
    auto* trk = lookupTracker (clip.trackerNodeId);
    if (trk == nullptr) return;

    switch (clip.followAction)
    {
        case FollowAction::None:
            break;

        case FollowAction::Stop:
            trk->schedulePlaying (clip.sequenceIdx, -1.0, false);
            clip.state.store (LiveState::Stopped, std::memory_order_relaxed);
            repaint (cellBounds (clip.sceneRow, clip.columnIdx));
            break;

        case FollowAction::RestartClip:
            /* schedulePlaying(_, true) rewinds pos to 0 even when the
             * sequence is already playing -- see applyPendingForBlock
             * in tracker.cpp.  Effectively a re-trigger on wrap. */
            trk->schedulePlaying (clip.sequenceIdx, -1.0, true);
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
            {
                /* No further clip -- fall through to Stop. */
                trk->schedulePlaying (clip.sequenceIdx, -1.0, false);
                clip.state.store (LiveState::Stopped, std::memory_order_relaxed);
                repaint (cellBounds (clip.sceneRow, clip.columnIdx));
            }
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
     * snap to the same target beat -- Bitwig convention.  If clips
     * use heterogeneous quants the per-clip values are ignored for
     * the duration of this bang; subsequent solo bangs use the
     * clip's own quant again. */
    LaunchQuant slowest = LaunchQuant::Off;
    bool any = false;
    for (auto* c : clips_)
    {
        if (c->sceneRow != sceneRow) continue;
        any = true;
        if ((int) c->launchQuant > (int) slowest)
            slowest = c->launchQuant;
    }
    if (! any) return;

    const bool transportPlaying = (monitor_ != nullptr && monitor_->playing.get());
    const double targetBeat = transportPlaying
        ? computeTargetBeat (currentTransportBeat(), slowest)
        : -1.0;

    /* Exclusive scene launch -- stop every clip that's currently
     * active on a row OTHER than the one we're banging.  Without
     * this, scene 2's clips keep playing when the user goes back
     * to scene 1 (since same-column mutual exclusion only catches
     * the clips that scene 1 ALSO has a slot for).  All stops use
     * the same targetBeat so the switch is atomic in one audio
     * block. */
    for (auto* c : clips_)
    {
        if (c->sceneRow == sceneRow) continue;
        const LiveState s = c->state.load (std::memory_order_relaxed);
        if (s != LiveState::Playing && s != LiveState::WaitingToStart) continue;
        if (auto* trk = lookupTracker (c->trackerNodeId))
            trk->schedulePlaying (c->sequenceIdx, targetBeat, false);
        c->state.store (targetBeat < 0.0 ? LiveState::Stopped
                                         : LiveState::WaitingToStop,
                        std::memory_order_relaxed);
        repaint (cellBounds (c->sceneRow, c->columnIdx));
    }

    for (auto* c : clips_)
        if (c->sceneRow == sceneRow)
            transitionClip (*c, targetBeat);

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

void SessionView::editSceneTempo     (int sceneRow) { openSceneView (sceneRow); }
void SessionView::editSceneSignature (int sceneRow) { openSceneView (sceneRow); }

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

bool SessionView::sceneHasActiveClip (int sceneRow) const noexcept
{
    for (auto* c : clips_)
    {
        if (c->sceneRow != sceneRow) continue;
        const auto s = c->state.load (std::memory_order_relaxed);
        if (s != LiveState::Stopped)
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
     * straight away; UI tick will confirm. */
    for (auto* c : clips_)
    {
        if (auto* trk = lookupTracker (c->trackerNodeId))
            trk->schedulePlaying (c->sequenceIdx, -1.0, false);
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
        if (auto* trk = lookupTracker (c->trackerNodeId))
            trk->schedulePlaying (c->sequenceIdx, -1.0, false);
        c->state.store (LiveState::Stopped, std::memory_order_relaxed);
    }
    repaint();
}

bool SessionView::isColumnMuted (int columnIdx) const noexcept
{
    /* Returns the USER-asserted mute (the explicit press), not the
     * effective engine state.  Visual buttons should reflect user
     * intent so toggling is predictable. */
    if (columnIdx < 0 || columnIdx >= columnUserMuted_.size()) return false;
    return columnUserMuted_.getUnchecked (columnIdx);
}

bool SessionView::isColumnSoloed (int columnIdx) const noexcept
{
    if (columnIdx < 0 || columnIdx >= columnSoloed_.size()) return false;
    return columnSoloed_.getUnchecked (columnIdx);
}

void SessionView::applyMuteAndSoloState()
{
    /* Lazy-grow the parallel state vectors to match columns_ size. */
    while (columnUserMuted_.size() < columns_.size()) columnUserMuted_.add (false);
    while (columnSoloed_.size()    < columns_.size()) columnSoloed_   .add (false);

    bool anySolo = false;
    for (int i = 0; i < columns_.size(); ++i)
        if (columnSoloed_[i]) { anySolo = true; break; }

    for (int c = 0; c < columns_.size(); ++c)
    {
        if (auto* trk = lookupTracker (columns_.getReference (c).trackerNodeId))
        {
            /* When any solo is active, non-soloed columns are
             * effectively muted regardless of user intent. */
            const bool effectiveMute = anySolo ? ! columnSoloed_[c]
                                               : columnUserMuted_[c];
            if (trk->isMuted() != effectiveMute)
                trk->setMuted (effectiveMute);
        }
    }
}

void SessionView::toggleColumnMute (int columnIdx)
{
    if (columnIdx < 0 || columnIdx >= columns_.size()) return;
    while (columnUserMuted_.size() <= columnIdx) columnUserMuted_.add (false);
    columnUserMuted_.set (columnIdx, ! columnUserMuted_[columnIdx]);
    applyMuteAndSoloState();
    repaint (headerRowBounds());
}

void SessionView::toggleColumnSolo (int columnIdx)
{
    if (columnIdx < 0 || columnIdx >= columns_.size()) return;
    while (columnSoloed_.size() <= columnIdx) columnSoloed_.add (false);
    columnSoloed_.set (columnIdx, ! columnSoloed_[columnIdx]);
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

    const auto targetTrackerId = columns_.getReference (newColumnIdx).trackerNodeId;

    /* Cross-tracker migration would require re-parenting a vht
     * `sequence` (changing its `clt` + back-pointer to a different
     * `module`); deferred until a dedicated adoptSequence API lands.
     * v1: silently reject cross-tracker drops.  In practice columns
     * are 1:1 with TrackerNodes today, so cross-column ==> cross-
     * tracker; same-column drag inside a single column is the only
     * case that actually fires. */
    if (targetTrackerId != clip.trackerNodeId) return;

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
    if (columns_.getReference (newColumnIdx).trackerNodeId != src.trackerNodeId)
        return;   // cross-tracker copy needs deferred adoptSequence API

    auto* trk = lookupTracker (src.trackerNodeId);
    if (trk == nullptr) return;

    const int newIdx = trk->cloneSequence (src.sequenceIdx);
    if (newIdx < 0) return;

    auto* clip = new SessionClip();
    clip->id            = juce::Uuid();
    clip->name          = src.name;
    clip->color         = src.color;
    clip->trackerNodeId = src.trackerNodeId;
    clip->sequenceIdx   = newIdx;
    clip->sceneRow      = newSceneRow;
    clip->columnIdx     = newColumnIdx;
    clip->launchQuant   = src.launchQuant;
    clip->followAction  = src.followAction;
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
    clip->launchQuant   = defaultLaunchQuant_;
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
                          juce::Component* content,
                          const juce::String& title)
        : juce::DocumentWindow (title,
                                juce::Colour { 0xff'18'18'18 },
                                juce::DocumentWindow::allButtons),
          owner_ (view)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (content, true);
        setResizable (true, false);
        centreWithSize (820, 540);
        setWantsKeyboardFocus (true);
        setVisible (true);
    }
    void closeButtonPressed() override { delete this; }

    /* Spacebar in the tracker popup must toggle the global transport
     * play/pause, same as Element's main spacebar binding -- otherwise
     * the user has to dismiss the popup before starting playback. */
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::spaceKey)
        {
            if (auto* v = owner_.getComponent())
            {
                v->transportTogglePlay();
                return true;
            }
        }
        return juce::DocumentWindow::keyPressed (key);
    }

private:
    juce::Component::SafePointer<SessionView> owner_;
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
        l.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      pt, style));
        l.setColour (juce::Label::textColourId, juce::Colour { 0xff'c0'c0'c0 });
        addAndMakeVisible (l);
    }

    void configureEditor (juce::TextEditor& e)
    {
        e.setMultiLine (false);
        e.setReturnKeyStartsNewLine (false);
        e.setSelectAllWhenFocused (true);
        e.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
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

/* openSceneView is defined here so SceneViewContent / SceneViewWindow
 * are complete types at the point of `new`.  All other SessionView
 * method definitions live above. */
void SessionView::openSceneView (int sceneRow)
{
    if (sceneRow < 0 || sceneRow >= scenes_.size()) return;
    auto* content = new SceneViewContent (this, sceneRow);
    auto* win = new SceneViewWindow (content,
                                     "Scene " + juce::String (sceneRow + 1));
    juce::ignoreUnused (win);   // self-deletes on close
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
    auto* win = new TrackerPatternWindow (this, editor, "Tracker - " + clip.name);
    juce::ignoreUnused (win);   // self-deletes on close
}

void SessionView::deleteClip (SessionClip& clip)
{
    if (auto* trk = lookupTracker (clip.trackerNodeId))
        trk->schedulePlaying (clip.sequenceIdx, -1.0, false);

    /* Note: we do NOT call trk->removeSequence() -- the underlying vht
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

    /* Toolbar prefs at the sessionView root. */
    defaultLaunchQuant_ = static_cast<LaunchQuant> (
        juce::jlimit (0, 4, (int) tree.getProperty ("defaultLaunchQuant",
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
            sc.id            = Uuid (sn.getProperty ("id").toString());
            sc.name          = sn.getProperty ("name", "Scene " + String (i + 1));
            sc.color         = Colour::fromString (sn.getProperty ("color", "ff303030").toString());
            sc.tempoOverride = (double) sn.getProperty ("tempoOverride", -1.0);
            sc.beatsPerBar   = (int)    sn.getProperty ("beatsPerBar", 0);
            sc.beatDivisor   = (int)    sn.getProperty ("beatDivisor", 0);
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
            clip->launchQuant   = static_cast<LaunchQuant> (
                juce::jlimit (0, 4, (int) cn.getProperty ("launchQuant",
                                                          (int) LaunchQuant::Bar)));
            clip->followAction  = static_cast<FollowAction> (
                juce::jlimit (0, 4, (int) cn.getProperty ("followAction",
                                                          (int) FollowAction::None)));
            clips_.add (clip);
        }
    }
}

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
        cn.setProperty ("trackerNodeId", (juce::int64) c.trackerNodeId, nullptr);
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
        cn.setProperty ("trackerNodeId", (juce::int64) clip->trackerNodeId, nullptr);
        cn.setProperty ("sequenceIdx",   clip->sequenceIdx,                nullptr);
        cn.setProperty ("sceneRow",      clip->sceneRow,                   nullptr);
        cn.setProperty ("columnIdx",     clip->columnIdx,                  nullptr);
        cn.setProperty ("launchQuant",   (int) clip->launchQuant,          nullptr);
        cn.setProperty ("followAction",  (int) clip->followAction,         nullptr);
        clipsTree.appendChild (cn, nullptr);
    }
    tree.appendChild (clipsTree, nullptr);
}

/* === 30 Hz state poll ================================================== */

void SessionView::timerCallback()
{
    if (! isShowing()) return;   // gated per feedback_gui_must_stay_fast

    ++pulsePhase_;

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
        if (clip->sequenceIdx < 0) continue;
        auto* trk = lookupTracker (clip->trackerNodeId);
        if (trk == nullptr) continue;

        const bool enginePlaying = trk->isSequencePlaying (clip->sequenceIdx);
        const LiveState cur = clip->state.load (std::memory_order_relaxed);

        /* State reconciliation: the audio thread flips seq->playing
         * at the scheduled boundary; the UI tick observes the result
         * and transitions WaitingTo* -> final state.  Crucially, we
         * do NOT clobber WaitingTo* with an engine snapshot that
         * disagrees -- those states represent "we're waiting for the
         * boundary," which means engine has NOT changed yet. */
        LiveState next = cur;
        switch (cur)
        {
            case LiveState::Stopped:
                if (enginePlaying) next = LiveState::Playing;
                break;
            case LiveState::WaitingToStart:
                if (enginePlaying) next = LiveState::Playing;
                break;
            case LiveState::Playing:
                if (! enginePlaying) next = LiveState::Stopped;
                break;
            case LiveState::WaitingToStop:
                if (! enginePlaying) next = LiveState::Stopped;
                break;
        }

        const int posRow = enginePlaying
            ? (int) trk->getSequencePositionRows (clip->sequenceIdx)
            : -1;

        /* Repaint conditions: state change, position bar moved while
         * playing, OR clip is in a WaitingTo* state (so the pulsing
         * border animates each tick). */
        const bool isWaiting = (cur == LiveState::WaitingToStart
                             || cur == LiveState::WaitingToStop);
        const bool needRepaint = (next != cur)
                              || (posRow != clip->lastDrawnPosRow)
                              || (enginePlaying != clip->lastDrawnPlaying)
                              || isWaiting;

        if (next != cur)
            clip->state.store (next, std::memory_order_relaxed);
        clip->lastDrawnPlaying = enginePlaying;
        clip->lastDrawnPosRow  = posRow;

        if (needRepaint)
            repaint (cellBounds (clip->sceneRow, clip->columnIdx));

        /* Follow-action edge: only check after state reconciliation
         * so a freshly-launched clip doesn't immediately fire its
         * follow action on the first poll.  sequenceWrappedSinceLastQuery
         * consumes the wrap edge -- repeated calls in the same wrap
         * window return false. */
        if (next == LiveState::Playing
            && clip->followAction != FollowAction::None
            && trk->sequenceWrappedSinceLastQuery (clip->sequenceIdx))
        {
            applyFollowAction (*clip);
        }
    }
}

} // namespace element
