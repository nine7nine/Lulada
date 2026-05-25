// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>

namespace element::dsp::automation {

/** Per-cycle parameter change accumulator -- one instance per
 *  automatable parameter target.  Pattern lifted from Zrythm's
 *  `ProcessorBase::ParameterChangeTracker`:
 *
 *    base_value       -- the value the parameter held at the start of
 *                        this audio block (snapshot of the user/host
 *                        knob position).
 *    automated_value  -- value AFTER per-block automation sampling
 *                        from the AutomationEngine (Phase 1).
 *    modulated_value  -- final value AFTER any per-block modulators
 *                        (LFOs etc., future phase).  This is what the
 *                        plugin / node actually consumes.
 *    changed_this_block -- set when modulated_value differs from the
 *                        last block's modulated_value.  Processors
 *                        read this in `process_block()` to decide
 *                        whether to re-derive cached state (e.g. a
 *                        filter coefficient recomputation).
 *
 *  RT-safety: the change tracker is touched by ONE thread only -- the
 *  audio thread, during its render pass.  No locks needed; the atomic
 *  on `automation_mode` (defined separately on AutomationTrack) gates
 *  whether the audio thread bothers sampling.  This struct itself
 *  carries no atomics. */
struct ParameterChangeTracker
{
    double baseValue       { 0.0 };
    double automatedValue  { 0.0 };
    double modulatedValue  { 0.0 };
    double lastModulated   { 0.0 };
    bool   changedThisBlock { false };

    /** Snapshot the user/host knob position at block start.  Called by
     *  the AutomationEngine before sampling automation for this block. */
    void beginBlock (double knobValue) noexcept
    {
        baseValue        = knobValue;
        automatedValue   = knobValue;
        modulatedValue   = knobValue;
        changedThisBlock = false;
    }

    /** Apply an automation-engine sample for this block.  Overwrites
     *  any prior automated/modulated value -- automation takes
     *  precedence over the static knob position when present. */
    void applyAutomation (double v) noexcept
    {
        automatedValue = v;
        modulatedValue = v;
    }

    /** Mark the block done + compute the changed flag.  Processors
     *  read modulatedValue + changedThisBlock from here. */
    void finalizeBlock() noexcept
    {
        changedThisBlock = (modulatedValue != lastModulated);
        lastModulated    = modulatedValue;
    }
};

} // namespace element::dsp::automation
