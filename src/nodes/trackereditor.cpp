// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/trackereditor.hpp"
#include "nodes/tracker.hpp"

namespace element {

namespace {

/* Layout constants — Renoise-ish. Tunable later. */
constexpr int kRowHeight        = 18;
constexpr int kRowGutterWidth   = 40;
constexpr int kTrackWidth       = 168;
constexpr int kTrackHeaderH     = 30;
constexpr int kColumnSubWidth   = 32;
constexpr float kCellFontSize   = 13.0f;
constexpr float kHeaderFontSize = 12.0f;

/* Sub-column X positions (relative to the track's left edge) and widths.
 * Each FX column takes 36 px (12 per char × 3 chars). */
constexpr int kNoteX      = 4;
constexpr int kNoteW      = 36;     // "C-5", incl trailing pad
constexpr int kVelX       = 44;     // velocity column
constexpr int kVelHalfW   = 12;     // each nybble character
constexpr int kVelW       = 24;     // hi+lo
constexpr int kFx1X       = 76;     // FX1 column start
constexpr int kFxCharW    = 12;     // letter / hi / lo each 12 px wide
constexpr int kFxColW     = 36;     // FX column total (letter + hi + lo)
constexpr int kFx2X       = 120;    // FX2 column start

/* Total sub-columns the cursor walks through (per track):
 *   0  note
 *   1  vel-hi    2  vel-lo
 *   3  fx1-let   4  fx1-hi  5  fx1-lo
 *   6  fx2-let   7  fx2-hi  8  fx2-lo */
constexpr int kNumSubCols = 9;

/* Dark palette. */
const juce::Colour kBgColour          { 0xff'18'18'18 };
const juce::Colour kGutterColour      { 0xff'14'14'14 };
const juce::Colour kRowDividerColour  { 0xff'22'22'22 };
const juce::Colour kBeatHighlight     { 0xff'1f'1f'1f };
const juce::Colour kPlayheadHighlight { 0x66'ff'a0'40 }; // amber, translucent
const juce::Colour kCursorHighlight   { 0x88'40'a0'ff }; // cyan, translucent
const juce::Colour kRowTextColour     { 0xff'6a'6a'6a };
const juce::Colour kNoteTextColour    { 0xff'd4'd4'd4 };
const juce::Colour kEmptyCellColour   { 0xff'3a'3a'3a };
const juce::Colour kVelTextColour     { 0xff'd0'80'40 };
const juce::Colour kEditModeColour    { 0xff'e0'40'40 };

const juce::Colour kTrackTints[] = {
    juce::Colour { 0xff'c5'5a'5a }, // red
    juce::Colour { 0xff'c5'8a'4a }, // orange
    juce::Colour { 0xff'b5'b5'4a }, // yellow
    juce::Colour { 0xff'6a'b5'5a }, // green
    juce::Colour { 0xff'4a'a5'b5 }, // cyan
    juce::Colour { 0xff'5a'7a'c5 }, // blue
    juce::Colour { 0xff'9a'5a'c5 }, // purple
    juce::Colour { 0xff'c5'5a'9a }, // pink
};
inline juce::Colour trackTint (int idx) {
    return kTrackTints[((unsigned) idx) % (sizeof (kTrackTints) / sizeof (kTrackTints[0]))];
}

juce::String formatNote (int note)
{
    static const char* names[12] = {
        "C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"
    };
    const int octave = (note / 12) - 1;
    const int pc     = note % 12;
    return juce::String (names[pc]) + juce::String (octave);
}

/* QWERTY -> tracker semitone-offset map.
 * Lower row (Z-row): octave 0 of base.
 * Upper row (Q-row): octave 1 of base (i.e. +12 semitones).
 *
 * Returns -1 if the key isn't a note key. */
int qwertyToSemitone (int keyCode)
{
    switch (keyCode)
    {
        // lower row: Z S X D C V G B H N J M ,
        case 'Z': return 0;
        case 'S': return 1;
        case 'X': return 2;
        case 'D': return 3;
        case 'C': return 4;
        case 'V': return 5;
        case 'G': return 6;
        case 'B': return 7;
        case 'H': return 8;
        case 'N': return 9;
        case 'J': return 10;
        case 'M': return 11;
        case ',': return 12;
        case 'L': return 13; // C# next
        case '.': return 14; // D
        // upper row: Q 2 W 3 E R 5 T 6 Y 7 U I
        case 'Q': return 12;
        case '2': return 13;
        case 'W': return 14;
        case '3': return 15;
        case 'E': return 16;
        case 'R': return 17;
        case '5': return 18;
        case 'T': return 19;
        case '6': return 20;
        case 'Y': return 21;
        case '7': return 22;
        case 'U': return 23;
        case 'I': return 24;
        case '9': return 25;
        case 'O': return 26;
        case '0': return 27;
        case 'P': return 28;
        default: return -1;
    }
}

} // anonymous namespace


/* ===========================================================================
 * PatternView — the grid.  Owns the edit cursor + handles keyboard input.
 * =========================================================================*/
class TrackerEditor::PatternView : public juce::Component
{
public:
    explicit PatternView (TrackerNode* node) : trackerNode (node)
    {
        setWantsKeyboardFocus (true);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (kBgColour);

        if (trackerNode == nullptr) return;

        Snapshot s;
        snapshot (s);

        paintHeader (g, s);
        paintGutter (g, s);
        paintCells  (g, s);

        ensurePlayheadVisible (s.playheadRow);

        if (showHelp)
            paintHelp (g);
    }

    int desiredWidth (int ntrk) const
    {
        return kRowGutterWidth + juce::jmax (1, ntrk) * kTrackWidth;
    }
    int desiredHeight (int rows) const
    {
        return kTrackHeaderH + juce::jmax (1, rows) * kRowHeight;
    }

    /** Resize this Component to the full pattern extent so the parent
     *  Viewport can scroll. Called from the editor's timer + resized. */
    void updateGridSize()
    {
        if (trackerNode == nullptr) return;

        int rows = 16, ntrk = 1;
        {
            juce::ScopedLock sl (trackerNode->engineLock());
            if (auto* mod = trackerNode->modulePtr())
            {
                if (auto* seq = mod->curr_seq)
                {
                    rows = seq->length;
                    ntrk = seq->ntrk;
                }
            }
        }
        const int w = desiredWidth (ntrk);
        const int h = desiredHeight (rows);
        if (getWidth() != w || getHeight() != h)
            setSize (w, h);
    }

    bool keyPressed (const juce::KeyPress& kp) override
    {
        const int kc = kp.getKeyCode();
        const auto mods = kp.getModifiers();
        const bool ctrl  = mods.isCtrlDown() || mods.isCommandDown();
        const bool shift = mods.isShiftDown();

        // F1 / ? toggles help overlay.
        if (kc == juce::KeyPress::F1Key || kc == '?')
        {
            showHelp = ! showHelp;
            repaint();
            return true;
        }

        // Any key while help is up dismisses it.
        if (showHelp)
        {
            showHelp = false;
            repaint();
            return true;
        }

        // Ctrl-modified shortcuts first so plain arrow handlers below
        // don't shadow Ctrl+Up/Down.
        if (ctrl)
        {
            if (kc == juce::KeyPress::upKey)
                { changePatternLength (shift ?  16 :  1); return true; }
            if (kc == juce::KeyPress::downKey)
                { changePatternLength (shift ? -16 : -1); return true; }
            if (kc == juce::KeyPress::pageUpKey)
                { switchPattern (-1); return true; }
            if (kc == juce::KeyPress::pageDownKey)
                { switchPattern ( 1); return true; }
            if (kc == 'N' || kc == 'n') { newPattern();          return true; }
            if (kc == 'D' || kc == 'd') { duplicatePattern();    return true; }
            if (kc == 'T' || kc == 't') { addTrack();            return true; }
            if (kc == 'W' || kc == 'w')
            {
                if (shift) deletePattern();
                else       deleteCurrentTrack();
                return true;
            }
            if (kc == 'Z' || kc == 'z')
            {
                if (shift) redoOp(); else undoOp();
                return true;
            }
            if (kc == 'Y' || kc == 'y') { redoOp();        return true; }
            if (kc == 'C' || kc == 'c') { copySelection(); return true; }
            if (kc == 'X' || kc == 'x') { cutSelection();  return true; }
            if (kc == 'V' || kc == 'v') { pasteClipboard();return true; }
            if (kc == 'A' || kc == 'a')
            {
                /* Select All — anchor at (0,0), cursor at (length-1, ntrk-1). */
                Snapshot s; snapshot (s);
                selAnchorRow = 0; selAnchorTrack = 0;
                cursorRow = s.rows - 1; cursorTrack = s.ntrk - 1;
                selActive = true;
                repaint();
                return true;
            }
        }

        // ESC toggles edit mode
        if (kc == juce::KeyPress::escapeKey)
        {
            editMode = ! editMode;
            repaint();
            return true;
        }

        /* Navigation. Shift-modified navigation extends the selection;
         * plain navigation clears it. */
        auto nav = [&] (auto&& fn) {
            if (shift) extendSelectionToCursor();
            else       clearSelection();
            fn();
            if (shift) { selActive = true; repaint(); }
        };
        if (kc == juce::KeyPress::upKey)         { nav ([&]{ moveCursor (-1, 0); });  return true; }
        if (kc == juce::KeyPress::downKey)       { nav ([&]{ moveCursor ( 1, 0); });  return true; }
        if (kc == juce::KeyPress::leftKey)       { nav ([&]{ stepSubCol (-1); });     return true; }
        if (kc == juce::KeyPress::rightKey)      { nav ([&]{ stepSubCol ( 1); });     return true; }
        if (kc == juce::KeyPress::pageUpKey)     { nav ([&]{ moveCursor (-16, 0); }); return true; }
        if (kc == juce::KeyPress::pageDownKey)   { nav ([&]{ moveCursor ( 16, 0); }); return true; }
        if (kc == juce::KeyPress::homeKey)       { nav ([&]{ setCursorAbs (0, cursorTrack); }); return true; }
        if (kc == juce::KeyPress::endKey)        { nav ([&]{ setCursorAbs (INT_MAX, cursorTrack); }); return true; }

        // Insert / Shift+Insert: insert / delete row at cursor (current track).
        if (kc == juce::KeyPress::insertKey && editMode)
        {
            if (shift) deleteRowAtCursor();
            else       insertRowAtCursor();
            return true;
        }

        // Octave change: [ ]
        if (kc == '[') { octave = juce::jmax (0, octave - 1); repaint(); return true; }
        if (kc == ']') { octave = juce::jmin (8, octave + 1); repaint(); return true; }
        // Fast octave set: number keys 1, 4, 5, 8 (avoid clashing with note input
        // upper-row 2, 3, 5, 6, 7 sharps — Shift modifier makes the distinction).
        if (kp.getModifiers().isShiftDown() && kc >= '1' && kc <= '8')
        {
            octave = kc - '0';
            repaint();
            return true;
        }

        // Tab / Shift+Tab: jump tracks.  (Sub-column traversal moved
        // to Left/Right arrows below.)
        if (kc == juce::KeyPress::tabKey)
        {
            Snapshot s;
            snapshot (s);
            if (shift)
                cursorTrack = juce::jmax (0, cursorTrack - 1);
            else
                cursorTrack = juce::jmin (s.ntrk - 1, cursorTrack + 1);
            cursorSubCol = 0;
            ensureCursorVisible();
            repaint();
            return true;
        }

        if (! editMode) return false;

        // Delete / backspace — context-aware clear.
        if (kc == juce::KeyPress::deleteKey || kc == juce::KeyPress::backspaceKey)
        {
            if (selActive)                                   clearSelectionCells();
            else if (cursorSubCol == 0)                      writeCell (-1);
            else if (cursorSubCol == 1 || cursorSubCol == 2) writeVelocityNybble (0);
            else if (cursorSubCol == 3 || cursorSubCol == 6) writeFxLetter (0);
            else                                              writeFxNybble (0);
            return true;
        }

        /* Velocity sub-columns: hex digits write nybbles. */
        if (cursorSubCol == 1 || cursorSubCol == 2)
        {
            int nybble = -1;
            if      (kc >= '0' && kc <= '9') nybble = kc - '0';
            else if (kc >= 'A' && kc <= 'F') nybble = 10 + (kc - 'A');
            else if (kc >= 'a' && kc <= 'f') nybble = 10 + (kc - 'a');
            if (nybble >= 0) { writeVelocityNybble (nybble); return true; }
            return false;
        }

        /* FX letter sub-columns: accept A-Z and 0-9 (FT2 effect codes). */
        if (cursorSubCol == 3 || cursorSubCol == 6)
        {
            const int up = (kc >= 'a' && kc <= 'z') ? (kc - 32) : kc;
            if ((up >= 'A' && up <= 'Z') || (up >= '0' && up <= '9'))
            { writeFxLetter (up); return true; }
            return false;
        }

        /* FX param hex sub-columns: hex digits. */
        if (cursorSubCol == 4 || cursorSubCol == 5
            || cursorSubCol == 7 || cursorSubCol == 8)
        {
            int nybble = -1;
            if      (kc >= '0' && kc <= '9') nybble = kc - '0';
            else if (kc >= 'A' && kc <= 'F') nybble = 10 + (kc - 'A');
            else if (kc >= 'a' && kc <= 'f') nybble = 10 + (kc - 'a');
            if (nybble >= 0) { writeFxNybble (nybble); return true; }
            return false;
        }

        /* Note input.  JUCE on X11 returns the raw keysym for letter
         * keys — `z` unshifted comes through as ASCII 122, not 90. Fold
         * to uppercase so qwertyToSemitone's `case 'Z':` matches either. */
        const int kcUpper = (kc >= 'a' && kc <= 'z') ? (kc - 32) : kc;
        const int semis = qwertyToSemitone (kcUpper);
        if (semis >= 0)
        {
            const int midiNote = juce::jlimit (0, 127, (octave + 1) * 12 + semis);
            previewNote (midiNote); // audible preview before commit
            writeCell (midiNote);
            return true;
        }

        return false;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();

        Snapshot s;
        snapshot (s);

        const int x = e.x;
        const int y = e.y;
        if (x < kRowGutterWidth) return;

        /* Click in track header (top band):
         *   plain                  → toggle mute
         *   Shift+click            → solo (mute others; re-solo cancels)
         *   click in channel pill  → cycle channel (right-click reverses) */
        if (y >= 0 && y < kTrackHeaderH)
        {
            const int trk = (x - kRowGutterWidth) / kTrackWidth;
            if (trk >= 0 && trk < s.ntrk)
            {
                const int trkXBase = kRowGutterWidth + trk * kTrackWidth;
                const int pillRight = trkXBase + kTrackWidth - 8;
                const int pillLeft  = trkXBase + kTrackWidth - 36;
                const bool inPill = (x >= pillLeft && x <= pillRight && y <= 22);
                if (inPill)
                {
                    /* Cycle channel; jump cursor to this track first
                     * so cycleCursorTrackChannel acts on the clicked one. */
                    cursorTrack = trk;
                    cycleCursorTrackChannel (e.mods.isRightButtonDown() ? -1 : 1);
                }
                else if (e.mods.isShiftDown())
                {
                    soloTrack (trk);
                }
                else
                {
                    toggleTrackMute (trk);
                }
            }
            return;
        }

        /* Click in cell → jump cursor there. Shift+click extends
         * selection from existing anchor; plain click clears it. */
        const int row = (y - kTrackHeaderH) / kRowHeight;
        const int trk = (x - kRowGutterWidth) / kTrackWidth;
        if (row < 0 || row >= s.rows || trk < 0 || trk >= s.ntrk) return;

        if (e.mods.isShiftDown())
        {
            extendSelectionToCursor();
            cursorRow = row;
            cursorTrack = trk;
            selActive = true;
        }
        else
        {
            clearSelection();
            cursorRow = row;
            cursorTrack = trk;
        }
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        Snapshot s; snapshot (s);
        const int row = (e.y - kTrackHeaderH) / kRowHeight;
        const int trk = (e.x - kRowGutterWidth) / kTrackWidth;
        if (row < 0 || row >= s.rows || trk < 0 || trk >= s.ntrk) return;

        if (! selActive) extendSelectionToCursor();
        cursorRow = row;
        cursorTrack = trk;
        selActive = true;
        repaint();
    }

private:
    struct Cell {
        int type = 0; int note = 0; int velocity = 0;
        int fx[2] {0, 0};
        int fxParam[2] {0, 0};
    };
    struct TrackS {
        int port = 0, channel = 0, ncols = 1;
        bool muted = false;
        std::vector<Cell> cells;
    };
    struct Snapshot {
        int rows = 16, ntrk = 1, rpb = 4;
        int playheadRow = -1;   // -1 = not playing
        int patternIndex = 0, patternCount = 1;
        std::vector<TrackS> tracks;
    };

    void snapshot (Snapshot& s) const
    {
        if (trackerNode == nullptr) return;
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;

        auto* seq = mod->curr_seq;
        s.rows = seq->length;
        s.ntrk = seq->ntrk;
        s.rpb  = seq->rpb;

        if (mod->playing)
        {
            int p = (int) seq->pos;
            if (p < 0) p = 0;
            if (p >= seq->length) p = seq->length - 1;
            s.playheadRow = p;
        }

        /* Pattern N / M — find index of curr_seq in mod->seq[]. */
        s.patternCount = mod->nseq;
        for (int p = 0; p < mod->nseq; ++p)
            if (mod->seq[p] == seq) { s.patternIndex = p; break; }

        s.tracks.resize ((size_t) seq->ntrk);
        for (int t = 0; t < seq->ntrk; ++t)
        {
            auto* trk = seq->trk[t];
            if (! trk) continue;
            s.tracks[(size_t) t].port    = trk->port;
            s.tracks[(size_t) t].channel = trk->channel;
            s.tracks[(size_t) t].ncols   = trk->ncols;
            s.tracks[(size_t) t].muted   = (trk->playing == 0);
            s.tracks[(size_t) t].cells.resize ((size_t) seq->length);
            for (int r = 0; r < seq->length; ++r)
            {
                auto& cell = s.tracks[(size_t) t].cells[(size_t) r];
                const auto& row = trk->rows[0][r]; // col 0
                cell.type     = row.type;
                cell.note     = row.note;
                cell.velocity = row.velocity;
                cell.fx[0]      = row.fx[0];
                cell.fx[1]      = row.fx[1];
                cell.fxParam[0] = row.fxParam[0];
                cell.fxParam[1] = row.fxParam[1];
            }
        }
    }

    void paintHeader (juce::Graphics& g, const Snapshot& s)
    {
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      kHeaderFontSize, juce::Font::bold));

        g.setColour (kGutterColour);
        g.fillRect (0, 0, kRowGutterWidth, kTrackHeaderH);

        // Edit-mode indicator dot in the header corner.
        if (editMode)
        {
            g.setColour (kEditModeColour);
            g.fillEllipse (4.0f, 4.0f, 8.0f, 8.0f);
        }
        // Octave indicator (top-left of gutter, below the dot).
        g.setColour (kRowTextColour);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      kHeaderFontSize - 2.0f, juce::Font::plain));
        g.drawText (juce::String ("o") + juce::String (octave),
                    2, kTrackHeaderH - 14, kRowGutterWidth - 4, 12,
                    juce::Justification::centred);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      kHeaderFontSize, juce::Font::bold));

        for (int t = 0; t < s.ntrk; ++t)
        {
            const int x = kRowGutterWidth + t * kTrackWidth;
            const auto fullTint = trackTint (t);
            const bool muted = (size_t) t < s.tracks.size() && s.tracks[(size_t) t].muted;
            const auto tint = muted ? fullTint.withSaturation (0.2f).withBrightness (0.4f)
                                    : fullTint;

            g.setColour (tint);
            g.fillRect (x, 0, kTrackWidth - 1, 6);

            g.setColour (tint.withAlpha (0.18f));
            g.fillRect (x, 6, kTrackWidth - 1, kTrackHeaderH - 6);

            g.setColour (tint);
            g.drawText (juce::String::formatted ("Track%02d", t),
                        x + 6, 6, kTrackWidth - 28, 14,
                        juce::Justification::centredLeft);

            /* Channel pill (top-right of track header). */
            const int chan = (size_t) t < s.tracks.size()
                                ? s.tracks[(size_t) t].channel + 1
                                : t + 1;
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                          kHeaderFontSize - 2.0f, juce::Font::plain));
            g.drawText (juce::String::formatted ("ch%02d", chan),
                        x + kTrackWidth - 36, 6, 28, 14,
                        juce::Justification::centredRight);

            /* Bottom sub-label: M indicator when muted, else "Note  Vel". */
            if (muted)
            {
                g.setColour (juce::Colours::red.withAlpha (0.85f));
                g.drawText ("MUTE",
                            x + 6, kTrackHeaderH - 14, kTrackWidth - 12, 12,
                            juce::Justification::centredLeft);
            }
            else
            {
                g.setColour (juce::Colours::white.withAlpha (0.45f));
                g.drawText ("Note  Vel",
                            x + 6, kTrackHeaderH - 14, kTrackWidth - 12, 12,
                            juce::Justification::centredLeft);
            }
            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                          kHeaderFontSize, juce::Font::bold));
        }
    }

    void paintGutter (juce::Graphics& g, const Snapshot& s)
    {
        g.setColour (kGutterColour);
        g.fillRect (0, kTrackHeaderH, kRowGutterWidth, s.rows * kRowHeight);

        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      kCellFontSize, juce::Font::plain));

        for (int r = 0; r < s.rows; ++r)
        {
            const int y = kTrackHeaderH + r * kRowHeight;
            const bool isBeat = (s.rpb > 0) && (r % s.rpb == 0);
            if (isBeat)
            {
                g.setColour (kBeatHighlight);
                g.fillRect (0, y, kRowGutterWidth, kRowHeight);
            }
            if (r == s.playheadRow)
            {
                g.setColour (kPlayheadHighlight);
                g.fillRect (0, y, kRowGutterWidth, kRowHeight);
            }
            g.setColour (isBeat ? juce::Colours::white.withAlpha (0.55f)
                                : kRowTextColour);
            g.drawText (juce::String::toHexString (r).toUpperCase().paddedLeft ('0', 2),
                        4, y, kRowGutterWidth - 8, kRowHeight,
                        juce::Justification::centredRight);
        }
    }

    void paintCells (juce::Graphics& g, const Snapshot& s)
    {
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      kCellFontSize, juce::Font::plain));

        for (int t = 0; t < s.ntrk; ++t)
        {
            if ((size_t) t >= s.tracks.size()) continue;
            const auto& trk = s.tracks[(size_t) t];
            const int tx = kRowGutterWidth + t * kTrackWidth;

            for (int r = 0; r < s.rows; ++r)
            {
                const int y = kTrackHeaderH + r * kRowHeight;
                const bool isBeat = (s.rpb > 0) && (r % s.rpb == 0);

                if (isBeat)
                {
                    g.setColour (kBeatHighlight);
                    g.fillRect (tx, y, kTrackWidth - 1, kRowHeight);
                }
                if (r == s.playheadRow)
                {
                    g.setColour (kPlayheadHighlight);
                    g.fillRect (tx, y, kTrackWidth - 1, kRowHeight);
                }
                /* Selection rectangle fill (before cursor on top). */
                if (selActive)
                {
                    const int r0 = juce::jmin (selAnchorRow,   cursorRow);
                    const int r1 = juce::jmax (selAnchorRow,   cursorRow);
                    const int t0 = juce::jmin (selAnchorTrack, cursorTrack);
                    const int t1 = juce::jmax (selAnchorTrack, cursorTrack);
                    if (r >= r0 && r <= r1 && t >= t0 && t <= t1)
                    {
                        g.setColour (juce::Colour { 0x55'80'40'a0 });
                        g.fillRect (tx, y, kTrackWidth - 1, kRowHeight);
                    }
                }

                if (r == cursorRow && t == cursorTrack)
                {
                    g.setColour (kCursorHighlight);
                    int sx = 0, sw = 0;
                    switch (cursorSubCol)
                    {
                        case 0: sx = tx + kNoteX;                 sw = kNoteW;     break;
                        case 1: sx = tx + kVelX;                  sw = kVelHalfW;  break;
                        case 2: sx = tx + kVelX + kVelHalfW;      sw = kVelHalfW;  break;
                        case 3: sx = tx + kFx1X;                  sw = kFxCharW;   break;
                        case 4: sx = tx + kFx1X + kFxCharW;       sw = kFxCharW;   break;
                        case 5: sx = tx + kFx1X + 2 * kFxCharW;   sw = kFxCharW;   break;
                        case 6: sx = tx + kFx2X;                  sw = kFxCharW;   break;
                        case 7: sx = tx + kFx2X + kFxCharW;       sw = kFxCharW;   break;
                        case 8: sx = tx + kFx2X + 2 * kFxCharW;   sw = kFxCharW;   break;
                    }
                    g.fillRect (sx, y, sw, kRowHeight);
                }

                g.setColour (kRowDividerColour);
                g.fillRect (tx + kTrackWidth - 1, y, 1, kRowHeight);

                if (! trk.cells.empty() && r < (int) trk.cells.size())
                {
                    drawCell (g, trk.cells[(size_t) r], tx, y, kRowHeight);
                }
                else
                {
                    drawEmptyCell (g, tx, y, kRowHeight);
                }
            }
        }
    }

    /* x is the track-left edge (tx).  We draw three column groups:
     *   note (kNoteX), velocity (kVelX), FX1 (kFx1X), FX2 (kFx2X). */
    void drawCell (juce::Graphics& g, const Cell& cell, int x, int y, int h)
    {
        /* Note + velocity */
        if (cell.type == 1)
        {
            g.setColour (kNoteTextColour);
            g.drawText (formatNote (cell.note),
                        x + kNoteX, y, kNoteW, h,
                        juce::Justification::centredLeft);
            g.setColour (kVelTextColour);
            g.drawText (juce::String::toHexString (cell.velocity).toUpperCase().paddedLeft ('0', 2),
                        x + kVelX, y, kVelW, h,
                        juce::Justification::centredLeft);
        }
        else if (cell.type == 2)
        {
            g.setColour (kEmptyCellColour.brighter (0.4f));
            g.drawText ("OFF",
                        x + kNoteX, y, kNoteW, h,
                        juce::Justification::centredLeft);
            g.setColour (kEmptyCellColour);
            g.drawText ("--", x + kVelX, y, kVelW, h,
                        juce::Justification::centredLeft);
        }
        else
        {
            g.setColour (kEmptyCellColour);
            g.drawText ("---", x + kNoteX, y, kNoteW, h,
                        juce::Justification::centredLeft);
            g.drawText ("--", x + kVelX, y, kVelW, h,
                        juce::Justification::centredLeft);
        }

        /* FX columns */
        drawFxCell (g, cell.fx[0], cell.fxParam[0], x + kFx1X, y, h);
        drawFxCell (g, cell.fx[1], cell.fxParam[1], x + kFx2X, y, h);
    }

    void drawFxCell (juce::Graphics& g, int fxType, int fxParam,
                     int x, int y, int h)
    {
        const bool hasFx = fxType > 0;
        const juce::Colour fxLetterColour { 0xff'7a'c0'd4 };
        const juce::Colour fxParamColour  { 0xff'a0'a0'a0 };

        if (hasFx)
        {
            g.setColour (fxLetterColour);
            char ch = (char) (fxType & 0x7f);
            g.drawText (juce::String::charToString ((juce_wchar) ch),
                        x, y, kFxCharW, h, juce::Justification::centred);
            g.setColour (fxParamColour);
            const int hi = (fxParam >> 4) & 0x0f;
            const int lo = fxParam & 0x0f;
            const auto hex = juce::String ("0123456789ABCDEF");
            g.drawText (hex.substring (hi, hi + 1),
                        x + kFxCharW, y, kFxCharW, h, juce::Justification::centred);
            g.drawText (hex.substring (lo, lo + 1),
                        x + 2 * kFxCharW, y, kFxCharW, h, juce::Justification::centred);
        }
        else
        {
            g.setColour (kEmptyCellColour);
            g.drawText (".", x, y, kFxCharW, h, juce::Justification::centred);
            g.drawText (".", x + kFxCharW, y, kFxCharW, h, juce::Justification::centred);
            g.drawText (".", x + 2 * kFxCharW, y, kFxCharW, h, juce::Justification::centred);
        }
    }

    void drawEmptyCell (juce::Graphics& g, int x, int y, int h)
    {
        g.setColour (kEmptyCellColour);
        g.drawText ("---", x + kNoteX, y, kNoteW, h, juce::Justification::centredLeft);
        g.drawText ("--",  x + kVelX,  y, kVelW,  h, juce::Justification::centredLeft);
        drawFxCell (g, 0, 0, x + kFx1X, y, h);
        drawFxCell (g, 0, 0, x + kFx2X, y, h);
    }

    void moveCursor (int dRow, int dTrack)
    {
        Snapshot s;
        snapshot (s);
        setCursorAbs (cursorRow + dRow, cursorTrack + dTrack, s);
    }

    void setCursorAbs (int row, int trk)
    {
        Snapshot s;
        snapshot (s);
        setCursorAbs (row, trk, s);
    }

    void setCursorAbs (int row, int trk, const Snapshot& s)
    {
        cursorRow   = juce::jlimit (0, juce::jmax (0, s.rows - 1), row);
        cursorTrack = juce::jlimit (0, juce::jmax (0, s.ntrk - 1), trk);
        ensureCursorVisible();
        repaint();
    }

    /** Step cursor one sub-column left/right.  At track boundaries
     *  cross into the previous/next track. */
    void stepSubCol (int dir)
    {
        Snapshot s;
        snapshot (s);
        if (dir > 0)
        {
            if (cursorSubCol < kNumSubCols - 1)
                ++cursorSubCol;
            else if (cursorTrack < s.ntrk - 1)
            {
                ++cursorTrack;
                cursorSubCol = 0;
            }
        }
        else
        {
            if (cursorSubCol > 0)
                --cursorSubCol;
            else if (cursorTrack > 0)
            {
                --cursorTrack;
                cursorSubCol = kNumSubCols - 1;
            }
        }
        ensureCursorVisible();
        repaint();
    }

    /** When follow-playhead is on + playing, scroll viewport to keep
     *  the playhead row roughly centred. Called from the editor's
     *  timer via updateGridSize-style cadence. */
    void ensurePlayheadVisible (int playheadRow)
    {
        if (! followPlayhead || playheadRow < 0) return;
        if (auto* vp = findParentComponentOfClass<juce::Viewport>())
        {
            const int rowY = kTrackHeaderH + playheadRow * kRowHeight;
            const auto vis = vp->getViewArea();
            if (rowY < vis.getY() + 2 * kRowHeight
                || rowY > vis.getBottom() - 3 * kRowHeight)
            {
                vp->setViewPosition (vis.getX(),
                                     juce::jmax (0, rowY - vis.getHeight() / 2));
            }
        }
    }

    /** Scroll the parent Viewport so the cursor cell stays on screen. */
    void ensureCursorVisible()
    {
        if (auto* vp = findParentComponentOfClass<juce::Viewport>())
        {
            const int rowY = kTrackHeaderH + cursorRow * kRowHeight;
            const int trkX = kRowGutterWidth + cursorTrack * kTrackWidth;
            const juce::Rectangle<int> cell (trkX, rowY,
                                              kTrackWidth, kRowHeight);
            const auto vis = vp->getViewArea();
            if (! vis.contains (cell))
                vp->setViewPosition (
                    juce::jmax (0, trkX - vis.getWidth() / 2),
                    juce::jmax (0, rowY - vis.getHeight() / 2));
        }
    }

    /** Write a note (>= 0) or clear (< 0) at the cursor. Advances cursor
     *  by one row after a successful write — Renoise-style auto-step. */
    void writeCell (int midiNote)
    {
        if (trackerNode == nullptr) return;
        trackerNode->pushUndo();

        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;
        if (cursorTrack < 0 || cursorTrack >= seq->ntrk) return;
        auto* trk = seq->trk[cursorTrack];
        if (! trk || cursorRow < 0 || cursorRow >= seq->length) return;

        if (midiNote < 0)
        {
            track_set_row (trk, 0, cursorRow, 0, 0, 0, 0); // clear
        }
        else
        {
            track_set_row (trk, 0, cursorRow, 1, midiNote, 100, 0);
        }

        if (editStep > 0)
        {
            const int next = cursorRow + editStep;
            cursorRow = (next < seq->length) ? next : (next % seq->length);
        }
        ensureCursorVisible();
        repaint();
    }

    /** Write a letter into the FX-letter sub-column (cursorSubCol 3 or
     *  6).  Sets fx[slot] = ASCII letter; clears via 0. After write,
     *  advance cursor to the FX-hi sub-column of the same FX slot. */
    void writeFxLetter (int letter)
    {
        if (trackerNode == nullptr) return;
        const int slot = (cursorSubCol == 3) ? 0 : 1;
        if (cursorSubCol != 3 && cursorSubCol != 6) return;
        trackerNode->pushUndo();

        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;
        if (cursorTrack < 0 || cursorTrack >= seq->ntrk) return;
        auto* trk = seq->trk[cursorTrack];
        if (! trk || cursorRow < 0 || cursorRow >= seq->length) return;

        trk->rows[0][cursorRow].fx[slot] = letter;
        if (letter == 0)
            trk->rows[0][cursorRow].fxParam[slot] = 0;
        ++cursorSubCol;
        repaint();
    }

    /** Set one hex nybble of an FX param.
     *  cursorSubCol 4/7 = high nybble of FX1/FX2; 5/8 = low nybble.
     *  After low-nybble write, optionally advance to next row. */
    void writeFxNybble (int nybble)
    {
        if (trackerNode == nullptr) return;
        if (cursorSubCol < 4 || cursorSubCol > 8 || cursorSubCol == 6) return;
        const int slot = (cursorSubCol <= 5) ? 0 : 1;
        const bool isHi = (cursorSubCol == 4 || cursorSubCol == 7);
        trackerNode->pushUndo();

        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;
        if (cursorTrack < 0 || cursorTrack >= seq->ntrk) return;
        auto* trk = seq->trk[cursorTrack];
        if (! trk || cursorRow < 0 || cursorRow >= seq->length) return;

        int v = trk->rows[0][cursorRow].fxParam[slot];
        if (isHi) v = ((nybble & 0x0f) << 4) | (v & 0x0f);
        else      v = (v & 0xf0) | (nybble & 0x0f);
        trk->rows[0][cursorRow].fxParam[slot] = v & 0xff;

        if (isHi)
        {
            ++cursorSubCol; // hi → lo
        }
        else
        {
            /* After low-nybble: stay in same sub-column but auto-advance row. */
            if (editStep > 0)
            {
                const int next = cursorRow + editStep;
                cursorRow = (next < seq->length) ? next : (next % seq->length);
            }
            ensureCursorVisible();
        }
        repaint();
    }

    /** Set one hex nybble of the cell's velocity. cursorSubCol == 1
     *  writes the high nybble; == 2 writes the low nybble. After the
     *  low nybble, advance to next row + return cursor to note column. */
    void writeVelocityNybble (int nybble)
    {
        if (trackerNode == nullptr) return;
        if (cursorSubCol < 1 || cursorSubCol > 2) return;
        trackerNode->pushUndo();

        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;
        if (cursorTrack < 0 || cursorTrack >= seq->ntrk) return;
        auto* trk = seq->trk[cursorTrack];
        if (! trk || cursorRow < 0 || cursorRow >= seq->length) return;

        /* Velocity edit only applies if there's a note here. */
        const auto& current = trk->rows[0][cursorRow];
        if (current.type != 1) return; // not a note_on; ignore

        int vel = current.velocity;
        if (cursorSubCol == 1)
            vel = ((nybble & 0x0f) << 4) | (vel & 0x0f);
        else
            vel = (vel & 0xf0) | (nybble & 0x0f);
        vel = juce::jlimit (0, 127, vel);

        track_set_row (trk, 0, cursorRow, current.type, current.note, vel, 0);

        if (cursorSubCol == 1)
        {
            cursorSubCol = 2;
        }
        else
        {
            cursorSubCol = 0;
            if (editStep > 0)
            {
                const int next = cursorRow + editStep;
                cursorRow = (next < seq->length) ? next : (next % seq->length);
            }
            ensureCursorVisible();
        }
        repaint();
    }

public:
    /* === Public mutation / accessor surface for the toolbar.       === */

    void changePatternLength (int delta)
    {
        if (trackerNode == nullptr) return;
        trackerNode->pushUndo();
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;
        const int newLen = juce::jlimit (1, 1024, seq->length + delta);
        if (newLen == seq->length) return;
        sequence_set_length (seq, newLen);
        if (cursorRow >= newLen) cursorRow = newLen - 1;
        repaint();
    }

    void addTrack()
    {
        if (trackerNode == nullptr) return;
        trackerNode->pushUndo();
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;
        if (seq->ntrk >= 16) return;
        const int newCh = seq->ntrk;
        track* trk = track_new (0 /*port*/, newCh, seq->length, seq->length, TRACK_DEF_CTRLPR);
        sequence_add_track (seq, trk);
        cursorTrack = seq->ntrk - 1;
        repaint();
    }

    void deleteCurrentTrack()
    {
        if (trackerNode == nullptr) return;
        trackerNode->pushUndo();
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;
        if (seq->ntrk <= 1) return;
        if (cursorTrack < 0 || cursorTrack >= seq->ntrk) return;
        sequence_del_track (seq, cursorTrack);
        if (cursorTrack >= seq->ntrk) cursorTrack = seq->ntrk - 1;
        repaint();
    }

    /** Add a new empty pattern (same length as current). Engine
     *  advances all sequences in mod->seq[], but only those with
     *  playing=1 emit MIDI — we keep one-pattern-at-a-time semantics
     *  by gating playing across the curr_seq pointer. */
    void newPattern()
    {
        if (trackerNode == nullptr) return;
        trackerNode->pushUndo();
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr) return;
        const int len = mod->curr_seq ? mod->curr_seq->length : 16;

        sequence* seq = sequence_new (len);
        /* Mirror the test pattern's defaults: 1 track. */
        track* trk = track_new (0, 0, len, len, TRACK_DEF_CTRLPR);
        sequence_add_track (seq, trk);
        module_add_sequence (mod, seq);

        /* Pause all sequences except the new one. */
        for (int i = 0; i < mod->nseq; ++i)
            sequence_set_playing (mod->seq[i], 0);
        sequence_set_playing (seq, 1);
        mod->curr_seq = seq;

        cursorRow = 0;
        cursorTrack = 0;
        cursorSubCol = 0;
        repaint();
    }

    /** Switch the active pattern by delta (+1 / -1). */
    void switchPattern (int delta)
    {
        if (trackerNode == nullptr) return;
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->nseq == 0) return;

        int idx = 0;
        for (int i = 0; i < mod->nseq; ++i)
            if (mod->seq[i] == mod->curr_seq) { idx = i; break; }

        idx = (idx + delta + mod->nseq) % mod->nseq;
        auto* target = mod->seq[idx];

        for (int i = 0; i < mod->nseq; ++i)
            sequence_set_playing (mod->seq[i], 0);
        sequence_set_playing (target, 1);
        mod->curr_seq = target;

        /* Clamp cursor into new pattern bounds. */
        if (cursorRow >= target->length) cursorRow = target->length - 1;
        if (cursorTrack >= target->ntrk) cursorTrack = target->ntrk - 1;
        cursorSubCol = 0;
        repaint();
    }

    /** Toggle mute on track index t (sets trk->playing). When muting,
     *  kill currently-ringing notes so no stuck notes remain. */
    void toggleTrackMute (int t)
    {
        if (trackerNode == nullptr) return;
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;
        if (t < 0 || t >= seq->ntrk) return;
        auto* trk = seq->trk[t];
        if (! trk) return;

        if (trk->playing)
        {
            track_kill_notes (trk);
            trk->playing = 0;
        }
        else
        {
            trk->playing = 1;
        }
        repaint();
    }

    /** Solo track t: mute all others.  If t is already the only
     *  un-muted track, undo the solo (un-mute all). */
    void soloTrack (int t)
    {
        if (trackerNode == nullptr) return;
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;
        if (t < 0 || t >= seq->ntrk) return;

        /* Already solo'd? (t is unmuted, others all muted) → cancel. */
        bool alreadySolo = (seq->trk[t]->playing == 1);
        for (int i = 0; i < seq->ntrk && alreadySolo; ++i)
            if (i != t && seq->trk[i]->playing == 1)
                alreadySolo = false;

        for (int i = 0; i < seq->ntrk; ++i)
        {
            auto* trk = seq->trk[i];
            if (! trk) continue;
            int wantPlaying = alreadySolo ? 1 : (i == t ? 1 : 0);
            if (trk->playing == 1 && wantPlaying == 0)
                track_kill_notes (trk);
            trk->playing = wantPlaying;
        }
        repaint();
    }

    /** Insert a blank row at the cursor in the current track. Cells
     *  from cursorRow .. length-2 shift down by one; last row is lost. */
    void insertRowAtCursor()
    {
        if (trackerNode == nullptr) return;
        trackerNode->pushUndo();
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;
        if (cursorTrack < 0 || cursorTrack >= seq->ntrk) return;
        auto* trk = seq->trk[cursorTrack];
        if (! trk) return;

        for (int c = 0; c < trk->ncols; ++c)
        {
            for (int r = seq->length - 1; r > cursorRow; --r)
            {
                const auto& src = trk->rows[c][r - 1];
                track_set_row (trk, c, r, src.type, src.note, src.velocity, src.delay);
            }
            track_set_row (trk, c, cursorRow, 0, 0, 0, 0); // blank
        }
        repaint();
    }

    /** Delete the row at the cursor in the current track. Cells from
     *  cursorRow+1 .. length-1 shift up; last row becomes blank. */
    void deleteRowAtCursor()
    {
        if (trackerNode == nullptr) return;
        trackerNode->pushUndo();
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;
        if (cursorTrack < 0 || cursorTrack >= seq->ntrk) return;
        auto* trk = seq->trk[cursorTrack];
        if (! trk) return;

        for (int c = 0; c < trk->ncols; ++c)
        {
            for (int r = cursorRow; r < seq->length - 1; ++r)
            {
                const auto& src = trk->rows[c][r + 1];
                track_set_row (trk, c, r, src.type, src.note, src.velocity, src.delay);
            }
            track_set_row (trk, c, seq->length - 1, 0, 0, 0, 0);
        }
        repaint();
    }

    /** Clone current pattern as a new sequence + switch to it. */
    void duplicatePattern()
    {
        if (trackerNode == nullptr) return;
        trackerNode->pushUndo();
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        sequence* clone = sequence_clone (mod->curr_seq);
        if (! clone) return;
        module_add_sequence (mod, clone);

        for (int i = 0; i < mod->nseq; ++i)
            sequence_set_playing (mod->seq[i], 0);
        sequence_set_playing (clone, 1);
        mod->curr_seq = clone;

        cursorRow = 0;
        cursorTrack = 0;
        cursorSubCol = 0;
        repaint();
    }

    /** Delete the current pattern. Keeps at least one. */
    void deletePattern()
    {
        if (trackerNode == nullptr) return;
        trackerNode->pushUndo();
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->nseq <= 1 || mod->curr_seq == nullptr) return;

        int idx = 0;
        for (int i = 0; i < mod->nseq; ++i)
            if (mod->seq[i] == mod->curr_seq) { idx = i; break; }

        module_del_sequence (mod, idx);
        if (mod->nseq <= 0) return;

        const int newIdx = juce::jmin (idx, mod->nseq - 1);
        for (int i = 0; i < mod->nseq; ++i)
            sequence_set_playing (mod->seq[i], 0);
        mod->curr_seq = mod->seq[newIdx];
        sequence_set_playing (mod->curr_seq, 1);

        if (cursorRow   >= mod->curr_seq->length) cursorRow   = mod->curr_seq->length - 1;
        if (cursorTrack >= mod->curr_seq->ntrk)   cursorTrack = mod->curr_seq->ntrk - 1;
        cursorSubCol = 0;
        repaint();
    }

    bool getFollowPlayhead() const noexcept { return followPlayhead; }
    void toggleFollowPlayhead() { followPlayhead = ! followPlayhead; repaint(); }

    /* === Selection / clipboard / undo ================================ */

    bool hasSelection() const noexcept { return selActive; }
    void clearSelection() { selActive = false; repaint(); }

    /** Start or extend selection to current cursor position. */
    void extendSelectionToCursor()
    {
        if (! selActive)
        {
            selAnchorRow   = cursorRow;
            selAnchorTrack = cursorTrack;
            selActive = true;
        }
        repaint();
    }

    void selectionBounds (int& r0, int& r1, int& t0, int& t1) const
    {
        if (! selActive)
        {
            r0 = r1 = cursorRow;
            t0 = t1 = cursorTrack;
            return;
        }
        r0 = juce::jmin (selAnchorRow,   cursorRow);
        r1 = juce::jmax (selAnchorRow,   cursorRow);
        t0 = juce::jmin (selAnchorTrack, cursorTrack);
        t1 = juce::jmax (selAnchorTrack, cursorTrack);
    }

    void copySelection()
    {
        if (trackerNode == nullptr) return;
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;

        int r0, r1, t0, t1;
        selectionBounds (r0, r1, t0, t1);
        r0 = juce::jlimit (0, seq->length - 1, r0);
        r1 = juce::jlimit (0, seq->length - 1, r1);
        t0 = juce::jlimit (0, seq->ntrk - 1,   t0);
        t1 = juce::jlimit (0, seq->ntrk - 1,   t1);

        clipboard.assign ((size_t) (r1 - r0 + 1),
                          std::vector<ClipCell> ((size_t) (t1 - t0 + 1)));
        for (int r = r0; r <= r1; ++r)
        {
            for (int t = t0; t <= t1; ++t)
            {
                auto* trk = seq->trk[t];
                if (! trk) continue;
                const auto& src = trk->rows[0][r];
                auto& dst = clipboard[(size_t)(r - r0)][(size_t)(t - t0)];
                dst.type     = src.type;
                dst.note     = src.note;
                dst.velocity = src.velocity;
                dst.delay    = src.delay;
                dst.fx[0]      = src.fx[0];
                dst.fx[1]      = src.fx[1];
                dst.fxParam[0] = src.fxParam[0];
                dst.fxParam[1] = src.fxParam[1];
            }
        }
    }

    void cutSelection()
    {
        if (trackerNode == nullptr) return;
        copySelection();
        trackerNode->pushUndo();

        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;

        int r0, r1, t0, t1;
        selectionBounds (r0, r1, t0, t1);
        for (int r = r0; r <= r1; ++r)
        {
            for (int t = t0; t <= t1; ++t)
            {
                if (t < 0 || t >= seq->ntrk) continue;
                auto* trk = seq->trk[t];
                if (! trk) continue;
                track_set_row (trk, 0, r, 0, 0, 0, 0);
            }
        }
        repaint();
    }

    void clearSelectionCells()
    {
        if (! selActive) return;
        if (trackerNode == nullptr) return;
        trackerNode->pushUndo();

        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;

        int r0, r1, t0, t1;
        selectionBounds (r0, r1, t0, t1);
        for (int r = r0; r <= r1; ++r)
        {
            for (int t = t0; t <= t1; ++t)
            {
                if (t < 0 || t >= seq->ntrk) continue;
                auto* trk = seq->trk[t];
                if (! trk) continue;
                track_set_row (trk, 0, r, 0, 0, 0, 0);
            }
        }
        repaint();
    }

    void pasteClipboard()
    {
        if (trackerNode == nullptr || clipboard.empty()) return;
        trackerNode->pushUndo();

        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;

        const int rows  = (int) clipboard.size();
        const int trks  = (int) clipboard[0].size();
        for (int dr = 0; dr < rows; ++dr)
        {
            const int targetRow = cursorRow + dr;
            if (targetRow < 0 || targetRow >= seq->length) continue;
            for (int dt = 0; dt < trks; ++dt)
            {
                const int targetTrk = cursorTrack + dt;
                if (targetTrk < 0 || targetTrk >= seq->ntrk) continue;
                auto* trk = seq->trk[targetTrk];
                if (! trk) continue;
                const auto& src = clipboard[(size_t) dr][(size_t) dt];
                track_set_row (trk, 0, targetRow, src.type, src.note, src.velocity, src.delay);
                trk->rows[0][targetRow].fx[0]      = src.fx[0];
                trk->rows[0][targetRow].fx[1]      = src.fx[1];
                trk->rows[0][targetRow].fxParam[0] = src.fxParam[0];
                trk->rows[0][targetRow].fxParam[1] = src.fxParam[1];
            }
        }
        repaint();
    }

    void undoOp()
    {
        if (trackerNode) trackerNode->undo();
        clearSelection();
    }
    void redoOp()
    {
        if (trackerNode) trackerNode->redo();
        clearSelection();
    }

    /** Emit a brief audible preview of a note via the queue buffer, so
     *  the user hears what they're typing.  Schedules the note_off
     *  ~200 ms later via callAfterDelay; SafePointer guards against
     *  the editor being destroyed before the timer fires. */
    void previewNote (int midiNote)
    {
        if (trackerNode == nullptr) return;
        int port = 0, channel = 0;
        {
            juce::ScopedLock sl (trackerNode->engineLock());
            auto* mod = trackerNode->modulePtr();
            if (mod == nullptr || mod->curr_seq == nullptr) return;
            auto* seq = mod->curr_seq;
            if (cursorTrack < 0 || cursorTrack >= seq->ntrk) return;
            auto* trk = seq->trk[cursorTrack];
            if (! trk) return;
            port = trk->port;
            channel = trk->channel;
            queue_midi_note_on (mod->clt, seq, port, channel, midiNote, 100);
        }

        juce::Component::SafePointer<PatternView> weakSelf (this);
        juce::Timer::callAfterDelay (200,
            [weakSelf, midiNote, port, channel]()
            {
                if (auto* self = weakSelf.getComponent())
                {
                    if (self->trackerNode == nullptr) return;
                    juce::ScopedLock sl (self->trackerNode->engineLock());
                    auto* mod = self->trackerNode->modulePtr();
                    if (mod == nullptr || mod->curr_seq == nullptr) return;
                    queue_midi_note_off (mod->clt, mod->curr_seq, port, channel, midiNote);
                }
            });
    }

    /** Cycle the cursor track's MIDI channel (1..16) — channel pill
     *  click in header. */
    void cycleCursorTrackChannel (int delta = 1)
    {
        if (trackerNode == nullptr) return;
        trackerNode->pushUndo();
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;
        if (cursorTrack < 0 || cursorTrack >= seq->ntrk) return;
        auto* trk = seq->trk[cursorTrack];
        if (! trk) return;
        int ch = trk->channel + delta;
        while (ch < 0)  ch += 16;
        while (ch > 15) ch -= 16;
        trk->channel = ch;
        repaint();
    }

    /* === Edit-mode + octave + edit-step toggles ====================== */

    bool getEditMode() const noexcept { return editMode; }
    void toggleEditMode() { editMode = ! editMode; repaint(); }

    int  getOctave() const noexcept { return octave; }
    void changeOctave (int delta)
    {
        octave = juce::jlimit (0, 8, octave + delta);
        repaint();
    }

    int  getEditStep() const noexcept { return editStep; }
    void changeEditStep (int delta)
    {
        editStep = juce::jlimit (0, 16, editStep + delta);
        repaint();
    }

    bool getHelpVisible() const noexcept { return showHelp; }
    void toggleHelp() { showHelp = ! showHelp; repaint(); }

    /* === Read-only accessors for the toolbar status display ========== */

    int getPatternLength() const
    {
        if (trackerNode == nullptr) return 0;
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        return (mod && mod->curr_seq) ? mod->curr_seq->length : 0;
    }
    int getTrackCount() const
    {
        if (trackerNode == nullptr) return 0;
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        return (mod && mod->curr_seq) ? mod->curr_seq->ntrk : 0;
    }
    int getPatternIndex() const
    {
        if (trackerNode == nullptr) return 0;
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (! mod) return 0;
        for (int i = 0; i < mod->nseq; ++i)
            if (mod->seq[i] == mod->curr_seq) return i;
        return 0;
    }
    int getPatternCount() const
    {
        if (trackerNode == nullptr) return 0;
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        return mod ? mod->nseq : 0;
    }
    float getBPM() const
    {
        if (trackerNode == nullptr) return 120.f;
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        return mod ? mod->bpm : 120.f;
    }

private:
    void paintHelp (juce::Graphics& g)
    {
        if (auto* vp = findParentComponentOfClass<juce::Viewport>())
        {
            const auto vis = vp->getViewArea();
            g.setColour (juce::Colour { 0xee'08'08'08 });
            g.fillRect (vis);
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                          13.0f, juce::Font::plain));

            const juce::StringArray lines = {
                "TRACKER KEYBINDINGS",
                "",
                "EDIT MODE",
                "  ESC          toggle edit mode (red dot in corner)",
                "  Z S X D C V G B H N J M ,    notes (low row, C to C+1)",
                "  Q 2 W 3 E R 5 T 6 Y 7 U I    notes (high row, C+12 up)",
                "  [ / ]        octave -/+",
                "  Shift+1..8   set octave 1..8",
                "  Delete       clear note (or zero velocity nybble)",
                "  0-9 A-F      set velocity hex digit (in vel sub-cols)",
                "  Insert       insert row at cursor (current track)",
                "  Shift+Insert delete row at cursor",
                "",
                "NAVIGATION",
                "  Up / Down    move cursor row",
                "  Left / Right step sub-column (note / vel-hi / vel-lo,",
                "                  crosses tracks at boundaries)",
                "  Tab / S-Tab  jump to next / previous track",
                "  PgUp / PgDn  jump 16 rows",
                "  Home / End   first / last row",
                "  Click        jump cursor to cell",
                "  Click header        toggle track mute",
                "  Shift+Click header  solo track (others muted)",
                "  Click channel pill  cycle MIDI channel 1..16",
                "                      (right-click reverses)",
                "",
                "PATTERN / TRACKS",
                "  Ctrl+Up/Dn   pattern length -/+ 1",
                "  Ctrl+Shift+Up/Dn   length -/+ 16",
                "  Ctrl+T       add track",
                "  Ctrl+W       remove cursor track (keeps last)",
                "  Ctrl+N       new pattern",
                "  Ctrl+D       duplicate current pattern",
                "  Ctrl+Shift+W delete current pattern (keeps last)",
                "  Ctrl+PgUp/Dn switch pattern -/+ (wraps)",
                "",
                "SELECTION / CLIPBOARD / UNDO",
                "  Shift+Arrow / Shift+PgUp/Dn / Shift+Click",
                "                  extend selection",
                "  Ctrl+A       select all in current pattern",
                "  Ctrl+C       copy selection",
                "  Ctrl+X       cut selection",
                "  Ctrl+V       paste at cursor",
                "  Delete       clear selection (if active)",
                "  Ctrl+Z       undo",
                "  Ctrl+Y / Ctrl+Shift+Z   redo",
                "",
                "  F1 / ?       toggle this help",
                "",
                "(transport via Element's main play/stop; BPM follows",
                " Element.  Edit step + FOLLOW playhead on toolbar.",
                " Note keys preview through downstream synth. Live MIDI",
                " input recorded into pattern when Element is recording.)"
            };

            const int lineHeight = 18;
            int y = vis.getY() + 16;
            for (const auto& line : lines)
            {
                g.drawText (line, vis.getX() + 24, y, vis.getWidth() - 48, lineHeight,
                            juce::Justification::centredLeft);
                y += lineHeight;
            }
        }
    }

    /* Selection state — anchor + cursor define an inclusive rectangle.
     * Set on Shift+navigation; cleared on plain navigation. */
    int selAnchorRow   = 0;
    int selAnchorTrack = 0;
    bool selActive     = false;

    struct ClipCell {
        int type = 0; int note = 0; int velocity = 0; int delay = 0;
        int fx[2] {0, 0};
        int fxParam[2] {0, 0};
    };
    /* Clipboard contents: indexed by [rowOffset][trackOffset]. */
    std::vector<std::vector<ClipCell>> clipboard;

    TrackerNode* trackerNode;
    int cursorRow    = 0;
    int cursorTrack  = 0;
    int cursorSubCol = 0; // 0 = note, 1 = vel-hi, 2 = vel-lo
    int octave       = 4;
    int editStep     = 1; // auto-advance amount after a write (0 = no advance)
    bool editMode    = false;
    bool showHelp    = false;
    bool followPlayhead = true; // scroll viewport with the playhead during playback
};


/* ===========================================================================
 * Toolbar — top strip of buttons + status labels.  All actions delegate
 * back to the owning TrackerEditor (which then calls into PatternView /
 * TrackerNode under engineLock_).
 * =========================================================================*/
class TrackerEditor::Toolbar : public juce::Component
{
public:
    explicit Toolbar (TrackerEditor& ed) : editor (ed)
    {
        configureButton (editBtn,   "EDIT", [this]{ editor.toggleEditMode(); });
        editBtn.setClickingTogglesState (true);

        configureButton (helpBtn,   "?",    [this]{ editor.toggleHelp(); });
        configureButton (octMinusBtn,  "<", [this]{ editor.changeOctave (-1); });
        configureButton (octPlusBtn,   ">", [this]{ editor.changeOctave ( 1); });
        configureButton (stepMinusBtn, "<", [this]{ editor.changeEditStep (-1); });
        configureButton (stepPlusBtn,  ">", [this]{ editor.changeEditStep ( 1); });
        configureButton (lenMinusBtn,  "-", [this]{ editor.changePatternLength (-1); });
        configureButton (lenPlusBtn,   "+", [this]{ editor.changePatternLength ( 1); });
        configureButton (trkRemoveBtn, "-", [this]{ editor.deleteCurrentTrack(); });
        configureButton (trkAddBtn,    "+", [this]{ editor.addTrack(); });
        configureButton (patPrevBtn,   "<", [this]{ editor.switchPattern (-1); });
        configureButton (patNextBtn,   ">", [this]{ editor.switchPattern ( 1); });
        configureButton (patNewBtn,   "NEW",[this]{ editor.newPattern(); });
        configureButton (patDupBtn,   "DUP",[this]{ editor.duplicatePattern(); });
        configureButton (patDelBtn,   "DEL",[this]{ editor.deletePattern(); });
        configureButton (followBtn,   "FOLLOW", [this]{ editor.toggleFollowPlayhead(); });
        followBtn.setClickingTogglesState (true);

        configureButton (undoBtn, "UND", [this]{ editor.undoOp(); });
        configureButton (redoBtn, "RED", [this]{ editor.redoOp(); });

        configureLabel (octLabel);
        configureLabel (stepLabel);
        configureLabel (lenLabel);
        configureLabel (trkLabel);
        configureLabel (patLabel);
        configureLabel (bpmLabel);

        refresh();
    }

    /** Pull state from the editor and update widget text. Cheap; called
     *  from the editor's 30 Hz timer. */
    void refresh()
    {
        editBtn.setToggleState (editor.isEditMode(), juce::dontSendNotification);
        editBtn.setColour (juce::TextButton::buttonOnColourId,
                           juce::Colour { 0xff'c0'30'30 });
        followBtn.setToggleState (editor.getFollowPlayhead(), juce::dontSendNotification);
        followBtn.setColour (juce::TextButton::buttonOnColourId,
                             juce::Colour { 0xff'40'80'40 });
        undoBtn.setEnabled (editor.canUndo());
        redoBtn.setEnabled (editor.canRedo());

        octLabel .setText (juce::String::formatted ("OCT %d",   editor.getOctave()),       juce::dontSendNotification);
        stepLabel.setText (juce::String::formatted ("STEP %d",  editor.getEditStep()),     juce::dontSendNotification);
        lenLabel .setText (juce::String::formatted ("LEN %d",   editor.getPatternLength()),juce::dontSendNotification);
        trkLabel .setText (juce::String::formatted ("TRK %d",   editor.getTrackCount()),   juce::dontSendNotification);
        patLabel .setText (juce::String::formatted ("PAT %d/%d", editor.getPatternIndex() + 1,
                                                                 juce::jmax (1, editor.getPatternCount())),
                           juce::dontSendNotification);
        bpmLabel .setText (juce::String::formatted ("BPM %.1f", editor.getBPM()),          juce::dontSendNotification);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour { 0xff'24'24'24 });
        g.setColour (juce::Colour { 0xff'35'35'35 });
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    }

    void resized() override
    {
        int x = 6;
        const int y = 4;
        const int h = getHeight() - 8;
        const int btnW  = 24;
        const int lblW  = 64;
        const int wideW = 48;

        auto place = [&] (juce::Component& c, int w) {
            c.setBounds (x, y, w, h);
            x += w + 2;
        };
        auto sep = [&] { x += 8; };

        place (editBtn, wideW); sep();
        place (octMinusBtn, btnW);  place (octLabel, lblW);  place (octPlusBtn, btnW);  sep();
        place (stepMinusBtn, btnW); place (stepLabel, lblW); place (stepPlusBtn, btnW); sep();
        place (lenMinusBtn, btnW);  place (lenLabel, lblW);  place (lenPlusBtn, btnW);  sep();
        place (trkRemoveBtn, btnW); place (trkLabel, lblW);  place (trkAddBtn, btnW);   sep();
        place (patPrevBtn, btnW);   place (patLabel, 72);    place (patNextBtn, btnW);
        place (patNewBtn, 40);
        place (patDupBtn, 40);
        place (patDelBtn, 40);
        sep();
        place (followBtn, 56); sep();
        place (undoBtn, 36); place (redoBtn, 36); sep();
        place (bpmLabel, 80); sep();
        place (helpBtn, btnW);
    }

private:
    void configureButton (juce::TextButton& b, const juce::String& text,
                          std::function<void()> on)
    {
        b.setButtonText (text);
        b.onClick = std::move (on);
        b.setColour (juce::TextButton::buttonColourId,  juce::Colour { 0xff'2c'2c'2c });
        b.setColour (juce::TextButton::textColourOffId, juce::Colour { 0xff'd0'd0'd0 });
        b.setColour (juce::TextButton::textColourOnId,  juce::Colours::white);
        addAndMakeVisible (b);
    }
    void configureLabel (juce::Label& l)
    {
        l.setJustificationType (juce::Justification::centred);
        l.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      12.0f, juce::Font::plain));
        l.setColour (juce::Label::textColourId, juce::Colour { 0xff'c0'c0'c0 });
        addAndMakeVisible (l);
    }

    TrackerEditor& editor;
    juce::TextButton editBtn, helpBtn;
    juce::TextButton octMinusBtn,   octPlusBtn;
    juce::TextButton stepMinusBtn,  stepPlusBtn;
    juce::TextButton lenMinusBtn,   lenPlusBtn;
    juce::TextButton trkRemoveBtn,  trkAddBtn;
    juce::TextButton patPrevBtn,    patNextBtn, patNewBtn, patDupBtn, patDelBtn;
    juce::TextButton followBtn;
    juce::TextButton undoBtn, redoBtn;
    juce::Label octLabel, stepLabel, lenLabel, trkLabel, patLabel, bpmLabel;
};


/* =========================================================================== */

TrackerEditor::TrackerEditor (const Node& n)
    : NodeEditor (n)
{
    setOpaque (true);

    patternView.reset (new PatternView (getNodeObjectOfType<TrackerNode>()));

    viewport.reset (new juce::Viewport());
    viewport->setViewedComponent (patternView.get(), false);
    viewport->setScrollBarsShown (true, true);
    viewport->setWantsKeyboardFocus (false);  // forward to PatternView
    addAndMakeVisible (viewport.get());

    toolbar.reset (new Toolbar (*this));
    addAndMakeVisible (toolbar.get());

    patternView->grabKeyboardFocus();

    setResizable (true);
    /* Toolbar is ~36 tall; default body fits ~24 rows × 4 tracks. */
    constexpr int kToolbarH = 36;
    setSize (juce::jmax (1020, kRowGutterWidth + 4 * kTrackWidth + 16),
             kToolbarH + kTrackHeaderH + 24 * kRowHeight + 4);
    startTimerHz (30);
}

TrackerEditor::~TrackerEditor()
{
    stopTimer();
    toolbar.reset();
    viewport.reset();
    patternView.reset();
}

void TrackerEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBgColour);
}

void TrackerEditor::resized()
{
    constexpr int kToolbarH = 36;
    auto r = getLocalBounds();
    if (toolbar)
        toolbar->setBounds (r.removeFromTop (kToolbarH));
    if (viewport)
        viewport->setBounds (r);
    if (patternView)
        patternView->updateGridSize();
}

void TrackerEditor::timerCallback()
{
    if (patternView)
    {
        patternView->updateGridSize();
        patternView->repaint();
    }
    refreshToolbar();
}

void TrackerEditor::refreshToolbar()
{
    if (toolbar) toolbar->refresh();
}

/* === Bridge methods ====================================================== */

bool  TrackerEditor::isEditMode() const    { return patternView ? patternView->getEditMode() : false; }
void  TrackerEditor::toggleEditMode()      { if (patternView) patternView->toggleEditMode(); }
int   TrackerEditor::getOctave() const     { return patternView ? patternView->getOctave() : 4; }
void  TrackerEditor::changeOctave (int d)  { if (patternView) patternView->changeOctave (d); }
int   TrackerEditor::getEditStep() const   { return patternView ? patternView->getEditStep() : 1; }
void  TrackerEditor::changeEditStep (int d){ if (patternView) patternView->changeEditStep (d); }
int   TrackerEditor::getPatternLength() const { return patternView ? patternView->getPatternLength() : 0; }
void  TrackerEditor::changePatternLength (int d) { if (patternView) patternView->changePatternLength (d); }
int   TrackerEditor::getTrackCount() const { return patternView ? patternView->getTrackCount() : 0; }
void  TrackerEditor::addTrack()            { if (patternView) patternView->addTrack(); }
void  TrackerEditor::deleteCurrentTrack()  { if (patternView) patternView->deleteCurrentTrack(); }
int   TrackerEditor::getPatternIndex() const { return patternView ? patternView->getPatternIndex() : 0; }
int   TrackerEditor::getPatternCount() const { return patternView ? patternView->getPatternCount() : 1; }
void  TrackerEditor::newPattern()          { if (patternView) patternView->newPattern(); }
void  TrackerEditor::duplicatePattern()    { if (patternView) patternView->duplicatePattern(); }
void  TrackerEditor::deletePattern()       { if (patternView) patternView->deletePattern(); }
void  TrackerEditor::switchPattern (int d) { if (patternView) patternView->switchPattern (d); }
float TrackerEditor::getBPM() const        { return patternView ? patternView->getBPM() : 120.f; }
void  TrackerEditor::toggleHelp()          { if (patternView) patternView->toggleHelp(); }
bool  TrackerEditor::getFollowPlayhead() const { return patternView ? patternView->getFollowPlayhead() : false; }
void  TrackerEditor::toggleFollowPlayhead() { if (patternView) patternView->toggleFollowPlayhead(); }
void  TrackerEditor::undoOp() { if (patternView) patternView->undoOp(); }
void  TrackerEditor::redoOp() { if (patternView) patternView->redoOp(); }
bool  TrackerEditor::canUndo() const
{
    if (auto* tn = const_cast<TrackerEditor*> (this)->getNodeObjectOfType<TrackerNode>())
        return tn->canUndo();
    return false;
}
bool  TrackerEditor::canRedo() const
{
    if (auto* tn = const_cast<TrackerEditor*> (this)->getNodeObjectOfType<TrackerNode>())
        return tn->canRedo();
    return false;
}

} // namespace element
