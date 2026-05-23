// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "services/timeline/playlist.hpp"

#include <memory>

namespace element {

class TimelineAdapter;     // Phase 2: per-kind dispatcher

/** One row in the arrangement view.  Binds a Playlist to a target
 *  graph node via uuid; the Lane is NOT a graph node itself.  Adding
 *  / removing lanes doesn't change graph topology -- you add a node
 *  separately and then point a Lane at it.
 *
 *  Lane is value-typed at the data layer (this struct), wrapped by
 *  ArrangementView's UI components.  Lifetime: owned by the
 *  arrangement session-state container (Phase 1d serialises into
 *  tags::arrangement).
 *
 *  See timeline-audio-design.md Section 1.3.
 */
struct Lane
{
    juce::Uuid    id;
    juce::String  name;
    juce::Colour  colour     { 0xff'30'30'30 };
    int           heightPx   { 60 };
    bool          muted      { false };
    bool          soloed     { false };
    bool          armed      { false };

    /** Uuid of the target node in the live graph.  Resolved at adapter
     *  bind time -- when the target node is missing (e.g. user
     *  deleted it from the graph editor), the lane stays in the
     *  arrangement but doesn't dispatch.  ArrangementView surfaces
     *  this as a greyed lane with a "rebind" affordance. */
    juce::Uuid    targetNodeUuid;

    /** Dispatch contents.  Owned by the Lane; copy-on-write semantics
     *  would be a v2 optimisation if we want lane duplication to
     *  share regions until the user mutates one. */
    Playlist      playlist;

    /** Sparse-write XML.  Only writes non-default fields. */
    juce::ValueTree toValueTree() const;
    static Lane     fromValueTree (const juce::ValueTree&);
};

} // namespace element
