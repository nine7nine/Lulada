// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ElementApp.h"

#include <element/node.hpp>
#include <element/tags.hpp>

namespace element {

/* Shared category / format / node colour palette.
 *
 * Single source of truth for the per-category accent colours used by:
 *   - graph block outlines (src/ui/block.cpp)
 *   - plugin-manager row tints (src/ui/pluginmanagercomponent.cpp)
 *   - anywhere else that wants to surface "what kind of node is this"
 *
 * The category strings are matched case-insensitively against both
 * halves of a potential vendor-style "X|Y" tag.  Plugin vendors are
 * inconsistent about whether the bucket is in the first or second
 * slot (e.g. CLAP uses "delay", VST3 uses "Fx|Delay") so we accept
 * either. */

inline juce::Colour defaultColorForCategory (const juce::String& category)
{
    if (category.isEmpty()) return juce::Colour (0x00000000);

    const auto first  = category.upToFirstOccurrenceOf ("|", false, true).trim().toLowerCase();
    const auto second = category.fromFirstOccurrenceOf ("|", false, true).trim().toLowerCase();
    auto matchAny = [&] (std::initializer_list<const char*> patterns) {
        for (auto* p : patterns)
        {
            if (first == p || second == p
                || first.contains (p) || second.contains (p))
                return true;
        }
        return false;
    };

    /* === Most-specific effect buckets first (so an "Fx|Reverb"
         lands on "reverb" rather than the generic "effect" bucket). */
    if (matchAny ({ "reverb" }))             return juce::Colour { 0xff'4a'a8'8c };  /* teal-green */
    if (matchAny ({ "delay", "echo" }))      return juce::Colour { 0xff'5a'b0'b0 };  /* aqua */
    if (matchAny ({ "eq", "equalizer", "equaliser" }))
                                              return juce::Colour { 0xff'9a'c0'4a };  /* lime */
    if (matchAny ({ "dynamics", "compressor", "limiter", "gate",
                    "expander", "mastering" }))
                                              return juce::Colour { 0xff'c0'b0'40 };  /* mustard */
    if (matchAny ({ "distortion", "saturation", "amp", "amplifier" }))
                                              return juce::Colour { 0xff'd0'60'40 };  /* burnt orange */
    if (matchAny ({ "modulation", "chorus", "flanger", "phaser",
                    "tremolo", "vibrato" }))
                                              return juce::Colour { 0xff'a0'5a'c0 };  /* lavender */
    if (matchAny ({ "filter", "wah" }))      return juce::Colour { 0xff'5a'c0'a0 };  /* mint */
    if (matchAny ({ "pitch", "pitch-shift", "pitch-shifter",
                    "pitch-correction", "vocoder" }))
                                              return juce::Colour { 0xff'b0'9a'c0 };  /* dusty purple */
    if (matchAny ({ "spatial", "stereo", "surround", "panner" }))
                                              return juce::Colour { 0xff'5a'7a'c0 };  /* sky blue */
    if (matchAny ({ "analyzer", "analyser", "meter", "spectrum" }))
                                              return juce::Colour { 0xff'40'a0'c0 };  /* steel blue */
    if (matchAny ({ "drum", "drum-machine", "percussion" }))
                                              return juce::Colour { 0xff'e0'70'4a };  /* terracotta */

    /* === Broad family buckets. */
    if (matchAny ({ "instrument", "synth", "synthesizer", "synthesiser",
                    "sampler", "rom-sampler", "monosynth", "multivoice",
                    "virtual-instrument", "piano", "strings", "brass",
                    "winds", "ensemble", "external" }))
        return juce::Colour { 0xff'8a'5a'c0 };                                       /* warm purple */
    if (matchAny ({ "sequencer", "tracker", "arpeggiator",
                    "step-sequencer", "pattern" }))
        return juce::Colour { 0xff'40'a0'a0 };                                       /* teal */
    if (matchAny ({ "midi" }))
        return juce::Colour { 0xff'd0'80'40 };                                       /* orange */
    if (matchAny ({ "effect", "fx", "multi-effects", "tools",
                    "utility" }))
        return juce::Colour { 0xff'5a'a5'5a };                                       /* green */
    if (matchAny ({ "mixer", "mixing", "router", "routing", "channel" }))
        return juce::Colour { 0xff'5a'7a'a0 };                                       /* slate blue */
    if (matchAny ({ "audio", "player", "media", "playback" }))
        return juce::Colour { 0xff'4a'90'd0 };                                       /* blue */
    if (matchAny ({ "control", "osc", "script", "automation",
                    "control-surface" }))
        return juce::Colour { 0xff'c0'a0'40 };                                       /* gold */
    if (matchAny ({ "generator", "visualiser", "visualizer", "scope",
                    "noise" }))
        return juce::Colour { 0xff'b0'5a'a0 };                                       /* magenta */

    return juce::Colour { 0xff'70'70'70 };
}

/* Plugin format → accent colour.  Used as fallback when a plugin / node
 * has no category at all. */
inline juce::Colour defaultColorForFormat (const juce::String& format)
{
    if (format == "VST")       return juce::Colour (0xff'3d'5a'fe);  // indigo A400 — VST2
    if (format == "VST3")      return juce::Colour (0xff'd5'00'f9);  // purple A400 — VST3
    if (format == "CLAP")      return juce::Colour (0xff'00'e5'ff);  // cyan   A400 — CLAP
    if (format == "LV2")       return juce::Colour (0xff'ff'17'44);  // red    A400 — LV2
    if (format == "AudioUnit") return juce::Colour (0xff'ff'91'00);  // orange A400 — AU
    if (format == "Element")   return juce::Colour (0xff'ff'40'81);  // pink   A400 — Element internal
    return juce::Colour (0xff'bd'bd'bd);                              // gray 400 fallback
}

/* Category first, format fallback.  Use this anywhere you have a
 * category string + format string but no Node — e.g. the plugin
 * manager's table model where the rows are KnownPluginList entries. */
inline juce::Colour colorForCategoryOrFormat (const juce::String& category,
                                              const juce::String& format)
{
    if (category.isNotEmpty())
    {
        const auto c = defaultColorForCategory (category);
        if (c != juce::Colour (0x00000000))
            return c;
    }
    return defaultColorForFormat (format);
}

/* Resolved per-node accent colour.  Single source of truth for the
 * "what kind of node is this" tint surfaced by graph block borders
 * (block.cpp), graph mixer strips (nodechannelstrip.hpp), node-strip
 * dock + anywhere else that wants this signal.  Priority:
 *   1. Audio / MIDI I/O pseudo-nodes -- match their wire colour.
 *   2. Plugin category (via tags::category property, or the live
 *      Processor's PluginDescription as fallback for sessions saved
 *      before tags::category existed).
 *   3. Plugin format (VST / VST3 / CLAP / LV2 / AU / Element) when
 *      there's no category at all. */
inline juce::Colour colorForNode (const Node& node)
{
    if (! node.isValid())
        return juce::Colour (0xff'bd'bd'bd);

    if (node.isAudioInputNode() || node.isAudioOutputNode())
        return juce::Colour (0xff'00'e6'76);   // green A400 -- matches audio wire
    if (node.isMidiInputNode() || node.isMidiOutputNode())
        return juce::Colour (0xff'ff'a7'26);   // orange 400 -- matches MIDI wire

    auto category = node.getProperty (tags::category).toString();
    if (category.isEmpty())
    {
        if (auto obj = node.getObject())
        {
            juce::PluginDescription pd;
            obj->getPluginDescription (pd);
            category = pd.category;
        }
    }
    return colorForCategoryOrFormat (category, node.getFormat().toString());
}

/** Per-node tint that prefers an Arrangement Lane binding's `colour`
 *  when the node is the target of any lane (Lane.targetNodeUuid ==
 *  node.getUuid()).  Falls back to `colorForNode` when no lane points
 *  here.  Lets audio-clip mixer strips inherit the lane colour the
 *  user picks in ArrangementView, matching Bitwig / Ableton.
 *
 *  `sessionRoot` is the session's root ValueTree (typically obtained
 *  via `ViewHelpers::getSession(this)->data()` at the call site).
 *  Pass an invalid VT to short-circuit to category-only colour. */
inline juce::Colour colorForNodeWithLane (const Node& node,
                                          const juce::ValueTree& sessionRoot)
{
    if (sessionRoot.isValid() && node.isValid())
    {
        const auto arrangement = sessionRoot.getChildWithName (tags::arrangement);
        if (arrangement.isValid())
        {
            const auto lanes = arrangement.getChildWithName ("lanes");
            if (lanes.isValid())
            {
                const auto target = node.getUuid().toString();
                for (int i = 0; i < lanes.getNumChildren(); ++i)
                {
                    const auto lane = lanes.getChild (i);
                    if (lane.getProperty ("targetNodeUuid").toString() == target)
                    {
                        const auto s = lane.getProperty ("colour").toString();
                        if (s.isNotEmpty())
                            return juce::Colour::fromString (s);
                        break;
                    }
                }
            }
        }
    }
    return colorForNode (node);
}

} // namespace element
