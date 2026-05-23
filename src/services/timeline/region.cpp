// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/timeline/region.hpp"

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
} // namespace

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
    return r;
}

} // namespace element
