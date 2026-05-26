// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/midiplayer.hpp"

#include <algorithm>
#include <cmath>

namespace element {

MidiPlayerNode::MidiPlayerNode()
    : MidiFilterNode (0)
{
    setName ("MIDI Player");

    /* Publish an empty entry table up front so the audio thread
     * never observes a null pointer. */
    activeEntries_.store (new EntryList(), std::memory_order_release);
}

MidiPlayerNode::~MidiPlayerNode()
{
    if (auto* live = activeEntries_.exchange (nullptr, std::memory_order_acq_rel))
        delete live;
}

void MidiPlayerNode::prepareToRender (double sampleRate, int maxBufferSize)
{
    currentSampleRate_ = sampleRate;
    currentBlockSize_  = maxBufferSize;
    lastPlayingState_  = false;
    held_.reset();
}

void MidiPlayerNode::releaseResources()
{
    /* Nothing to free on the audio side; entries table is released
     * in the dtor. */
}

void MidiPlayerNode::refreshPorts()
{
    if (createdPorts_) return;

    PortList newPorts;
    newPorts.add (PortType::Midi, 0, 0, "midi_out", "MIDI Out", false);
    createdPorts_ = true;
    setPorts (newPorts);
}

void MidiPlayerNode::setState (const void*, int)
{
    /* Stateless on the wire -- the bound region list is rebuilt by
     * the message thread from the lane's playlist after every
     * session load.  Persistence lives in the Lane / Playlist /
     * MidiNoteRegion ValueTree path; this node's only "state" is
     * its identity (it's a singleton-per-lane). */
}

void MidiPlayerNode::getState (juce::MemoryBlock& block)
{
    block.setSize (0);
}

//==============================================================================

void MidiPlayerNode::setBoundRegions (std::vector<RegionEntry> entries)
{
    /* Build a fresh immutable EntryList, atomic-swap it in, push the
     * displaced one onto the trash deque.  Audio thread observes the
     * swap on its next snapshot load. */
    auto next = std::make_unique<EntryList> (std::move (entries));
    const EntryList* raw   = next.release();
    const auto       stamp = audioEpoch_.load (std::memory_order_acquire);
    const EntryList* old   = activeEntries_.exchange (raw, std::memory_order_acq_rel);
    if (old != nullptr)
        entriesTrash_.push_back (EntryTrash {
            std::unique_ptr<const EntryList> (old), stamp });
}

void MidiPlayerNode::sweepBindingsTrash() noexcept
{
    const auto safeEpoch = audioEpoch_.load (std::memory_order_acquire);
    while (! entriesTrash_.empty()
           && entriesTrash_.front().stampEpoch < safeEpoch)
        entriesTrash_.pop_front();
}

//==============================================================================

bool MidiPlayerNode::computeBlockBeats (int    numSamples,
                                          double& blockStartBeat,
                                          double& blockEndBeat,
                                          double& samplesPerBeat) noexcept
{
    blockStartBeat = -1.0;
    blockEndBeat   = -1.0;
    samplesPerBeat = 0.0;

    auto* ph = getPlayHead();
    if (ph == nullptr) return false;
    auto pos = ph->getPosition();
    if (! pos.hasValue()) return false;

    /* BPM is required to convert sample-offset for sub-block events.
     * If absent default to 120 -- consistent with ArrangementView /
     * AudioClipNode fallback. */
    double bpm = 120.0;
    if (auto b = pos->getBpm())
        bpm = (double) *b;
    if (bpm <= 0.0) return false;

    if (currentSampleRate_ <= 0.0) return false;
    samplesPerBeat = currentSampleRate_ * 60.0 / bpm;

    /* Prefer getPpqPosition (already in beats) over time-in-samples
     * which would need a beat-of-origin reference. */
    if (auto ppq = pos->getPpqPosition())
    {
        blockStartBeat = (double) *ppq;
        blockEndBeat   = blockStartBeat + (double) numSamples / samplesPerBeat;
        return true;
    }

    if (auto inSamples = pos->getTimeInSamples())
    {
        blockStartBeat = (double) *inSamples / samplesPerBeat;
        blockEndBeat   = blockStartBeat + (double) numSamples / samplesPerBeat;
        return true;
    }

    return false;
}

void MidiPlayerNode::emitAllNotesOff (juce::MidiBuffer& out,
                                        int               sampleOffset) noexcept
{
    if (held_.none()) return;
    for (int i = 0; i < kHeldBits; ++i)
    {
        if (! held_.test ((std::size_t) i)) continue;
        const int channel = (i / 128) + 1;   /* back to 1-based */
        const int pitch   = i % 128;
        out.addEvent (juce::MidiMessage::noteOff (channel, pitch),
                      sampleOffset);
        held_.reset ((std::size_t) i);
    }
}

void MidiPlayerNode::render (RenderContext& rc)
{
    if (rc.midi.getNumBuffers() <= 0) return;
    juce::MidiBuffer* const out = rc.midi.getWriteBuffer (0);
    if (out == nullptr) return;

    /* Audio thread always clears its output buffer at block start --
     * we don't forward anything from upstream (no MIDI input on this
     * node).  refreshPorts() reserves output port 0 only. */
    out->clear();

    const int nsamples = rc.audio.getNumSamples();

    /* Bump epoch FIRST so the message-thread sweep can prove every
     * stamp <= prevEpoch is safe to free.  Subsequent loads in this
     * block see the bumped value but the snapshot pointer they got
     * was published BEFORE the bump, so it stays valid. */
    audioEpoch_.fetch_add (1, std::memory_order_acq_rel);

    /* Read transport state once at the top.  No playhead / no
     * play position means "stopped" -- emit all-notes-off for any
     * lingering held pairs and return. */
    bool wantPlaying = false;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
            wantPlaying = pos->getIsPlaying();
    }

    if (! wantPlaying)
    {
        if (lastPlayingState_)
            emitAllNotesOff (*out, 0);
        lastPlayingState_ = false;
        return;
    }
    lastPlayingState_ = true;

    double blockStartBeat = 0.0;
    double blockEndBeat   = 0.0;
    double samplesPerBeat = 0.0;
    if (! computeBlockBeats (nsamples, blockStartBeat, blockEndBeat, samplesPerBeat))
    {
        /* Transport says playing but no PPQ/sample info -- conservative
         * behaviour: hold the previous state (do nothing), don't
         * leak NoteOns. */
        return;
    }

    const EntryList* entries = activeEntries_.load (std::memory_order_acquire);
    if (entries == nullptr || entries->empty()) return;

    for (const auto& entry : *entries)
    {
        if (entry.region == nullptr || entry.lengthBeats <= 0.0)
            continue;

        const double regionStart = entry.positionBeats;
        const double regionEnd   = entry.positionBeats + entry.lengthBeats;

        if (entry.looped)
        {
            /* Looped: emit notes whose loop-wrapped local-beat
             * window falls in this block. */
            emitRegionInBlock (entry, blockStartBeat, blockEndBeat,
                               samplesPerBeat, nsamples, *out);
            continue;
        }

        /* Non-looped: only emit when the block intersects the
         * region's [start, end) range.  If we've left the region
         * since the previous block, flush any held notes for that
         * region's channels (cheap approximation: flush all held). */
        const bool blockTouchesRegion =
            blockEndBeat > regionStart && blockStartBeat < regionEnd;

        if (! blockTouchesRegion)
            continue;

        emitRegionInBlock (entry, blockStartBeat, blockEndBeat,
                           samplesPerBeat, nsamples, *out);
    }

    /* Transport-cross-region tidy-up: if the block straddles the
     * tail end of a region and notes are still held, emit
     * matching offs at the region's end-sample.  Simpler scheme
     * for v1: rely on per-note NoteOff emission via the snapshot
     * walk in emitRegionInBlock.  If a note's lengthBeats extends
     * past the region's lengthBeats AND we're inside the region's
     * tail block, the NoteOff still fires at the calculated
     * end-beat.  Notes whose lengthBeats overshoots regionEnd are
     * clamped to regionEnd inside emitRegionInBlock.  This keeps
     * the render path branch-free at block boundaries. */
}

void MidiPlayerNode::emitRegionInBlock (const RegionEntry& entry,
                                          double             blockStartBeat,
                                          double             blockEndBeat,
                                          double             samplesPerBeat,
                                          int                numSamples,
                                          juce::MidiBuffer&  out) noexcept
{
    const auto* snap = entry.region->loadSnapshot();
    if (snap == nullptr || snap->empty()) return;

    /* Advance the region's audio epoch so its trash sweep can
     * reclaim displaced note snapshots. */
    entry.region->advanceAudioEpoch();

    const double regionStart = entry.positionBeats;
    const double regionLen   = entry.lengthBeats;
    const double regionEnd   = regionStart + regionLen;

    /* Map the block's transport beat range into the region's local
     * beat coordinates.  For looped regions, modulo into [0, len). */
    auto toLocal = [regionStart, regionLen, looped = entry.looped]
                   (double beat) noexcept -> double {
        const double lb = beat - regionStart;
        if (! looped) return lb;
        if (regionLen <= 0.0) return lb;
        double m = std::fmod (lb, regionLen);
        if (m < 0.0) m += regionLen;
        return m;
    };

    /* For the non-looped path, clamp the local-beat window to
     * [0, regionLen) -- the audio thread is inside the region's
     * span so events past lengthBeats don't fire. */
    double localStart, localEnd;
    if (entry.looped)
    {
        localStart = toLocal (blockStartBeat);
        localEnd   = localStart + (blockEndBeat - blockStartBeat);
        /* If localEnd wraps past regionLen, we have to split into
         * two windows: [localStart, regionLen) + [0, localEnd-regionLen).
         * Implementation: just call the inner loop twice with adjusted
         * sample offsets. */
        if (localEnd > regionLen)
        {
            const double tailLen = regionLen - localStart;
            /* First chunk: [localStart, regionLen). */
            for (const auto& n : *snap)
            {
                const double noteEnd = n.onBeat + n.lengthBeats;
                if (n.onBeat >= regionLen || noteEnd <= 0.0) continue;

                /* NoteOn falls in [localStart, regionLen). */
                if (n.onBeat >= localStart && n.onBeat < regionLen)
                {
                    const double beatDelta = n.onBeat - localStart;
                    int sampleOffset = (int) std::round (beatDelta * samplesPerBeat);
                    sampleOffset = juce::jlimit (0, numSamples - 1, sampleOffset);
                    out.addEvent (juce::MidiMessage::noteOn (
                                       juce::jlimit (1, 16, n.channel),
                                       juce::jlimit (0, 127, n.pitch),
                                       (juce::uint8) juce::jlimit (1, 127, n.velocity)),
                                  sampleOffset);
                    held_.set ((std::size_t) heldIndex (n.channel, n.pitch));
                }

                /* NoteOff falls in [localStart, regionLen). */
                if (noteEnd > localStart && noteEnd <= regionLen)
                {
                    const double beatDelta = noteEnd - localStart;
                    int sampleOffset = (int) std::round (beatDelta * samplesPerBeat);
                    sampleOffset = juce::jlimit (0, numSamples - 1, sampleOffset);
                    out.addEvent (juce::MidiMessage::noteOff (
                                       juce::jlimit (1, 16, n.channel),
                                       juce::jlimit (0, 127, n.pitch)),
                                  sampleOffset);
                    held_.reset ((std::size_t) heldIndex (n.channel, n.pitch));
                }
            }

            /* Second chunk: wrap window [0, localEnd-regionLen).
             * Sample offset begins at tailLen-into-the-block. */
            const double wrapEnd = localEnd - regionLen;
            const int wrapBaseOffset =
                (int) std::round (tailLen * samplesPerBeat);
            for (const auto& n : *snap)
            {
                const double noteEnd = n.onBeat + n.lengthBeats;
                if (noteEnd <= 0.0) continue;

                if (n.onBeat >= 0.0 && n.onBeat < wrapEnd)
                {
                    const double beatDelta = n.onBeat;
                    int sampleOffset = wrapBaseOffset
                                     + (int) std::round (beatDelta * samplesPerBeat);
                    sampleOffset = juce::jlimit (0, numSamples - 1, sampleOffset);
                    out.addEvent (juce::MidiMessage::noteOn (
                                       juce::jlimit (1, 16, n.channel),
                                       juce::jlimit (0, 127, n.pitch),
                                       (juce::uint8) juce::jlimit (1, 127, n.velocity)),
                                  sampleOffset);
                    held_.set ((std::size_t) heldIndex (n.channel, n.pitch));
                }

                if (noteEnd > 0.0 && noteEnd < wrapEnd)
                {
                    const double beatDelta = noteEnd;
                    int sampleOffset = wrapBaseOffset
                                     + (int) std::round (beatDelta * samplesPerBeat);
                    sampleOffset = juce::jlimit (0, numSamples - 1, sampleOffset);
                    out.addEvent (juce::MidiMessage::noteOff (
                                       juce::jlimit (1, 16, n.channel),
                                       juce::jlimit (0, 127, n.pitch)),
                                  sampleOffset);
                    held_.reset ((std::size_t) heldIndex (n.channel, n.pitch));
                }
            }
            return;
        }
        /* fall through to common emission with looped-mapped window */
    }
    else
    {
        localStart = blockStartBeat - regionStart;
        localEnd   = blockEndBeat   - regionStart;
        /* Clamp the window to the region.  Negative localStart means
         * the region begins inside the block -- handled by skipping
         * notes whose onBeat < 0. */
        if (localEnd <= 0.0) return;
    }

    /* Common emission for non-looped + looped-no-wrap.  Notes are
     * sorted by (onBeat, pitch) so an early-exit on onBeat >= localEnd
     * is safe. */
    for (const auto& n : *snap)
    {
        const double noteEnd = n.onBeat + n.lengthBeats;
        if (n.onBeat >= localEnd) break;
        if (noteEnd  <= localStart) continue;

        /* NoteOn lies in [localStart, localEnd) AND inside region. */
        if (n.onBeat >= localStart
            && n.onBeat <  localEnd
            && n.onBeat <  regionLen)
        {
            const double beatDelta = n.onBeat - localStart;
            int sampleOffset = (int) std::round (beatDelta * samplesPerBeat);
            sampleOffset = juce::jlimit (0, numSamples - 1, sampleOffset);
            out.addEvent (juce::MidiMessage::noteOn (
                               juce::jlimit (1, 16, n.channel),
                               juce::jlimit (0, 127, n.pitch),
                               (juce::uint8) juce::jlimit (1, 127, n.velocity)),
                          sampleOffset);
            held_.set ((std::size_t) heldIndex (n.channel, n.pitch));
        }

        /* NoteOff lies in [localStart, localEnd).  Clamp to regionLen
         * for non-looped regions so notes extending past the region
         * end fire their NoteOff at region end (not at the note's
         * raw end -- the region's container defines the playable
         * span). */
        const double offBeat = entry.looped
            ? noteEnd
            : juce::jmin (noteEnd, regionLen);
        if (offBeat > localStart && offBeat <= localEnd)
        {
            const double beatDelta = offBeat - localStart;
            int sampleOffset = (int) std::round (beatDelta * samplesPerBeat);
            sampleOffset = juce::jlimit (0, numSamples - 1, sampleOffset);
            out.addEvent (juce::MidiMessage::noteOff (
                               juce::jlimit (1, 16, n.channel),
                               juce::jlimit (0, 127, n.pitch)),
                          sampleOffset);
            held_.reset ((std::size_t) heldIndex (n.channel, n.pitch));
        }
    }
}

} // namespace element
