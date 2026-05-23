// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/trackereditor.hpp"
#include "nodes/tracker.hpp"

namespace element {

namespace {

/* Layout constants. */
constexpr int kRowHeight        = 18;
constexpr int kRowGutterWidth   = 40;
constexpr int kTrackHeaderH     = 56;   /* tint + name/ch + inst + legend + MUTE/SOLO */
constexpr int kColumnSubWidth   = 32;
constexpr float kCellFontSize   = 13.0f;
constexpr float kHeaderFontSize = 12.0f;

/* Sub-column X positions (relative to the track's left edge) and widths.
 * Each column uses 12 px per character.  Aux columns (delay / prob /
 * vel-range / delay-range) sit between Vel and FX1, ordered to read as
 * "the modifiers that shape this note's trigger" before the per-row
 * engine effects in FX1 / FX2. */
constexpr int kNoteX      = 4;
constexpr int kNoteW      = 36;     // "C-5", incl trailing pad
constexpr int kVelX       = 44;     // velocity column
constexpr int kVelHalfW   = 12;     // each nybble character
constexpr int kVelW       = 24;     // hi+lo
constexpr int kAuxCharW   = 12;     // per digit / nybble in aux cells
constexpr int kAuxCellW   = 24;     // 2 chars
constexpr int kDelayX     = 72;     // Dl: delay (00..99 decimal = delay/100 row)
constexpr int kProbX      = 100;    // Pr: skip probability (00..99)
constexpr int kVrX        = 128;    // VR: velocity_range (00..FF hex, mirrors Vel)
constexpr int kDrX        = 156;    // DR: delay_range   (00..99 decimal)
constexpr int kFx1X       = 184;    // FX1 column start
constexpr int kFxCharW    = 12;     // letter / hi / lo each 12 px wide
constexpr int kFxColW     = 36;     // FX column total (letter + hi + lo)
constexpr int kFx2X       = 224;    // FX2 column start
constexpr int kTrackWidth = 264;    // full track including right padding

/* Total sub-columns the cursor walks through (per track):
 *   0   note
 *   1   vel-hi      2   vel-lo
 *   3   delay-hi    4   delay-lo       (decimal, 0-9 each)
 *   5   prob-hi     6   prob-lo        (decimal, 0-9 each)
 *   7   vr-hi       8   vr-lo          (hex 0-F)
 *   9   dr-hi      10   dr-lo          (decimal 0-9)
 *  11   fx1-let    12   fx1-hi   13   fx1-lo
 *  14   fx2-let    15   fx2-hi   16   fx2-lo */
constexpr int kNumSubCols = 17;

/* Sub-col index helpers -- kept as constexpr so the cursor logic
 * reads at call site rather than as bare integers. */
constexpr int kColNote      = 0;
constexpr int kColVelHi     = 1;
constexpr int kColVelLo     = 2;
constexpr int kColDelayHi   = 3;
constexpr int kColDelayLo   = 4;
constexpr int kColProbHi    = 5;
constexpr int kColProbLo    = 6;
constexpr int kColVrHi      = 7;
constexpr int kColVrLo      = 8;
constexpr int kColDrHi      = 9;
constexpr int kColDrLo      = 10;
constexpr int kColFx1Let    = 11;
constexpr int kColFx1Hi     = 12;
constexpr int kColFx1Lo     = 13;
constexpr int kColFx2Let    = 14;
constexpr int kColFx2Hi     = 15;
constexpr int kColFx2Lo     = 16;

/* Dark palette. */
const juce::Colour kBgColour          { 0xff'18'18'18 };
const juce::Colour kGutterColour      { 0xff'14'14'14 };
const juce::Colour kRowDividerColour  { 0xff'22'22'22 };
const juce::Colour kBeatHighlight     { 0xff'1f'1f'1f };
const juce::Colour kPlayheadHighlight { 0x66'ff'a0'40 }; // amber, translucent
const juce::Colour kCursorHighlight   { 0x88'40'a0'ff }; // cyan, translucent
const juce::Colour kRowTextColour     { 0xff'a8'a8'a8 };   // bumped (was 0x6a6a6a)
const juce::Colour kNoteTextColour    { 0xff'f0'f0'f0 };   // bumped (was 0xd4d4d4)
const juce::Colour kEmptyCellColour   { 0xff'3a'3a'3a };   // fill colour
const juce::Colour kEmptyTextColour   { 0xff'7a'7a'7a };   // "---" / "--" in empty cells -- distinct from cell fill so the text is readable
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

    /** Cheap UI tick — read just the playhead + structural state under
     *  lock and repaint only what changed.  Replaces the previous
     *  blanket repaint() at 30 Hz that was burning CPU on every frame. */
    void tickRepaint()
    {
        if (trackerNode == nullptr) return;

        int playheadRow = -1;
        int patternIndex = 0;
        int rows = 16, ntrk = 1;
        bool playing = false;
        {
            juce::ScopedLock sl (trackerNode->engineLock());
            if (auto* mod = trackerNode->modulePtr())
            {
                if (auto* seq = mod->curr_seq)
                {
                    rows = seq->length;
                    ntrk = seq->ntrk;
                    if (mod->playing)
                    {
                        int p = (int) seq->pos;
                        if (p < 0) p = 0;
                        if (p >= seq->length) p = seq->length - 1;
                        playheadRow = p;
                    }
                    playing = mod->playing != 0;
                }
                for (int p = 0; p < mod->nseq; ++p)
                    if (mod->seq[p] == mod->curr_seq) { patternIndex = p; break; }
            }
        }

        const bool structuralChange =
            (rows != lastRows_ || ntrk != lastNtrk_ || patternIndex != lastPatternIndex_);

        if (structuralChange)
        {
            const int w = desiredWidth (ntrk);
            const int h = desiredHeight (rows);
            if (getWidth() != w || getHeight() != h)
                setSize (w, h);
            repaint();
        }
        else if (playheadRow != lastPlayheadRow_)
        {
            /* Partial repaint: erase old row, paint new row.  This is the
             *  whole UI-perf budget every tracker frame — must stay tight. */
            const int gridW = getWidth();
            if (lastPlayheadRow_ >= 0)
            {
                const int y = kTrackHeaderH + lastPlayheadRow_ * kRowHeight;
                repaint (0, y, gridW, kRowHeight);
            }
            if (playheadRow >= 0)
            {
                const int y = kTrackHeaderH + playheadRow * kRowHeight;
                repaint (0, y, gridW, kRowHeight);
            }
        }

        if (playing && playheadRow >= 0)
            ensurePlayheadVisible (playheadRow);

        lastRows_         = rows;
        lastNtrk_         = ntrk;
        lastPatternIndex_ = patternIndex;
        lastPlayheadRow_  = playheadRow;
        lastPlaying_      = playing;
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
            if (selActive)                                    clearSelectionCells();
            else if (cursorSubCol == kColNote)                writeCell (-1);
            else if (cursorSubCol == kColVelHi
                  || cursorSubCol == kColVelLo)               writeVelocityNybble (0);
            else if (cursorSubCol == kColDelayHi
                  || cursorSubCol == kColDelayLo)             writeDelayDigit (0);
            else if (cursorSubCol == kColProbHi
                  || cursorSubCol == kColProbLo)              writeProbDigit (0);
            else if (cursorSubCol == kColVrHi
                  || cursorSubCol == kColVrLo)                writeVrNybble (0);
            else if (cursorSubCol == kColDrHi
                  || cursorSubCol == kColDrLo)                writeDrDigit (0);
            else if (cursorSubCol == kColFx1Let
                  || cursorSubCol == kColFx2Let)              writeFxLetter (0);
            else                                              writeFxNybble (0);
            return true;
        }

        /* Velocity sub-columns: hex digits write nybbles. */
        if (cursorSubCol == kColVelHi || cursorSubCol == kColVelLo)
        {
            int nybble = -1;
            if      (kc >= '0' && kc <= '9') nybble = kc - '0';
            else if (kc >= 'A' && kc <= 'F') nybble = 10 + (kc - 'A');
            else if (kc >= 'a' && kc <= 'f') nybble = 10 + (kc - 'a');
            if (nybble >= 0) { writeVelocityNybble (nybble); return true; }
            return false;
        }

        /* Delay sub-columns: decimal digits 0-9 (delay/100 row offset). */
        if (cursorSubCol == kColDelayHi || cursorSubCol == kColDelayLo)
        {
            if (kc >= '0' && kc <= '9') { writeDelayDigit (kc - '0'); return true; }
            return false;
        }

        /* Probability sub-columns: decimal 0-9 (skip-probability percent). */
        if (cursorSubCol == kColProbHi || cursorSubCol == kColProbLo)
        {
            if (kc >= '0' && kc <= '9') { writeProbDigit (kc - '0'); return true; }
            return false;
        }

        /* Velocity-range sub-columns: hex digits (mirrors Vel column). */
        if (cursorSubCol == kColVrHi || cursorSubCol == kColVrLo)
        {
            int nybble = -1;
            if      (kc >= '0' && kc <= '9') nybble = kc - '0';
            else if (kc >= 'A' && kc <= 'F') nybble = 10 + (kc - 'A');
            else if (kc >= 'a' && kc <= 'f') nybble = 10 + (kc - 'a');
            if (nybble >= 0) { writeVrNybble (nybble); return true; }
            return false;
        }

        /* Delay-range sub-columns: decimal 0-9. */
        if (cursorSubCol == kColDrHi || cursorSubCol == kColDrLo)
        {
            if (kc >= '0' && kc <= '9') { writeDrDigit (kc - '0'); return true; }
            return false;
        }

        /* FX letter sub-columns: accept A-Z and 0-9. */
        if (cursorSubCol == kColFx1Let || cursorSubCol == kColFx2Let)
        {
            const int up = (kc >= 'a' && kc <= 'z') ? (kc - 32) : kc;
            if ((up >= 'A' && up <= 'Z') || (up >= '0' && up <= '9'))
            { writeFxLetter (up); return true; }
            return false;
        }

        /* FX param hex sub-columns: hex digits. */
        if (cursorSubCol == kColFx1Hi || cursorSubCol == kColFx1Lo
            || cursorSubCol == kColFx2Hi || cursorSubCol == kColFx2Lo)
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

        /* Click in track header.  Explicit MUTE / SOLO buttons take
         * priority over generic header clicks now that they're visible
         * and match the SessionView's pattern.  Channel pill / inst
         * chip stay clickable; right-click still opens the popup. */
        if (y >= 0 && y < kTrackHeaderH)
        {
            const int trk = (x - kRowGutterWidth) / kTrackWidth;
            if (trk >= 0 && trk < s.ntrk)
            {
                const int trkXBase = kRowGutterWidth + trk * kTrackWidth;
                const int pillRight = trkXBase + kTrackWidth - 8;
                const int pillLeft  = trkXBase + kTrackWidth - 36;
                const juce::Point<int> p { x, y };
                const bool inMuteBtn  = muteButtonBounds (trk).contains (p);
                const bool inSoloBtn  = soloButtonBounds (trk).contains (p);
                const bool inChanPill = (x >= pillLeft && x <= pillRight
                                          && y >= 6 && y <= 20);
                const bool inInstChip = (x >= pillLeft && x <= pillRight
                                          && y >= 20 && y <= 32);

                if (e.mods.isRightButtonDown())
                {
                    showHeaderPopup (trk, e);
                }
                else if (inMuteBtn)
                {
                    toggleTrackMute (trk);
                }
                else if (inSoloBtn)
                {
                    soloTrack (trk);
                }
                else if (inInstChip)
                {
                    cursorTrack = trk;
                    cycleCursorTrackProgram (1);
                }
                else if (inChanPill)
                {
                    cursorTrack = trk;
                    cycleCursorTrackChannel (1);
                }
                else if (e.mods.isShiftDown())
                {
                    /* Shift-click on the name strip still solos for
                     * keyboard-driven users used to the prior gesture. */
                    soloTrack (trk);
                }
                /* Plain click on the name strip is now a no-op -- the
                 * explicit MUTE button replaces the old "click anywhere"
                 * shortcut.  Keeps drag-style accidents from muting. */
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
        int delay          = 0;    // 0..99 = delay/100 row offset
        int prob           = 0;    // 0..99 = chance to skip
        int velocity_range = 0;    // 0..127 = negative jitter range on velocity
        int delay_range    = 0;    // 0..99  = symmetric jitter range on delay
        int fx[2] {0, 0};
        int fxParam[2] {0, 0};
    };
    struct TrackS {
        int port = 0, channel = 0, ncols = 1;
        int program = -1;       /* -1 = no PC emitted; 0..127 = inst index */
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
            s.tracks[(size_t) t].program = trk->prog;
            s.tracks[(size_t) t].muted   = (trk->playing == 0);
            s.tracks[(size_t) t].cells.resize ((size_t) seq->length);
            for (int r = 0; r < seq->length; ++r)
            {
                auto& cell = s.tracks[(size_t) t].cells[(size_t) r];
                const auto& row = trk->rows[0][r]; // col 0
                cell.type           = row.type;
                cell.note           = row.note;
                cell.velocity       = row.velocity;
                cell.delay          = row.delay;
                cell.prob           = row.prob;
                cell.velocity_range = row.velocity_range;
                cell.delay_range    = row.delay_range;
                cell.fx[0]          = row.fx[0];
                cell.fx[1]          = row.fx[1];
                cell.fxParam[0]     = row.fxParam[0];
                cell.fxParam[1]     = row.fxParam[1];
            }
        }
    }

    /* Track header zones (per-track, x relative to track left edge).
     *   0..6        tint band
     *   6..20       Track name (left), channel pill + inst chip (right)
     *   20..32      Inst chip (when bound; otherwise reserved space)
     *   32..46      MUTE | SOLO buttons (14 px tall, 2 px outer + center gap)
     *   46..56      Column legend, each label centred under its sub-col
     *
     * The button row mirrors the session-view's per-column MUTE/SOLO
     * affordance so the two views feel the same.  Mute draws with a
     * red-ish fill when active; solo with yellow.  Legend sits just
     * above the cells so each label lines up with the column it
     * names; cells read top-down "label -> data". */
    static constexpr int kHdrButtonRowY = 32;
    static constexpr int kHdrButtonRowH = 14;
    static constexpr int kHdrLegendY    = 46;
    static constexpr int kHdrLegendH    = 10;

    juce::Rectangle<int> muteButtonBounds (int t) const noexcept
    {
        const int x = kRowGutterWidth + t * kTrackWidth;
        const int innerW = kTrackWidth - 4;
        const int w = (innerW - 2) / 2;
        return juce::Rectangle<int> (x + 2, kHdrButtonRowY, w, kHdrButtonRowH);
    }

    juce::Rectangle<int> soloButtonBounds (int t) const noexcept
    {
        const int x = kRowGutterWidth + t * kTrackWidth;
        const int innerW = kTrackWidth - 4;
        const int w = (innerW - 2) / 2;
        return juce::Rectangle<int> (x + 2 + w + 2, kHdrButtonRowY, w, kHdrButtonRowH);
    }

    /* Derived: is track t currently the lone un-muted track?  Matches
     * the toggle semantics in soloTrack(): "this one un-muted, others
     * all muted" reads as solo, otherwise not.  Single-track patterns
     * never show solo (the toggle is a no-op there). */
    static bool isTrackSoloed (const Snapshot& s, int t) noexcept
    {
        if (s.ntrk < 2 || t < 0 || (size_t) t >= s.tracks.size()) return false;
        if (s.tracks[(size_t) t].muted) return false;
        for (int i = 0; i < s.ntrk; ++i)
            if (i != t && (size_t) i < s.tracks.size() && ! s.tracks[(size_t) i].muted)
                return false;
        return true;
    }

    void paintHeader (juce::Graphics& g, const Snapshot& s)
    {
        g.setColour (kGutterColour);
        g.fillRect (0, 0, kRowGutterWidth, kTrackHeaderH);

        // Edit-mode indicator dot in the header corner.
        if (editMode)
        {
            g.setColour (kEditModeColour);
            g.fillEllipse (4.0f, 4.0f, 8.0f, 8.0f);
        }
        // Octave indicator (top-left of gutter, lower band).
        g.setColour (kRowTextColour);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                      kHeaderFontSize - 2.0f, juce::Font::plain));
        g.drawText (juce::String ("o") + juce::String (octave),
                    2, kTrackHeaderH - 14, kRowGutterWidth - 4, 12,
                    juce::Justification::centred);

        for (int t = 0; t < s.ntrk; ++t)
        {
            const int x = kRowGutterWidth + t * kTrackWidth;
            const auto fullTint = trackTint (t);
            const bool muted  = (size_t) t < s.tracks.size() && s.tracks[(size_t) t].muted;
            const bool soloed = isTrackSoloed (s, t);
            const auto tint = muted ? fullTint.withSaturation (0.2f).withBrightness (0.4f)
                                    : fullTint;

            /* Tint band + body wash. */
            g.setColour (tint);
            g.fillRect (x, 0, kTrackWidth - 1, 6);
            g.setColour (tint.withAlpha (0.18f));
            g.fillRect (x, 6, kTrackWidth - 1, kTrackHeaderH - 6);

            /* Track name in the upper strip. */
            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                          kHeaderFontSize, juce::Font::bold));
            g.setColour (tint);
            g.drawText (juce::String::formatted ("Track%02d", t),
                        x + 6, 6, kTrackWidth - 36, 14,
                        juce::Justification::centredLeft);

            /* Channel pill (top-right) -- click to cycle channel. */
            const int chan = (size_t) t < s.tracks.size()
                                ? s.tracks[(size_t) t].channel + 1
                                : t + 1;
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                          kHeaderFontSize - 2.0f, juce::Font::plain));
            g.drawText (juce::String::formatted ("ch%02d", chan),
                        x + kTrackWidth - 36, 6, 28, 14,
                        juce::Justification::centredRight);

            /* Instrument / program chip -- visible only when bound.
             * Sits in the 20..32 band so the legend strip below has its
             * own line. */
            const int prog = (size_t) t < s.tracks.size()
                                ? s.tracks[(size_t) t].program : -1;
            if (prog >= 0)
            {
                g.setColour (juce::Colour { 0xff'8a'd0'4a }.withAlpha (0.85f));
                g.drawText (juce::String::formatted ("i%03d", prog + 1),
                            x + kTrackWidth - 36, 20, 28, 12,
                            juce::Justification::centredRight);
            }

            /* MUTE | SOLO buttons -- match the SessionView pattern. */
            const auto muteR = muteButtonBounds (t);
            const auto soloR = soloButtonBounds (t);
            const juce::Colour btnTint = tint.withMultipliedBrightness (0.55f)
                                             .withSaturation (0.3f);

            g.setColour (muted ? juce::Colour { 0xff'40'30'30 } : btnTint);
            g.fillRect (muteR);
            g.setColour (kRowDividerColour);
            g.drawRect (muteR, 1);
            g.setColour (muted ? juce::Colours::white
                               : juce::Colours::white.withAlpha (0.70f));
            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                          kHeaderFontSize - 2.0f, juce::Font::bold));
            g.drawText ("MUTE", muteR, juce::Justification::centred);

            g.setColour (soloed ? juce::Colour { 0xff'd5'b0'30 } : btnTint);
            g.fillRect (soloR);
            g.setColour (kRowDividerColour);
            g.drawRect (soloR, 1);
            g.setColour (soloed ? juce::Colours::black
                                : juce::Colours::white.withAlpha (0.70f));
            g.drawText ("SOLO", soloR, juce::Justification::centred);

            /* Column legend -- one label per sub-col, positioned at the
             * sub-col's actual X so it reads as a header for the data
             * directly below.  Brighter than the previous "0.45 white"
             * to stand out from the cell text. */
            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                          kHeaderFontSize - 3.0f, juce::Font::bold));
            g.setColour (juce::Colour { 0xff'd0'd0'd0 });
            auto drawLabel = [&] (const char* txt, int colX, int w) {
                g.drawText (txt, x + colX, kHdrLegendY, w, kHdrLegendH,
                            juce::Justification::centredLeft);
            };
            drawLabel ("Nt",  kNoteX,  kNoteW);
            drawLabel ("Vel", kVelX,   kVelW);
            drawLabel ("Dl",  kDelayX, kAuxCellW);
            drawLabel ("Pr",  kProbX,  kAuxCellW);
            drawLabel ("VR",  kVrX,    kAuxCellW);
            drawLabel ("DR",  kDrX,    kAuxCellW);
            drawLabel ("Fx1", kFx1X,   kFxColW);
            drawLabel ("Fx2", kFx2X,   kFxColW);
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
                        case kColNote:    sx = tx + kNoteX;                 sw = kNoteW;     break;
                        case kColVelHi:   sx = tx + kVelX;                  sw = kVelHalfW;  break;
                        case kColVelLo:   sx = tx + kVelX + kVelHalfW;      sw = kVelHalfW;  break;
                        case kColDelayHi: sx = tx + kDelayX;                sw = kAuxCharW;  break;
                        case kColDelayLo: sx = tx + kDelayX + kAuxCharW;    sw = kAuxCharW;  break;
                        case kColProbHi:  sx = tx + kProbX;                 sw = kAuxCharW;  break;
                        case kColProbLo:  sx = tx + kProbX  + kAuxCharW;    sw = kAuxCharW;  break;
                        case kColVrHi:    sx = tx + kVrX;                   sw = kAuxCharW;  break;
                        case kColVrLo:    sx = tx + kVrX    + kAuxCharW;    sw = kAuxCharW;  break;
                        case kColDrHi:    sx = tx + kDrX;                   sw = kAuxCharW;  break;
                        case kColDrLo:    sx = tx + kDrX    + kAuxCharW;    sw = kAuxCharW;  break;
                        case kColFx1Let:  sx = tx + kFx1X;                  sw = kFxCharW;   break;
                        case kColFx1Hi:   sx = tx + kFx1X + kFxCharW;       sw = kFxCharW;   break;
                        case kColFx1Lo:   sx = tx + kFx1X + 2 * kFxCharW;   sw = kFxCharW;   break;
                        case kColFx2Let:  sx = tx + kFx2X;                  sw = kFxCharW;   break;
                        case kColFx2Hi:   sx = tx + kFx2X + kFxCharW;       sw = kFxCharW;   break;
                        case kColFx2Lo:   sx = tx + kFx2X + 2 * kFxCharW;   sw = kFxCharW;   break;
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

    /* x is the track-left edge (tx).  Column groups drawn left to right:
     *   note (kNoteX) | vel (kVelX) | delay (kDelayX) | prob (kProbX)
     *   | vr (kVrX) | dr (kDrX) | FX1 (kFx1X) | FX2 (kFx2X). */
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
            g.setColour (kEmptyTextColour);
            g.drawText ("--", x + kVelX, y, kVelW, h,
                        juce::Justification::centredLeft);
        }
        else
        {
            g.setColour (kEmptyTextColour);
            g.drawText ("---", x + kNoteX, y, kNoteW, h,
                        juce::Justification::centredLeft);
            g.drawText ("--", x + kVelX, y, kVelW, h,
                        juce::Justification::centredLeft);
        }

        /* Aux columns -- dim "--" when zero so the eye picks out only
         * cells that actually carry a modifier.  delay / prob / dr are
         * two-decimal-digit; vr is two-hex matching the velocity column
         * (since it operates on velocity bytes). */
        drawDecCell (g, cell.delay,          x + kDelayX, y, h, kAuxCellW);
        drawDecCell (g, cell.prob,           x + kProbX,  y, h, kAuxCellW);
        drawHexCell (g, cell.velocity_range, x + kVrX,    y, h, kAuxCellW);
        drawDecCell (g, cell.delay_range,    x + kDrX,    y, h, kAuxCellW);

        /* FX columns */
        drawFxCell (g, cell.fx[0], cell.fxParam[0], x + kFx1X, y, h);
        drawFxCell (g, cell.fx[1], cell.fxParam[1], x + kFx2X, y, h);
    }

    void drawDecCell (juce::Graphics& g, int value, int x, int y, int h, int w)
    {
        if (value == 0)
        {
            g.setColour (kEmptyTextColour);
            g.drawText ("--", x, y, w, h, juce::Justification::centredLeft);
            return;
        }
        const int v = juce::jlimit (0, 99, value);
        g.setColour (kRowTextColour);
        g.drawText (juce::String (v).paddedLeft ('0', 2),
                    x, y, w, h, juce::Justification::centredLeft);
    }

    void drawHexCell (juce::Graphics& g, int value, int x, int y, int h, int w)
    {
        if (value == 0)
        {
            g.setColour (kEmptyTextColour);
            g.drawText ("--", x, y, w, h, juce::Justification::centredLeft);
            return;
        }
        const int v = juce::jlimit (0, 255, value);
        g.setColour (kRowTextColour);
        g.drawText (juce::String::toHexString (v).toUpperCase().paddedLeft ('0', 2),
                    x, y, w, h, juce::Justification::centredLeft);
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
            g.setColour (kEmptyTextColour);
            g.drawText (".", x, y, kFxCharW, h, juce::Justification::centred);
            g.drawText (".", x + kFxCharW, y, kFxCharW, h, juce::Justification::centred);
            g.drawText (".", x + 2 * kFxCharW, y, kFxCharW, h, juce::Justification::centred);
        }
    }

    void drawEmptyCell (juce::Graphics& g, int x, int y, int h)
    {
        g.setColour (kEmptyTextColour);
        g.drawText ("---", x + kNoteX,  y, kNoteW,    h, juce::Justification::centredLeft);
        g.drawText ("--",  x + kVelX,   y, kVelW,     h, juce::Justification::centredLeft);
        g.drawText ("--",  x + kDelayX, y, kAuxCellW, h, juce::Justification::centredLeft);
        g.drawText ("--",  x + kProbX,  y, kAuxCellW, h, juce::Justification::centredLeft);
        g.drawText ("--",  x + kVrX,    y, kAuxCellW, h, juce::Justification::centredLeft);
        g.drawText ("--",  x + kDrX,    y, kAuxCellW, h, juce::Justification::centredLeft);
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
    /* track_set_row zeros delay / prob / velocity_range / delay_range
     * (its contract for the recording path); we want note / velocity
     * edits in the editor to preserve those modifiers.  Capture before,
     * restore after.  Fx fields are untouched by track_set_row, so
     * they survive without help. */
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

        auto& dst = trk->rows[0][cursorRow];
        const int prevDelay = dst.delay;
        const int prevProb  = dst.prob;
        const int prevVR    = dst.velocity_range;
        const int prevDR    = dst.delay_range;

        if (midiNote < 0)
        {
            /* Clear: wipe note / vel / delay AND the modifiers + fx --
             * Delete on the note column is the "clear this whole cell"
             * shortcut, distinct from the per-sub-col clears below. */
            track_set_row (trk, 0, cursorRow, 0, 0, 0, 0);
            dst.fx[0] = dst.fx[1] = 0;
            dst.fxParam[0] = dst.fxParam[1] = 0;
        }
        else
        {
            track_set_row (trk, 0, cursorRow, 1, midiNote, 100, prevDelay);
            dst.prob           = prevProb;
            dst.velocity_range = prevVR;
            dst.delay_range    = prevDR;
            dst.velocity_next  = 100;
            dst.delay_next     = prevDelay;
        }

        if (editStep > 0)
        {
            const int next = cursorRow + editStep;
            cursorRow = (next < seq->length) ? next : (next % seq->length);
        }
        ensureCursorVisible();
        repaint();
    }

    /** Write a letter into the FX-letter sub-column (kColFx1Let /
     *  kColFx2Let).  Sets fx[slot] = ASCII letter; clears via 0. After
     *  write, advance cursor to the FX-hi sub-column of the same slot. */
    void writeFxLetter (int letter)
    {
        if (trackerNode == nullptr) return;
        if (cursorSubCol != kColFx1Let && cursorSubCol != kColFx2Let) return;
        const int slot = (cursorSubCol == kColFx1Let) ? 0 : 1;
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
     *  cursorSubCol kColFx1Hi/kColFx2Hi = high nybble; +Lo variants = low.
     *  After low-nybble write, advance to next row. */
    void writeFxNybble (int nybble)
    {
        if (trackerNode == nullptr) return;
        const bool inFx1 = (cursorSubCol == kColFx1Hi || cursorSubCol == kColFx1Lo);
        const bool inFx2 = (cursorSubCol == kColFx2Hi || cursorSubCol == kColFx2Lo);
        if (! inFx1 && ! inFx2) return;
        const int slot = inFx1 ? 0 : 1;
        const bool isHi = (cursorSubCol == kColFx1Hi || cursorSubCol == kColFx2Hi);
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
            ++cursorSubCol; // hi -> lo
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
        if (cursorSubCol != kColVelHi && cursorSubCol != kColVelLo) return;
        trackerNode->pushUndo();

        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;
        if (cursorTrack < 0 || cursorTrack >= seq->ntrk) return;
        auto* trk = seq->trk[cursorTrack];
        if (! trk || cursorRow < 0 || cursorRow >= seq->length) return;

        /* Velocity edit only applies if there's a note here. */
        auto& cur = trk->rows[0][cursorRow];
        if (cur.type != 1) return; // not a note_on; ignore

        int vel = cur.velocity;
        if (cursorSubCol == kColVelHi)
            vel = ((nybble & 0x0f) << 4) | (vel & 0x0f);
        else
            vel = (vel & 0xf0) | (nybble & 0x0f);
        vel = juce::jlimit (0, 127, vel);

        /* Preserve delay / prob / vr / dr around the track_set_row call. */
        const int prevDelay = cur.delay;
        const int prevProb  = cur.prob;
        const int prevVR    = cur.velocity_range;
        const int prevDR    = cur.delay_range;
        track_set_row (trk, 0, cursorRow, cur.type, cur.note, vel, prevDelay);
        cur.prob           = prevProb;
        cur.velocity_range = prevVR;
        cur.delay_range    = prevDR;
        cur.velocity_next  = vel;
        cur.delay_next     = prevDelay;

        if (cursorSubCol == kColVelHi)
        {
            cursorSubCol = kColVelLo;
        }
        else
        {
            cursorSubCol = kColNote;
            if (editStep > 0)
            {
                const int next = cursorRow + editStep;
                cursorRow = (next < seq->length) ? next : (next % seq->length);
            }
            ensureCursorVisible();
        }
        repaint();
    }

    /* === Aux column writers (delay / prob / velocity_range / delay_range)
     *
     * Each writer:
     *   - locates the row + field under engineLock_ (no pointer reads
     *     outside the lock; the lookup is atomic with the write);
     *   - mutates the engine field directly (no track_set_row -- we
     *     don't want its zeroing of the OTHER aux fields);
     *   - keeps velocity_next / delay_next in sync so the randomiser
     *     reads a consistent starting point (row_randomise on next
     *     trigger will re-roll them within the new range);
     *   - bumps dirty so the thumbnail picks up the change;
     *   - advances cursor on low-digit input the same way the other
     *     columns do. */
    enum class AuxField { Delay, Prob, VelRange, DelayRange };

    void writeAuxDigit (AuxField which, int nybble, bool isHi)
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

        auto& cur = trk->rows[0][cursorRow];

        int* field        = nullptr;
        int maxValue      = 0;
        bool decimal      = true;
        int hiSubCol = 0, loSubCol = 0;
        switch (which)
        {
            case AuxField::Delay:
                field = &cur.delay;          maxValue = 99;
                hiSubCol = kColDelayHi;      loSubCol = kColDelayLo;
                decimal = true;
                break;
            case AuxField::Prob:
                field = &cur.prob;           maxValue = 99;
                hiSubCol = kColProbHi;       loSubCol = kColProbLo;
                decimal = true;
                break;
            case AuxField::VelRange:
                field = &cur.velocity_range; maxValue = 127;
                hiSubCol = kColVrHi;         loSubCol = kColVrLo;
                decimal = false;
                break;
            case AuxField::DelayRange:
                field = &cur.delay_range;    maxValue = 99;
                hiSubCol = kColDrHi;         loSubCol = kColDrLo;
                decimal = true;
                break;
        }
        if (field == nullptr) return;

        const int base = decimal ? 10 : 16;
        int v  = juce::jlimit (0, maxValue, *field);
        const int hi = decimal ? (v / 10) : ((v >> 4) & 0x0f);
        const int lo = decimal ? (v % 10) : (v & 0x0f);
        if (isHi)
            v = nybble * base + lo;
        else
            v = hi * base + nybble;
        *field = juce::jlimit (0, maxValue, v);

        cur.velocity_next = cur.velocity;
        cur.delay_next    = cur.delay;
        trk->dirty = 1;

        if (isHi)
        {
            cursorSubCol = loSubCol;
        }
        else
        {
            cursorSubCol = hiSubCol;
            if (editStep > 0)
            {
                const int next = cursorRow + editStep;
                cursorRow = (next < seq->length) ? next : (next % seq->length);
            }
            ensureCursorVisible();
        }
        repaint();
    }

    void writeDelayDigit (int nybble)
    {
        if (cursorSubCol != kColDelayHi && cursorSubCol != kColDelayLo) return;
        writeAuxDigit (AuxField::Delay, nybble, cursorSubCol == kColDelayHi);
    }
    void writeProbDigit (int nybble)
    {
        if (cursorSubCol != kColProbHi && cursorSubCol != kColProbLo) return;
        writeAuxDigit (AuxField::Prob, nybble, cursorSubCol == kColProbHi);
    }
    void writeVrNybble (int nybble)
    {
        if (cursorSubCol != kColVrHi && cursorSubCol != kColVrLo) return;
        writeAuxDigit (AuxField::VelRange, nybble, cursorSubCol == kColVrHi);
    }
    void writeDrDigit (int nybble)
    {
        if (cursorSubCol != kColDrHi && cursorSubCol != kColDrLo) return;
        writeAuxDigit (AuxField::DelayRange, nybble, cursorSubCol == kColDrHi);
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

    int getRpb() const
    {
        if (trackerNode == nullptr) return 4;
        juce::ScopedLock sl (const_cast<juce::CriticalSection&> (trackerNode->engineLock()));
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return 4;
        return mod->curr_seq->rpb;
    }

    void changeRpb (int delta)
    {
        if (trackerNode == nullptr || delta == 0) return;
        trackerNode->pushUndo();
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;
        const int newRpb = juce::jlimit (1, 32, seq->rpb + delta);
        if (newRpb == seq->rpb) return;
        seq->rpb = newRpb;
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
        /* Default new tracks to MIDI ch 1 (engine ch index 0) so they
         * play through whatever the downstream sampler / synth has
         * bound to ch 1 -- single-instrument users get notes out of
         * the box.  Multi-channel routing is opt-in via the channel
         * pill (click to cycle 1..16). */
        track* trk = track_new (0 /*port*/, 0 /*ch1*/,
                                seq->length, seq->length, TRACK_DEF_CTRLPR);
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
                dst.type           = src.type;
                dst.note           = src.note;
                dst.velocity       = src.velocity;
                dst.delay          = src.delay;
                dst.prob           = src.prob;
                dst.velocity_range = src.velocity_range;
                dst.delay_range    = src.delay_range;
                dst.fx[0]          = src.fx[0];
                dst.fx[1]          = src.fx[1];
                dst.fxParam[0]     = src.fxParam[0];
                dst.fxParam[1]     = src.fxParam[1];
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
                /* track_set_row zeros type/note/vel/delay + the aux
                 * fields; FX fields it leaves alone, so we have to
                 * wipe them here for a full cell clear. */
                track_set_row (trk, 0, r, 0, 0, 0, 0);
                trk->rows[0][r].fx[0] = trk->rows[0][r].fx[1] = 0;
                trk->rows[0][r].fxParam[0] = trk->rows[0][r].fxParam[1] = 0;
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
                trk->rows[0][r].fx[0] = trk->rows[0][r].fx[1] = 0;
                trk->rows[0][r].fxParam[0] = trk->rows[0][r].fxParam[1] = 0;
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
                auto& dst = trk->rows[0][targetRow];
                dst.prob           = src.prob;
                dst.velocity_range = src.velocity_range;
                dst.delay_range    = src.delay_range;
                dst.velocity_next  = src.velocity;
                dst.delay_next     = src.delay;
                dst.fx[0]          = src.fx[0];
                dst.fx[1]          = src.fx[1];
                dst.fxParam[0]     = src.fxParam[0];
                dst.fxParam[1]     = src.fxParam[1];
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

    /** Cycle the focused track's program (vht prog field).  Engine sends
     *  a MIDI PC at row 0 + on change via track_fix_program_change.
     *  -1 = off, 0..127 = instrument index. */
    void cycleCursorTrackProgram (int delta = 1)
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
        int p = trk->prog + delta;
        if (p < -1)   p = 127;
        if (p > 127)  p = -1;
        trk->prog = p;
        trk->prog_send = 1;     /* engine emits PC at next row trigger */
        trk->prog_sent = 0;
        repaint();
    }

    /** Right-click on a track header — popup menu for destination /
     *  program / mute actions. */
    void showHeaderPopup (int trk, const juce::MouseEvent& e)
    {
        if (trackerNode == nullptr) return;
        if (trk < 0) return;

        cursorTrack = trk;

        /* Snapshot the track's loop state for the menu checkmark.
         * Lookup under engineLock so a concurrent pattern switch can't
         * leave us holding a stale pointer. */
        bool trackLoops = true;
        {
            juce::ScopedLock sl (trackerNode->engineLock());
            auto* mod = trackerNode->modulePtr();
            if (mod != nullptr && mod->curr_seq != nullptr
                && trk >= 0 && trk < mod->curr_seq->ntrk)
            {
                if (auto* t = mod->curr_seq->trk[trk])
                    trackLoops = (t->loop != 0);
            }
        }

        juce::PopupMenu m;
        m.addSectionHeader ("Track " + juce::String (trk + 1));
        m.addItem (1, "Mute / unmute");
        m.addItem (2, "Solo");
        m.addSeparator();
        m.addItem (8, "Loop (one-shot when off)", /*enabled*/ true,
                   /*checked*/ trackLoops);
        m.addSeparator();
        m.addItem (3, "Inst +1");
        m.addItem (4, "Inst -1");
        m.addItem (5, "Inst off");
        m.addSeparator();
        juce::PopupMenu progSub;
        for (int i = 1; i <= 16; ++i)
            progSub.addItem (100 + i, "Instrument " + juce::String (i));
        m.addSubMenu ("Set instrument...", progSub);
        m.addSeparator();
        m.addItem (6, "Channel +1");
        m.addItem (7, "Channel -1");

        /* Anchor the popup at the actual click position (screen coords)
         * rather than at the component edge.  withTargetComponent on
         * its own picks a default placement (bottom-left of component)
         * which surfaced the menu way below the tracker grid. */
        const juce::Rectangle<int> targetArea (e.getScreenX(), e.getScreenY(), 1, 1);
        m.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetComponent (this)
                            .withTargetScreenArea (targetArea),
            [this, trk] (int sel) {
                switch (sel)
                {
                    case 0:  return;
                    case 1:  toggleTrackMute (trk); break;
                    case 2:  soloTrack (trk); break;
                    case 3:  cursorTrack = trk; cycleCursorTrackProgram ( 1); break;
                    case 4:  cursorTrack = trk; cycleCursorTrackProgram (-1); break;
                    case 5:  setTrackProgram (trk, -1); break;
                    case 6:  cursorTrack = trk; cycleCursorTrackChannel ( 1); break;
                    case 7:  cursorTrack = trk; cycleCursorTrackChannel (-1); break;
                    case 8:  toggleTrackLoop (trk); break;
                    default:
                        if (sel >= 101 && sel <= 116)
                            setTrackProgram (trk, sel - 101);
                        break;
                }
            });
    }

    void toggleTrackLoop (int t)
    {
        if (trackerNode == nullptr) return;
        trackerNode->pushUndo();
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        if (t < 0 || t >= mod->curr_seq->ntrk) return;
        auto* trk = mod->curr_seq->trk[t];
        if (! trk) return;
        trk->loop = trk->loop ? 0 : 1;
        repaint();
    }

    void setTrackProgram (int t, int prog)
    {
        if (trackerNode == nullptr) return;
        trackerNode->pushUndo();
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        if (t < 0 || t >= mod->curr_seq->ntrk) return;
        auto* trk = mod->curr_seq->trk[t];
        if (! trk) return;
        trk->prog = juce::jlimit (-1, 127, prog);
        trk->prog_send = 1;
        trk->prog_sent = 0;
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
                "  STEP         rows the cursor jumps after entering a note",
                "               (only applies while EDIT mode is on)",
                "  Delete       clear cell (note col) / zero digit (other cols)",
                "  0-9 A-F      hex digit (Vel / VR / FX param sub-cols)",
                "  0-9          decimal digit (Delay / Prob / DR sub-cols)",
                "  A-Z / 0-9    FX letter (FX1-let / FX2-let sub-cols)",
                "  Insert       insert row at cursor (current track)",
                "  Shift+Insert delete row at cursor",
                "",
                "NAVIGATION",
                "  Up / Down    move cursor row",
                "  Left / Right step sub-column (Note / Vel / Dl / Pr",
                "                  / VR / DR / FX1 / FX2; crosses",
                "                  tracks at boundaries)",
                "  Tab / S-Tab  jump to next / previous track",
                "  PgUp / PgDn  jump 16 rows",
                "  Home / End   first / last row",
                "  Click        jump cursor to cell",
                "  Click MUTE / SOLO   toggle on the track header",
                "  Click channel pill  cycle MIDI channel 1..16",
                "                      (right-click reverses)",
                "  Right-click header  full menu (mute/solo/loop/inst...)",
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
        int type = 0; int note = 0; int velocity = 0;
        int delay          = 0;
        int prob           = 0;
        int velocity_range = 0;
        int delay_range    = 0;
        int fx[2] {0, 0};
        int fxParam[2] {0, 0};
    };
    /* Clipboard contents: indexed by [rowOffset][trackOffset]. */
    std::vector<std::vector<ClipCell>> clipboard;

    TrackerNode* trackerNode;
    int cursorRow    = 0;
    int cursorTrack  = 0;
    int cursorSubCol = 0; // see kColNote / kColVelHi / ... at file top
    int octave       = 4;
    int editStep     = 1; // auto-advance amount after a write (0 = no advance)
    bool editMode    = false;
    bool showHelp    = false;
    bool followPlayhead = true; // scroll viewport with the playhead during playback

    /* UI-tick diff state — see tickRepaint().  Sentinel values force an
     * initial paint on the first tick. */
    int  lastPlayheadRow_  = -2;
    int  lastPatternIndex_ = -1;
    int  lastNtrk_         = -1;
    int  lastRows_         = -1;
    bool lastPlaying_      = false;
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
        configureButton (rpbMinusBtn,  "-", [this]{ editor.changeRpb (-1); });
        configureButton (rpbPlusBtn,   "+", [this]{ editor.changeRpb ( 1); });
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

        /* Static "name" labels — always show the setting name only. */
        configureLabel (octLabel);  octLabel .setText ("OCT",  juce::dontSendNotification);
        configureLabel (stepLabel); stepLabel.setText ("STEP", juce::dontSendNotification);
        configureLabel (lenLabel);  lenLabel .setText ("LEN",  juce::dontSendNotification);
        configureLabel (trkLabel);  trkLabel .setText ("TRK",  juce::dontSendNotification);
        configureLabel (patLabel);  patLabel .setText ("PAT",  juce::dontSendNotification);
        configureLabel (rpbLabel);  rpbLabel .setText ("RPB",  juce::dontSendNotification);
        configureLabel (bpmLabel);

        /* Editable value labels — double-click to type a target value
         * directly (so e.g. LEN 64 doesn't need 48 nudge clicks).
         * Apply on commit by delta'ing from the current value through
         * the existing change* path. */
        auto setupEditable = [this] (juce::Label& l, std::function<void (int)> commit) {
            l.setJustificationType (juce::Justification::centred);
            l.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                          12.0f, juce::Font::bold));
            l.setColour (juce::Label::textColourId,           juce::Colour { 0xff'ff'ff'ff });
            l.setColour (juce::Label::backgroundColourId,     juce::Colour { 0xff'18'18'18 });
            l.setColour (juce::Label::backgroundWhenEditingColourId, juce::Colour { 0xff'30'30'30 });
            l.setColour (juce::Label::outlineColourId,        juce::Colour { 0xff'40'40'40 });
            l.setEditable (false, /*onDoubleClick*/ true, /*lossOfFocusDiscards*/ false);
            l.onTextChange = [&l, commit = std::move (commit)] {
                /* Strip brackets / non-digits, parse, hand to caller. */
                const auto txt = l.getText().retainCharacters ("0123456789-").trim();
                if (txt.isNotEmpty()) commit (txt.getIntValue());
            };
            addAndMakeVisible (l);
        };
        setupEditable (octValue,  [this] (int v) { editor.changeOctave        (v - editor.getOctave()); });
        setupEditable (stepValue, [this] (int v) { editor.changeEditStep      (v - editor.getEditStep()); });
        setupEditable (lenValue,  [this] (int v) { editor.changePatternLength (v - editor.getPatternLength()); });
        setupEditable (rpbValue,  [this] (int v) { editor.changeRpb           (v - editor.getRpb()); });
        /* TRK + PAT also editable.  Per-step mutations are slightly
         * heavier (add/remove tracks; switch pattern jumps) but the
         * editable surface is the same UX win as direct LEN edit. */
        setupEditable (trkValue,  [this] (int v) {
            const int target = juce::jlimit (1, 32, v);
            int cur = editor.getTrackCount();
            while (cur < target) { editor.addTrack();            ++cur; }
            while (cur > target) { editor.deleteCurrentTrack();  --cur; }
        });
        setupEditable (patValue,  [this] (int v) {
            const int patCount = juce::jmax (1, editor.getPatternCount());
            const int target   = juce::jlimit (1, patCount, v);   /* user types 1-based */
            const int cur      = editor.getPatternIndex() + 1;
            editor.switchPattern (target - cur);
        });

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

        /* Name labels stay static; only value labels need refresh.
         * Skip pushing into a label that's currently being edited (the
         * focused editor would otherwise be reset mid-keystroke). */
        auto pushValue = [] (juce::Label& l, const juce::String& s) {
            if (! l.isBeingEdited())
                l.setText (s, juce::dontSendNotification);
        };
        pushValue (octValue,  juce::String (editor.getOctave()));
        pushValue (stepValue, juce::String (editor.getEditStep()));
        pushValue (lenValue,  juce::String (editor.getPatternLength()));
        pushValue (rpbValue,  juce::String (editor.getRpb()));
        pushValue (trkValue,  juce::String (editor.getTrackCount()));
        pushValue (patValue,  juce::String::formatted ("%d/%d",
                                                       editor.getPatternIndex() + 1,
                                                       juce::jmax (1, editor.getPatternCount())));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour { 0xff'24'24'24 });
        g.setColour (juce::Colour { 0xff'35'35'35 });
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    }

    void resized() override
    {
        /* Two-row toolbar.
         *   Row 1 = "current state" controls (mode, octave / step / len
         *           / trk, FOLLOW playback, help).
         *   Row 2 = pattern + history (RPB, PAT picker + NEW/DUP/DEL,
         *           UND/RED).
         *
         * Pattern-management used to live on row 1 alongside everything
         * else, which truncated NEW/DUP and UND/RED to "N..." / "D..."
         * / "U..." / "R...".  Moving them down lets us use full labels
         * and keeps row 1 from overflowing in narrower windows. */
        const int btnW    = 20;     /* tight nudge buttons */
        const int nameW   = 32;     /* "OCT" / "STEP" / etc. */
        const int valW    = 42;     /* "[16]" / "[1/1]" */
        const int wideW   = 48;
        const int patBtnW = 44;     /* fits "NEW" / "DUP" / "DEL" */
        const int hisBtnW = 40;     /* fits "UND" / "RED" */
        const int rowH    = (getHeight() - 8) / 2;
        const int yRow1   = 2;
        const int yRow2   = 2 + rowH + 2;

        struct Row {
            int x;
            int y;
            int h;
        };
        Row row { 6, yRow1, rowH };

        auto place = [&] (juce::Component& c, int w) {
            c.setBounds (row.x, row.y, w, row.h);
            row.x += w + 2;
        };
        auto sep = [&] { row.x += 8; };

        auto group = [&] (juce::Component& name, juce::Component& value,
                          juce::Component& minus, juce::Component& plus,
                          int valWidth) {
            place (name, nameW);
            place (value, valWidth);
            place (minus, btnW);
            place (plus,  btnW);
            sep();
        };

        /* --- Row 1: current-state controls --- */
        place (editBtn, wideW); sep();
        group (octLabel,  octValue,  octMinusBtn,   octPlusBtn,    valW);
        group (stepLabel, stepValue, stepMinusBtn,  stepPlusBtn,   valW);
        group (lenLabel,  lenValue,  lenMinusBtn,   lenPlusBtn,    valW);
        group (trkLabel,  trkValue,  trkRemoveBtn,  trkAddBtn,     valW);
        place (followBtn, 56); sep();
        place (helpBtn, btnW);

        /* bpmLabel is reserved but never populated in the tracker
         * context (Element's main transport owns the BPM display).
         * Park it offscreen so it doesn't reserve a phantom gap. */
        bpmLabel.setBounds (-100, -100, 1, 1);

        /* --- Row 2: pattern picker + ops + history --- */
        row = Row { 6, yRow2, rowH };
        group (rpbLabel,  rpbValue,  rpbMinusBtn,   rpbPlusBtn,    valW);
        group (patLabel,  patValue,  patPrevBtn,    patNextBtn,    52);
        place (patNewBtn, patBtnW);
        place (patDupBtn, patBtnW);
        place (patDelBtn, patBtnW);
        sep();
        place (undoBtn, hisBtnW);
        place (redoBtn, hisBtnW);
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
    juce::TextButton rpbMinusBtn,   rpbPlusBtn;
    juce::TextButton trkRemoveBtn,  trkAddBtn;
    juce::TextButton patPrevBtn,    patNextBtn, patNewBtn, patDupBtn, patDelBtn;
    juce::TextButton followBtn;
    juce::TextButton undoBtn, redoBtn;
    juce::Label octLabel, stepLabel, lenLabel, rpbLabel, trkLabel, patLabel, bpmLabel;
    /* Editable value displays (boxed "[N]" beside the name label).
     * Double-click to type a target value directly. */
    juce::Label octValue, stepValue, lenValue, rpbValue, trkValue, patValue;
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
    /* Two-row toolbar; default body fits ~24 rows x 4 tracks. */
    constexpr int kToolbarH = 68;
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
    /* Two-row toolbar: row 1 = transport / mode / pattern controls,
     * row 2 = per-sequence / per-pattern fine config (currently
     * RPB, reserved for trigger / loop / extras as they land). */
    constexpr int kToolbarH = 68;
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
    /* Pattern grid: cheap diff-driven repaint — full only on structural
     * change, partial only on playhead-row move.  Idle frames cost
     * one lock + a handful of integer compares. */
    if (patternView)
        patternView->tickRepaint();

    /* Toolbar: only refresh when transport / pattern state actually
     * differs from last frame, so the 30Hz tick doesn't keep rebuilding
     * label strings + repainting buttons on idle. */
    int   newPatternIndex  = patternView ? patternView->getPatternIndex() : 0;
    int   newPatternCount  = patternView ? patternView->getPatternCount() : 1;
    float newBpm           = patternView ? patternView->getBPM() : 120.f;
    bool  newEditMode      = patternView ? patternView->getEditMode() : false;
    int   newOctave        = patternView ? patternView->getOctave() : 4;
    int   newEditStep      = patternView ? patternView->getEditStep() : 1;
    int   newPatternLength = patternView ? patternView->getPatternLength() : 0;
    int   newRpb           = patternView ? patternView->getRpb() : 4;
    int   newTrackCount    = patternView ? patternView->getTrackCount() : 0;
    bool  newFollow        = patternView ? patternView->getFollowPlayhead() : false;
    bool  newCanUndo       = canUndo();
    bool  newCanRedo       = canRedo();

    if (newPatternIndex  != lastToolbarPatternIndex_
        || newPatternCount  != lastToolbarPatternCount_
        || std::abs (newBpm - lastToolbarBpm_) > 0.01f
        || newEditMode      != lastToolbarEditMode_
        || newOctave        != lastToolbarOctave_
        || newEditStep      != lastToolbarEditStep_
        || newPatternLength != lastToolbarPatternLength_
        || newRpb           != lastToolbarRpb_
        || newTrackCount    != lastToolbarTrackCount_
        || newFollow        != lastToolbarFollow_
        || newCanUndo       != lastToolbarCanUndo_
        || newCanRedo       != lastToolbarCanRedo_)
    {
        refreshToolbar();
        lastToolbarPatternIndex_  = newPatternIndex;
        lastToolbarPatternCount_  = newPatternCount;
        lastToolbarBpm_           = newBpm;
        lastToolbarEditMode_      = newEditMode;
        lastToolbarOctave_        = newOctave;
        lastToolbarEditStep_      = newEditStep;
        lastToolbarPatternLength_ = newPatternLength;
        lastToolbarRpb_           = newRpb;
        lastToolbarTrackCount_    = newTrackCount;
        lastToolbarFollow_        = newFollow;
        lastToolbarCanUndo_       = newCanUndo;
        lastToolbarCanRedo_       = newCanRedo;
    }
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
int   TrackerEditor::getRpb() const          { return patternView ? patternView->getRpb() : 4; }
void  TrackerEditor::changeRpb (int d)       { if (patternView) patternView->changeRpb (d); }
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
