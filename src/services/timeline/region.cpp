// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/timeline/region.hpp"

#include <algorithm>
#include <cmath>

namespace element {

namespace {
/* Property identifiers for the region ValueTree.  Short strings keep
 * the on-disk XML compact -- a session with hundreds of regions
 * shouldn't pay a percent overhead in attribute name bytes. */
const juce::Identifier kIdAttr        ("id");
const juce::Identifier kSourceIdAttr  ("src");
const juce::Identifier kSeqIdxAttr    ("seq");
const juce::Identifier kPosAttr       ("pos");
const juce::Identifier kStartAttr     ("start");
const juce::Identifier kLenAttr       ("len");
const juce::Identifier kGainAttr      ("gainDb");
const juce::Identifier kFadeInAttr    ("fadeIn");
const juce::Identifier kFadeOutAttr   ("fadeOut");
const juce::Identifier kLoopedAttr    ("loop");
const juce::Identifier kColourAttr    ("colour");
const juce::Identifier kNameAttr      ("name");

/* Volume envelope subtree.  Stored as a child <env> node containing
 * a flat list of <p> point nodes; keeps XML compact + diff-friendly. */
const juce::Identifier kEnvTag        ("env");
const juce::Identifier kEnvPointTag   ("p");
const juce::Identifier kEnvPtIdAttr   ("id");
const juce::Identifier kEnvPtBeatAttr ("b");
const juce::Identifier kEnvPtGainAttr ("g");
const juce::Identifier kEnvPtCurveAttr ("c");
} // namespace

float Region::gainAtBeatOffset (double localBeat) const noexcept
{
    if (volumeEnvelope.empty())  return (float) gainDb;
    if (volumeEnvelope.size() == 1) return volumeEnvelope.front().gainDb;

    /* Clamp out-of-range to endpoint gain (no extrapolation; an
     * envelope ends where its last point sits). */
    if (localBeat <= volumeEnvelope.front().beatOffset)
        return volumeEnvelope.front().gainDb;
    if (localBeat >= volumeEnvelope.back().beatOffset)
        return volumeEnvelope.back().gainDb;

    /* Linear scan -- breakpoints typically number in single digits
     * per region; binary search would dominate cache misses for the
     * sizes that actually occur in practice. */
    for (size_t i = 0; i + 1 < volumeEnvelope.size(); ++i)
    {
        const auto& a = volumeEnvelope[i];
        const auto& b = volumeEnvelope[i + 1];
        if (localBeat < a.beatOffset || localBeat >= b.beatOffset) continue;

        const double span = juce::jmax (1e-9, b.beatOffset - a.beatOffset);
        const double t    = juce::jlimit (0.0, 1.0,
                                            (localBeat - a.beatOffset) / span);
        double shaped = t;
        switch (a.curve)
        {
            case EnvelopeCurve::Linear:      shaped = t;                                       break;
            case EnvelopeCurve::Exponential: shaped = t * t;                                   break;
            case EnvelopeCurve::Smooth:      shaped = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * t); break;
            case EnvelopeCurve::Hold:        shaped = 0.0;                                     break;
        }
        return (float) (a.gainDb + shaped * (b.gainDb - a.gainDb));
    }
    return (float) gainDb;
}

void Region::sortEnvelope() noexcept
{
    std::sort (volumeEnvelope.begin(), volumeEnvelope.end(),
               [] (const EnvelopePoint& a, const EnvelopePoint& b) {
                   return a.beatOffset < b.beatOffset;
               });
}

juce::ValueTree Region::toValueTree() const
{
    juce::ValueTree v ("region");
    v.setProperty (kIdAttr,       id.toString(),         nullptr);
    v.setProperty (kSourceIdAttr, sourceId.toString(),   nullptr);
    v.setProperty (kPosAttr,      positionBeats,         nullptr);
    /* Sparse-write the rest -- only when not at the default value.
     * Keeps tracker-only regions tiny. */
    if (sequenceIdx  >= 0)            v.setProperty (kSeqIdxAttr,  sequenceIdx,          nullptr);
    if (startBeats   != 0.0)         v.setProperty (kStartAttr,   startBeats,           nullptr);
    if (lengthBeats  != 0.0)         v.setProperty (kLenAttr,     lengthBeats,          nullptr);
    if (gainDb       != 0.0)         v.setProperty (kGainAttr,    gainDb,               nullptr);
    if (fadeInBeats  != 0.0)         v.setProperty (kFadeInAttr,  fadeInBeats,          nullptr);
    if (fadeOutBeats != 0.0)         v.setProperty (kFadeOutAttr, fadeOutBeats,         nullptr);
    if (looped)                       v.setProperty (kLoopedAttr,  true,                 nullptr);
    /* Colour stored as ARGB hex string; only emit when set to a
     * non-default tint (every Region has a colour at construction,
     * but most clients want lane-default which writes through here
     * as the same value -- we still emit). */
    v.setProperty (kColourAttr,   colour.toString(),     nullptr);
    if (name.isNotEmpty())            v.setProperty (kNameAttr,    name,                 nullptr);

    /* Envelope: append a child <env> with one <p> per breakpoint.
     * Skipped when empty so the on-disk shape matches v0 regions. */
    if (! volumeEnvelope.empty())
    {
        juce::ValueTree envNode (kEnvTag);
        for (const auto& pt : volumeEnvelope)
        {
            juce::ValueTree pNode (kEnvPointTag);
            pNode.setProperty (kEnvPtIdAttr,   pt.id.toString(),       nullptr);
            pNode.setProperty (kEnvPtBeatAttr, pt.beatOffset,          nullptr);
            pNode.setProperty (kEnvPtGainAttr, (double) pt.gainDb,     nullptr);
            pNode.setProperty (kEnvPtCurveAttr, (int) pt.curve,        nullptr);
            envNode.appendChild (pNode, nullptr);
        }
        v.appendChild (envNode, nullptr);
    }
    return v;
}

Region Region::fromValueTree (const juce::ValueTree& v)
{
    Region r;
    if (! v.isValid() || v.getType() != juce::Identifier ("region"))
        return r;

    r.id            = juce::Uuid (v.getProperty (kIdAttr).toString());
    r.sourceId      = juce::Uuid (v.getProperty (kSourceIdAttr).toString());
    r.sequenceIdx   = (int)    v.getProperty (kSeqIdxAttr, -1);
    r.positionBeats = (double) v.getProperty (kPosAttr,     0.0);
    r.startBeats    = (double) v.getProperty (kStartAttr,   0.0);
    r.lengthBeats   = (double) v.getProperty (kLenAttr,     0.0);
    r.gainDb        = (double) v.getProperty (kGainAttr,    0.0);
    r.fadeInBeats   = (double) v.getProperty (kFadeInAttr,  0.0);
    r.fadeOutBeats  = (double) v.getProperty (kFadeOutAttr, 0.0);
    r.looped        = (bool)   v.getProperty (kLoopedAttr,  false);
    {
        const juce::String s = v.getProperty (kColourAttr).toString();
        if (s.isNotEmpty()) r.colour = juce::Colour::fromString (s);
    }
    r.name          = v.getProperty (kNameAttr, juce::String()).toString();

    /* Envelope child (forward-compatible: missing <env> reads as
     * empty, matching v0 regions). */
    const auto envNode = v.getChildWithName (kEnvTag);
    if (envNode.isValid())
    {
        const int n = envNode.getNumChildren();
        r.volumeEnvelope.reserve ((size_t) juce::jmax (0, n));
        for (int i = 0; i < n; ++i)
        {
            const auto p = envNode.getChild (i);
            if (p.getType() != kEnvPointTag) continue;
            EnvelopePoint pt;
            const auto idStr = p.getProperty (kEnvPtIdAttr).toString();
            pt.id         = idStr.isNotEmpty() ? juce::Uuid (idStr) : juce::Uuid();
            pt.beatOffset = (double) p.getProperty (kEnvPtBeatAttr, 0.0);
            pt.gainDb     = (float)  (double) p.getProperty (kEnvPtGainAttr, 0.0);
            pt.curve      = (EnvelopeCurve) (int) p.getProperty (kEnvPtCurveAttr, 0);
            r.volumeEnvelope.push_back (pt);
        }
        r.sortEnvelope();
    }
    return r;
}

} // namespace element
