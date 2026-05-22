// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "nodes/sampler.hpp"   // SamplerInstrument
#include "ElementApp.h"

namespace element {

/** Session-global pool of sample banks (FT2 / Buzz Tracker model).
 *
 *  Owns the SamplerInstrument table that used to live as a per-node
 *  `instruments[]` vector on each SamplerNode.  Multiple SamplerNodes
 *  in a single session all reference the SAME pool -- they are
 *  consumers, not owners.  Disk Op's Sample Bank pane is the primary
 *  UI surface for mutating the pool (load / rename / clear).  Each
 *  SamplerNode picks which bank(s) it plays via per-channel binding
 *  (channelBinding[MIDI channel] -> bank index).
 *
 *  Singleton.  Lifetime is process-bound; sessions explicitly serialize
 *  the bank table via getStateInformation / setStateInformation, and
 *  clearAll() resets the pool on session-new / session-load-before-
 *  setState.
 *
 *  Thread safety: all public accessors take an internal lock.  Audio
 *  thread reads via SamplerInstrument::Ptr captured at note-on time
 *  -- no pool lock on the audio thread.  ChangeBroadcaster notifies
 *  on the message thread (deferred).
 */
class SampleBankPool : public juce::ChangeBroadcaster
{
public:
    /** Max bank count -- 128 to match FT2's instrument table + Disk
     *  Op's existing 128-row grid (SampleBankPane::kNumBanks). */
    static constexpr int kNumBanks = 128;

    /** Process-lifetime singleton.  Constructed on first access. */
    static SampleBankPool& get();

    /** Number of allocated banks (0..kNumBanks).  Banks are
     *  lazy-grown via ensureInstrumentExists / addInstrument; banks
     *  with no samples loaded still count if explicitly allocated. */
    int getNumInstruments() const;

    /** Returns the bank at `index`, or nullptr if out of range OR
     *  not yet allocated.  Reference-counted Ptr so the audio thread
     *  can keep a copy alive even if the message thread later
     *  reallocates the vector. */
    SamplerInstrument::Ptr getInstrument (int index) const;

    /** Append a new bank to the table.  Returns the new bank, or
     *  nullptr if already at kNumBanks. */
    SamplerInstrument::Ptr addInstrument();

    /** Lazy-allocate banks up to and including targetIndex.  No-op
     *  if banks already exist past targetIndex.  Returns true on
     *  success, false on cap.  Matches Disk Op's existing
     *  "ensureInstrumentExists" pattern. */
    bool ensureInstrumentExists (int targetIndex);

    /** Remove the bank at `index`.  Bank N+1..end shift down.  No-op
     *  on out-of-range OR when count would drop below 1. */
    void removeInstrument (int index);

    /** Wipe the bank's slots + reset name, IN PLACE -- the bank
     *  index doesn't change.  Disk Op's delete-key uses this. */
    void clearInstrument (int index);

    /** Drop every bank.  Called on session-new and at the top of
     *  setStateInformation before deserialising. */
    void clearAll();

    /** Persist / restore the entire bank table.  Wire format mirrors
     *  the legacy SamplerNode per-bank XML (instr / slot trees with
     *  name + sourceFile + tuning + envelopes + keymap) so existing
     *  session files migrate cleanly when the first SamplerNode that
     *  setStateInformation runs imports its embedded bank data into
     *  the pool. */
    void getStateInformation (juce::MemoryBlock&);
    void setStateInformation (const void* data, int size);

    /** Returns true once setStateInformation has been called this
     *  session-load cycle.  SamplerNode::setStateInformation checks
     *  this so it knows whether to forward its embedded legacy bank
     *  data into the pool (first sampler wins; subsequent samplers
     *  skip). */
    bool hasLoaded() const noexcept;

    /** Reset the "has loaded" flag.  Called on session-new to ensure
     *  the next session-load fires setStateInformation again. */
    void resetLoadFlag() noexcept;

private:
    SampleBankPool() = default;
    ~SampleBankPool() override = default;

    SampleBankPool (const SampleBankPool&) = delete;
    SampleBankPool& operator= (const SampleBankPool&) = delete;

    mutable juce::CriticalSection lock_;
    std::vector<SamplerInstrument::Ptr> instruments_;
    bool hasLoaded_ = false;
};

} // namespace element
