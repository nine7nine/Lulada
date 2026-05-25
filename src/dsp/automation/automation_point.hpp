// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dsp/automation/curve.hpp"

namespace element::dsp::automation {

/** Single breakpoint on an AutomationRegion's curve.
 *
 *  tBeats is measured from the region's local start (0 == the region's
 *  positionBeats on the timeline).  valueNormalized is the parameter
 *  target value in [0, 1] (callers map to/from the target's natural
 *  range at the AutomationTarget boundary).
 *
 *  curve describes the SHAPE OF THE SEGMENT FROM THIS POINT TO THE
 *  NEXT POINT (sorted by tBeats ascending).  The last point's curve
 *  is unused -- there is no segment past it.  AutomationRegion holds
 *  these in a sorted std::vector and never mutates one in place; UI
 *  edits build a new vector and atomic-swap the snapshot ptr.
 *
 *  POD with trivial default ctor + trivial copy + trivial destructor
 *  so vectors of AutomationPoint can be constructed/copied/destroyed
 *  without per-element allocation. */
struct AutomationPoint
{
    double        tBeats           { 0.0 };
    double        valueNormalized  { 0.0 };
    CurveOptions  curve            { };
};

} // namespace element::dsp::automation
