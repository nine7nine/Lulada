// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/trackereditor.hpp"
#include "nodes/tracker.hpp"

namespace element {

namespace {

/* Layout constants — Renoise-ish. Tunable later. */
constexpr int kRowHeight        = 18;
constexpr int kRowGutterWidth   = 40;
constexpr int kTrackWidth       = 120;
constexpr int kTrackHeaderH     = 30;
constexpr int kColumnSubWidth   = 32;
constexpr float kCellFontSize   = 13.0f;
constexpr float kHeaderFontSize = 12.0f;

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
            if (kc == 'T' || kc == 't') { addTrack();            return true; }
            if (kc == 'W' || kc == 'w') { deleteCurrentTrack();  return true; }
        }

        // ESC toggles edit mode
        if (kc == juce::KeyPress::escapeKey)
        {
            editMode = ! editMode;
            repaint();
            return true;
        }

        // Navigation works regardless of edit mode.
        if (kc == juce::KeyPress::upKey)         { moveCursor (-1, 0);  return true; }
        if (kc == juce::KeyPress::downKey)       { moveCursor ( 1, 0);  return true; }
        if (kc == juce::KeyPress::leftKey)       { moveCursor ( 0, -1); return true; }
        if (kc == juce::KeyPress::rightKey)      { moveCursor ( 0,  1); return true; }
        if (kc == juce::KeyPress::pageUpKey)     { moveCursor (-16, 0); return true; }
        if (kc == juce::KeyPress::pageDownKey)   { moveCursor ( 16, 0); return true; }
        if (kc == juce::KeyPress::homeKey)       { setCursorAbs (0, cursorTrack); return true; }
        if (kc == juce::KeyPress::endKey)        { setCursorAbs (INT_MAX, cursorTrack); return true; }

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

        // Tab cycles cursor sub-column: note → vel-hi → vel-lo → note.
        if (kc == juce::KeyPress::tabKey)
        {
            if (shift) cursorSubCol = (cursorSubCol + 2) % 3;
            else       cursorSubCol = (cursorSubCol + 1) % 3;
            repaint();
            return true;
        }

        if (! editMode) return false;

        // Delete / backspace: clear cell (note column only).
        if (kc == juce::KeyPress::deleteKey || kc == juce::KeyPress::backspaceKey)
        {
            if (cursorSubCol == 0) writeCell (-1);
            else                   writeVelocityNybble (0); // zero the nybble
            return true;
        }

        if (cursorSubCol > 0)
        {
            // Velocity sub-column: accept hex digits.
            int nybble = -1;
            if      (kc >= '0' && kc <= '9') nybble = kc - '0';
            else if (kc >= 'A' && kc <= 'F') nybble = 10 + (kc - 'A');
            else if (kc >= 'a' && kc <= 'f') nybble = 10 + (kc - 'a');

            if (nybble >= 0)
            {
                writeVelocityNybble (nybble);
                return true;
            }
            return false; // don't treat hex letters as note input
        }

        /* Note input.  JUCE on X11 returns the raw keysym for letter
         * keys — `z` unshifted comes through as ASCII 122, not 90. Fold
         * to uppercase so qwertyToSemitone's `case 'Z':` matches either. */
        const int kcUpper = (kc >= 'a' && kc <= 'z') ? (kc - 32) : kc;
        const int semis = qwertyToSemitone (kcUpper);
        if (semis >= 0)
        {
            const int midiNote = juce::jlimit (0, 127, (octave + 1) * 12 + semis);
            writeCell (midiNote);
            return true;
        }

        return false;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();

        // Click in a cell -> jump cursor there.
        Snapshot s;
        snapshot (s);

        const int x = e.x;
        const int y = e.y;
        if (y < kTrackHeaderH || x < kRowGutterWidth) return;

        const int row = (y - kTrackHeaderH) / kRowHeight;
        const int trk = (x - kRowGutterWidth) / kTrackWidth;

        if (row < 0 || row >= s.rows || trk < 0 || trk >= s.ntrk) return;
        cursorRow = row;
        cursorTrack = trk;
        repaint();
    }

private:
    struct Cell { int type = 0; int note = 0; int velocity = 0; };
    struct TrackS { int port = 0, channel = 0, ncols = 1; std::vector<Cell> cells; };
    struct Snapshot {
        int rows = 16, ntrk = 1, rpb = 4;
        int playheadRow = -1;   // -1 = not playing
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

        s.tracks.resize ((size_t) seq->ntrk);
        for (int t = 0; t < seq->ntrk; ++t)
        {
            auto* trk = seq->trk[t];
            if (! trk) continue;
            s.tracks[(size_t) t].port    = trk->port;
            s.tracks[(size_t) t].channel = trk->channel;
            s.tracks[(size_t) t].ncols   = trk->ncols;
            s.tracks[(size_t) t].cells.resize ((size_t) seq->length);
            for (int r = 0; r < seq->length; ++r)
            {
                auto& cell = s.tracks[(size_t) t].cells[(size_t) r];
                const auto& row = trk->rows[0][r]; // col 0
                cell.type     = row.type;
                cell.note     = row.note;
                cell.velocity = row.velocity;
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
            const auto tint = trackTint (t);

            g.setColour (tint);
            g.fillRect (x, 0, kTrackWidth - 1, 6);

            g.setColour (tint.withAlpha (0.18f));
            g.fillRect (x, 6, kTrackWidth - 1, kTrackHeaderH - 6);

            g.setColour (tint);
            g.drawText (juce::String::formatted ("Track%02d", t),
                        x + 6, 8, kTrackWidth - 12, 16,
                        juce::Justification::centredLeft);

            g.setColour (juce::Colours::white.withAlpha (0.45f));
            g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                          kHeaderFontSize - 2.0f, juce::Font::plain));
            g.drawText ("Note  Vel",
                        x + 6, kTrackHeaderH - 14, kTrackWidth - 12, 12,
                        juce::Justification::centredLeft);
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
                if (r == cursorRow && t == cursorTrack)
                {
                    g.setColour (kCursorHighlight);
                    /* Sub-column-aware highlight:
                     *   0 = note (left half),
                     *   1 = vel-hi nybble,
                     *   2 = vel-lo nybble */
                    const int noteX = tx + 4;
                    const int noteW = kColumnSubWidth + 8;
                    const int velX  = tx + 8 + kColumnSubWidth + 4;
                    const int velNybbleW = kColumnSubWidth / 2;
                    if (cursorSubCol == 0)
                        g.fillRect (noteX, y, noteW, kRowHeight);
                    else if (cursorSubCol == 1)
                        g.fillRect (velX, y, velNybbleW, kRowHeight);
                    else
                        g.fillRect (velX + velNybbleW, y, velNybbleW, kRowHeight);
                }

                g.setColour (kRowDividerColour);
                g.fillRect (tx + kTrackWidth - 1, y, 1, kRowHeight);

                if (! trk.cells.empty() && r < (int) trk.cells.size())
                {
                    drawCell (g, trk.cells[(size_t) r], tx + 8, y, kRowHeight);
                }
                else
                {
                    drawEmptyCell (g, tx + 8, y, kRowHeight);
                }
            }
        }
    }

    void drawCell (juce::Graphics& g, const Cell& cell, int x, int y, int h)
    {
        if (cell.type == 1)
        {
            g.setColour (kNoteTextColour);
            g.drawText (formatNote (cell.note),
                        x, y, kColumnSubWidth, h,
                        juce::Justification::centredLeft);
            g.setColour (kVelTextColour);
            g.drawText (juce::String::toHexString (cell.velocity).toUpperCase().paddedLeft ('0', 2),
                        x + kColumnSubWidth + 8, y, kColumnSubWidth, h,
                        juce::Justification::centredLeft);
        }
        else if (cell.type == 2)
        {
            g.setColour (kEmptyCellColour.brighter (0.4f));
            g.drawText ("OFF", x, y, kColumnSubWidth, h,
                        juce::Justification::centredLeft);
        }
        else
        {
            drawEmptyCell (g, x, y, h);
        }
    }

    void drawEmptyCell (juce::Graphics& g, int x, int y, int h)
    {
        g.setColour (kEmptyCellColour);
        g.drawText ("---", x, y, kColumnSubWidth, h,
                    juce::Justification::centredLeft);
        g.drawText ("--", x + kColumnSubWidth + 8, y, kColumnSubWidth, h,
                    juce::Justification::centredLeft);
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

        const int next = cursorRow + 1;
        cursorRow = next < seq->length ? next : 0;
        ensureCursorVisible();
        repaint();
    }

    /** Set one hex nybble of the cell's velocity. cursorSubCol == 1
     *  writes the high nybble; == 2 writes the low nybble. After the
     *  low nybble, advance to next row + return cursor to note column. */
    void writeVelocityNybble (int nybble)
    {
        if (trackerNode == nullptr) return;
        if (cursorSubCol < 1 || cursorSubCol > 2) return;

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
            const int next = cursorRow + 1;
            cursorRow = next < seq->length ? next : 0;
            ensureCursorVisible();
        }
        repaint();
    }

    void changePatternLength (int delta)
    {
        if (trackerNode == nullptr) return;
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
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;
        if (seq->ntrk >= 16) return; // soft cap; engine has no hard limit
        const int newCh = seq->ntrk; // simplest scheme — channel == track idx
        track* trk = track_new (0 /*port*/, newCh, seq->length, seq->length, TRACK_DEF_CTRLPR);
        sequence_add_track (seq, trk);
        cursorTrack = seq->ntrk - 1;
        repaint();
    }

    void deleteCurrentTrack()
    {
        if (trackerNode == nullptr) return;
        juce::ScopedLock sl (trackerNode->engineLock());
        auto* mod = trackerNode->modulePtr();
        if (mod == nullptr || mod->curr_seq == nullptr) return;
        auto* seq = mod->curr_seq;
        if (seq->ntrk <= 1) return; // keep at least one track
        if (cursorTrack < 0 || cursorTrack >= seq->ntrk) return;
        sequence_del_track (seq, cursorTrack);
        if (cursorTrack >= seq->ntrk) cursorTrack = seq->ntrk - 1;
        repaint();
    }

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
                "  Tab / S-Tab  cycle sub-column (note / vel-hi / vel-lo)",
                "  0-9 A-F      set velocity hex digit (in vel sub-cols)",
                "",
                "NAVIGATION",
                "  Arrows       move cursor",
                "  PgUp / PgDn  jump 16 rows",
                "  Home / End   first / last row",
                "  Click        jump cursor to cell",
                "",
                "PATTERN",
                "  Ctrl+Up/Dn   pattern length -/+ 1",
                "  Ctrl+Shift+Up/Dn   length -/+ 16",
                "  Ctrl+T       add track",
                "  Ctrl+W       remove cursor track (keeps last)",
                "",
                "  F1 / ?       toggle this help",
                "",
                "(transport via Element's main play/stop;",
                " BPM follows Element)"
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

    TrackerNode* trackerNode;
    int cursorRow    = 0;
    int cursorTrack  = 0;
    int cursorSubCol = 0; // 0 = note, 1 = vel-hi, 2 = vel-lo
    int octave       = 4;
    bool editMode    = false;
    bool showHelp    = false;
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

    patternView->grabKeyboardFocus();

    setResizable (true);
    /* Default: ~24 rows visible × first 4 tracks visible (gutter +
     * 4 track widths + scrollbar) + header. Resizable beyond. */
    setSize (kRowGutterWidth + 4 * kTrackWidth + 16,
             kTrackHeaderH + 24 * kRowHeight + 4);
    startTimerHz (30);
}

TrackerEditor::~TrackerEditor()
{
    stopTimer();
    viewport.reset();
    patternView.reset();
}

void TrackerEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBgColour);
}

void TrackerEditor::resized()
{
    if (viewport)
        viewport->setBounds (getLocalBounds());
    /* PatternView::resized will fire next via the viewport; it sizes
     * itself to the engine pattern's full extent. */
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
}

} // namespace element
