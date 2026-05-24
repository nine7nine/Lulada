// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/gui_basics.hpp>

namespace element {
namespace ui {

/* Toolbar icon library.  Each free function takes a Graphics, a
 * float-bounds rect to draw inside, and the foreground colour the
 * caller wants used.  Lives in this header so BlockToolButton-like
 * IconDrawer std::function wrappers can adopt any of these by name
 * without duplicate Path data.
 *
 * Icons are sized to fill 60-75 % of the given rect so they read
 * the same way the FontAwesome icons used in the legacy toolbar
 * did, but the strokes are deliberately heavier so the buttons
 * stop reading as default-JUCE chrome.  No anti-alias hacks; rely
 * on juce::Path's built-in stroke joins. */

inline void drawCentredFitted (juce::Graphics& g,
                                juce::Rectangle<float> bounds,
                                juce::Path& p,
                                juce::Colour fg,
                                bool fill = true,
                                float strokePx = 1.4f,
                                float padPct = 0.20f)
{
    const auto pad = juce::jmin (bounds.getWidth(), bounds.getHeight()) * padPct;
    const auto fitBox = bounds.reduced (pad, pad);
    p.applyTransform (p.getTransformToScaleToFit (fitBox, true));
    g.setColour (fg);
    if (fill) g.fillPath (p);
    else      g.strokePath (p, juce::PathStrokeType (strokePx,
                                                       juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
}

/* ---- Transport ---- */

inline void iconPlay (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    juce::Path p;
    p.addTriangle (0.0f, 0.0f, 10.0f, 5.5f, 0.0f, 11.0f);
    drawCentredFitted (g, b, p, fg, true, 1.0f, 0.18f);
}

inline void iconStop (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    juce::Path p;
    p.addRectangle (0.0f, 0.0f, 10.0f, 10.0f);
    drawCentredFitted (g, b, p, fg, true, 1.0f, 0.25f);
}

inline void iconRecord (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    juce::Path p;
    p.addEllipse (0.0f, 0.0f, 10.0f, 10.0f);
    drawCentredFitted (g, b, p, fg, true, 1.0f, 0.22f);
}

inline void iconSeekZero (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    /* |◀  -- vertical bar + triangle pointing left. */
    juce::Path p;
    p.addRectangle (0.0f, 0.0f, 1.6f, 10.0f);
    p.addTriangle (10.0f, 0.0f, 2.5f, 5.0f, 10.0f, 10.0f);
    drawCentredFitted (g, b, p, fg, true, 1.0f, 0.22f);
}

/* ---- File / Edit ---- */

inline void iconMenu (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    /* Hamburger: three horizontal strokes. */
    const auto box = b.reduced (b.getWidth() * 0.18f, b.getHeight() * 0.20f);
    const float y0 = box.getY();
    const float y1 = box.getCentreY();
    const float y2 = box.getBottom();
    g.setColour (fg);
    g.fillRect (juce::Rectangle<float> (box.getX(), y0,      box.getWidth(), 1.6f));
    g.fillRect (juce::Rectangle<float> (box.getX(), y1-0.8f, box.getWidth(), 1.6f));
    g.fillRect (juce::Rectangle<float> (box.getX(), y2-1.6f, box.getWidth(), 1.6f));
}

inline void iconUndo (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    /* Curved arrow swinging left. */
    juce::Path p;
    p.addCentredArc (5.0f, 5.5f, 3.6f, 3.6f, 0.0f,
                      juce::MathConstants<float>::pi * 0.30f,
                      juce::MathConstants<float>::pi * 1.85f,
                      true);
    g.setColour (fg);
    auto pad = juce::jmin (b.getWidth(), b.getHeight()) * 0.22f;
    auto fit = b.reduced (pad, pad);
    p.applyTransform (p.getTransformToScaleToFit (fit, true));
    g.strokePath (p, juce::PathStrokeType (1.6f,
                                            juce::PathStrokeType::curved,
                                            juce::PathStrokeType::rounded));
    /* Arrow head at the start of the arc (top-left). */
    juce::Path head;
    head.addTriangle (0.0f, 0.0f, 3.4f, 0.0f, 1.7f, 2.6f);
    auto headBox = juce::Rectangle<float> (fit.getX() - 1.0f,
                                            fit.getY() - 1.0f,
                                            fit.getWidth() * 0.45f,
                                            fit.getHeight() * 0.45f);
    head.applyTransform (head.getTransformToScaleToFit (headBox, true));
    g.fillPath (head);
}

inline void iconRedo (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    /* Mirror of iconUndo across X. */
    juce::Graphics::ScopedSaveState ss (g);
    g.addTransform (juce::AffineTransform::scale (-1.0f, 1.0f)
                                          .translated (b.getRight() + b.getX(), 0.0f));
    iconUndo (g, b, fg);
}

/* ---- Windows / Plugins ---- */

inline void iconWindows (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    /* Two overlapping rounded rects -- "plugin windows" stack. */
    const auto box = b.reduced (b.getWidth() * 0.15f, b.getHeight() * 0.15f);
    const float w = box.getWidth() * 0.62f;
    const float h = box.getHeight() * 0.62f;
    g.setColour (fg);
    g.drawRoundedRectangle (box.getX(), box.getY(), w, h, 1.2f, 1.4f);
    g.drawRoundedRectangle (box.getRight() - w, box.getBottom() - h, w, h, 1.2f, 1.4f);
}

/* ---- Views (P / G / A / T / S / D / M) ---- */

inline void iconPatchBay (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    /* Patch-bay matrix glyph: 4x4 grid of small squares with a
     * diagonal of "active" cells filled solid, the rest outlined.
     * Mirrors Element's actual patch bay routing grid where each
     * cell is one possible row/column connection. */
    const auto box = b.reduced (b.getWidth() * 0.12f, b.getHeight() * 0.12f);
    constexpr int n = 4;
    const float cellW = (box.getWidth()  - 1.0f * (n - 1)) / (float) n;
    const float cellH = (box.getHeight() - 1.0f * (n - 1)) / (float) n;
    g.setColour (fg);
    for (int row = 0; row < n; ++row)
        for (int col = 0; col < n; ++col)
        {
            const float x = box.getX() + col * (cellW + 1.0f);
            const float y = box.getY() + row * (cellH + 1.0f);
            const bool filled = (row == col)               /* main diagonal */
                              || (row == 1 && col == 3)   /* extra accents */
                              || (row == 3 && col == 1);
            if (filled)
                g.fillRect (x, y, cellW, cellH);
            else
                g.drawRect (x, y, cellW, cellH, 1.0f);
        }
}

inline void iconGraph (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    /* Three boxes connected by lines (block-diagram). */
    const auto box = b.reduced (b.getWidth() * 0.15f, b.getHeight() * 0.20f);
    const float bw = box.getWidth() * 0.25f;
    const float bh = box.getHeight() * 0.32f;
    g.setColour (fg);
    g.drawRect (box.getX(), box.getCentreY() - bh * 0.5f, bw, bh, 1.3f);
    g.drawRect (box.getRight() - bw, box.getY(),          bw, bh, 1.3f);
    g.drawRect (box.getRight() - bw, box.getBottom() - bh, bw, bh, 1.3f);
    g.drawLine (box.getX() + bw, box.getCentreY(),
                 box.getRight() - bw, box.getY() + bh * 0.5f, 1.2f);
    g.drawLine (box.getX() + bw, box.getCentreY(),
                 box.getRight() - bw, box.getBottom() - bh * 0.5f, 1.2f);
}

inline void iconArrangement (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    /* Horizontal lanes -- timeline blocks at different X positions. */
    const auto box = b.reduced (b.getWidth() * 0.15f, b.getHeight() * 0.22f);
    const float laneH = box.getHeight() * 0.28f;
    const float gap = box.getHeight() * 0.06f;
    g.setColour (fg);
    g.fillRoundedRectangle (box.getX(), box.getY(),
                              box.getWidth() * 0.55f, laneH, 1.0f);
    g.fillRoundedRectangle (box.getX() + box.getWidth() * 0.20f,
                              box.getY() + laneH + gap,
                              box.getWidth() * 0.40f, laneH, 1.0f);
    g.fillRoundedRectangle (box.getX() + box.getWidth() * 0.10f,
                              box.getY() + (laneH + gap) * 2.0f,
                              box.getWidth() * 0.70f, laneH, 1.0f);
}

inline void iconTracker (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    /* Vertical pattern grid -- 3 columns of stacked cells. */
    const auto box = b.reduced (b.getWidth() * 0.18f, b.getHeight() * 0.15f);
    g.setColour (fg);
    const float colW = (box.getWidth() - 4.0f) / 3.0f;
    const float rowH = (box.getHeight() - 8.0f) / 5.0f;
    for (int c = 0; c < 3; ++c)
    {
        for (int r = 0; r < 5; ++r)
        {
            g.fillRect (box.getX() + (colW + 2.0f) * c,
                         box.getY() + (rowH + 2.0f) * r,
                         colW, rowH);
        }
    }
}

inline void iconSession (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    /* 2x2 cell grid -- Bitwig session view. */
    const auto box = b.reduced (b.getWidth() * 0.18f, b.getHeight() * 0.18f);
    const float cellW = box.getWidth() * 0.45f;
    const float cellH = box.getHeight() * 0.45f;
    g.setColour (fg);
    g.fillRoundedRectangle (box.getX(),                box.getY(),                cellW, cellH, 1.0f);
    g.fillRoundedRectangle (box.getRight() - cellW,    box.getY(),                cellW, cellH, 1.0f);
    g.fillRoundedRectangle (box.getX(),                box.getBottom() - cellH,    cellW, cellH, 1.0f);
    g.fillRoundedRectangle (box.getRight() - cellW,    box.getBottom() - cellH,    cellW, cellH, 1.0f);
}

inline void iconDisk (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    /* Floppy disk: outer rect, top notch (label) + inner shutter. */
    const auto box = b.reduced (b.getWidth() * 0.18f, b.getHeight() * 0.15f);
    g.setColour (fg);
    g.drawRect (box.toFloat(), 1.4f);
    /* Top metal slider */
    g.fillRect (box.getX() + box.getWidth() * 0.20f,
                 box.getY(),
                 box.getWidth() * 0.60f,
                 box.getHeight() * 0.32f);
    /* Bottom label */
    g.drawRect (box.getX() + box.getWidth() * 0.18f,
                 box.getBottom() - box.getHeight() * 0.45f,
                 box.getWidth() * 0.64f,
                 box.getHeight() * 0.40f,
                 1.2f);
}

inline void iconPluginManager (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    /* Plug glyph: 2 prongs + body + cord. */
    const auto box = b.reduced (b.getWidth() * 0.20f, b.getHeight() * 0.20f);
    g.setColour (fg);
    const float prongW = box.getWidth() * 0.12f;
    const float prongH = box.getHeight() * 0.22f;
    const float bodyW = box.getWidth() * 0.65f;
    const float bodyH = box.getHeight() * 0.40f;
    const float bodyX = box.getCentreX() - bodyW * 0.5f;
    const float bodyY = box.getY() + prongH;
    /* Two prongs */
    g.fillRect (bodyX + bodyW * 0.20f, box.getY(), prongW, prongH);
    g.fillRect (bodyX + bodyW * 0.68f, box.getY(), prongW, prongH);
    /* Body */
    g.fillRoundedRectangle (bodyX, bodyY, bodyW, bodyH, 1.5f);
    /* Cord */
    g.fillRect (box.getCentreX() - 1.0f, bodyY + bodyH,
                 2.0f, box.getBottom() - (bodyY + bodyH));
}

/* ---- Display / utility ---- */

inline void iconLoop (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    const auto box = b.reduced (b.getWidth() * 0.20f, b.getHeight() * 0.22f);
    const float cx = box.getCentreX();
    const float cy = box.getCentreY();
    const float rad = juce::jmin (box.getWidth(), box.getHeight()) * 0.42f;
    juce::Path arc;
    arc.addCentredArc (cx, cy, rad, rad, 0.0f, 0.4f, 6.0f, true);
    g.setColour (fg);
    g.strokePath (arc, juce::PathStrokeType (1.4f,
                                              juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
    juce::Path tip;
    tip.addTriangle (0.0f, 0.0f, 3.5f, 1.0f, 1.0f, 3.5f);
    auto tipBox = juce::Rectangle<float> (cx + rad * 0.55f, cy - rad * 0.95f,
                                            rad * 0.55f, rad * 0.55f);
    tip.applyTransform (tip.getTransformToScaleToFit (tipBox, true));
    g.fillPath (tip);
}

inline void iconMetronome (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    /* Trapezoid body + tick pendulum. */
    const auto box = b.reduced (b.getWidth() * 0.18f, b.getHeight() * 0.12f);
    juce::Path body;
    body.addQuadrilateral (
        box.getX() + box.getWidth() * 0.20f, box.getY(),
        box.getX() + box.getWidth() * 0.80f, box.getY(),
        box.getRight(),                       box.getBottom(),
        box.getX(),                            box.getBottom());
    g.setColour (fg);
    g.strokePath (body, juce::PathStrokeType (1.4f));
    g.drawLine (box.getCentreX(), box.getBottom(),
                 box.getX() + box.getWidth() * 0.60f, box.getY() + box.getHeight() * 0.20f,
                 1.4f);
}

inline void iconKeyboard (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    /* Mini piano: 6 white-key rectangles + 4 black-key cap rectangles. */
    const auto box = b.reduced (b.getWidth() * 0.12f, b.getHeight() * 0.25f);
    g.setColour (fg);
    g.drawRect (box, 1.2f);
    const float w = box.getWidth() / 6.0f;
    for (int i = 1; i < 6; ++i)
        g.drawLine (box.getX() + w * i, box.getY(),
                     box.getX() + w * i, box.getBottom(), 1.0f);
    const float bw = w * 0.55f;
    const float bh = box.getHeight() * 0.55f;
    g.fillRect (box.getX() + w * 1 - bw * 0.5f, box.getY(), bw, bh);
    g.fillRect (box.getX() + w * 2 - bw * 0.5f, box.getY(), bw, bh);
    g.fillRect (box.getX() + w * 4 - bw * 0.5f, box.getY(), bw, bh);
    g.fillRect (box.getX() + w * 5 - bw * 0.5f, box.getY(), bw, bh);
}

inline void iconMeterBars (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    /* Vertical meter bars at descending heights. */
    const auto box = b.reduced (b.getWidth() * 0.18f, b.getHeight() * 0.18f);
    const float w = (box.getWidth() - 6.0f) / 4.0f;
    const float hs[4] = { 0.95f, 0.55f, 0.75f, 0.40f };
    g.setColour (fg);
    for (int i = 0; i < 4; ++i)
    {
        const float h = box.getHeight() * hs[i];
        g.fillRect (box.getX() + (w + 2.0f) * i,
                     box.getBottom() - h, w, h);
    }
}

inline void iconCog (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    /* Settings cog: 8-tooth gear + inner hole. */
    const auto box = b.reduced (b.getWidth() * 0.14f, b.getHeight() * 0.14f);
    const float cx = box.getCentreX();
    const float cy = box.getCentreY();
    const float rOuter = juce::jmin (box.getWidth(), box.getHeight()) * 0.50f;
    const float rInner = rOuter * 0.62f;
    const float rHole  = rOuter * 0.30f;

    juce::Path gear;
    constexpr int teeth = 8;
    constexpr float twoPi = juce::MathConstants<float>::twoPi;
    for (int i = 0; i < teeth * 2; ++i)
    {
        const float a = (twoPi * i) / (teeth * 2.0f);
        const float r = (i % 2 == 0) ? rOuter : rInner;
        const float x = cx + std::cos (a) * r;
        const float y = cy + std::sin (a) * r;
        if (i == 0) gear.startNewSubPath (x, y);
        else        gear.lineTo (x, y);
    }
    gear.closeSubPath();
    g.setColour (fg);
    g.fillPath (gear);

    /* Centre hole (knock out a smaller filled circle in background
     * colour -- approximate by drawing a black disc.  Body sits on
     * dark button, so a hard black hole reads correctly). */
    g.setColour (juce::Colour (0xff'1c'1c'1c));
    g.fillEllipse (cx - rHole, cy - rHole, rHole * 2.0f, rHole * 2.0f);
}

inline void iconChannelStrip (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    /* Vertical fader track + cap. */
    const auto box = b.reduced (b.getWidth() * 0.30f, b.getHeight() * 0.10f);
    const float trackW = 2.0f;
    const float trackX = box.getCentreX() - trackW * 0.5f;
    g.setColour (fg);
    g.fillRect (trackX, box.getY(), trackW, box.getHeight());
    const float capH = box.getHeight() * 0.16f;
    const float capW = box.getWidth() * 1.6f;
    g.fillRoundedRectangle (box.getCentreX() - capW * 0.5f,
                              box.getCentreY() - capH * 0.5f,
                              capW, capH, 1.5f);
}

inline void iconMixer (juce::Graphics& g, juce::Rectangle<float> b, juce::Colour fg)
{
    /* 3-channel mixer: three vertical fader tracks side by side
     * with caps at different positions -- reads as a mini mixing
     * console at any button size. */
    const auto box = b.reduced (b.getWidth() * 0.12f, b.getHeight() * 0.15f);
    const float colSpan = box.getWidth() / 3.0f;
    const float trackW = 1.5f;
    const float capW = colSpan * 0.70f;
    const float capH = box.getHeight() * 0.13f;
    /* Cap Y-positions as a fraction down the column for each fader. */
    const float capYFrac[3] = { 0.30f, 0.55f, 0.20f };
    g.setColour (fg);
    for (int i = 0; i < 3; ++i)
    {
        const float cx = box.getX() + colSpan * (i + 0.5f);
        g.fillRect (cx - trackW * 0.5f, box.getY(), trackW, box.getHeight());
        const float capY = box.getY() + box.getHeight() * capYFrac[i] - capH * 0.5f;
        g.fillRoundedRectangle (cx - capW * 0.5f, capY, capW, capH, 1.0f);
    }
}

} // namespace ui
} // namespace element
