// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "services/sources/midi_source.hpp"

#include <element/juce/core.hpp>

#include <algorithm>

namespace element {

namespace {

/* Walk all tracks of `mf` and sum the NoteOn count.  Used both as the
 * cached noteCount_ during construction and as a sanity-cross-check
 * by callers that want to verify a session-restored MidiSource. */
int countNoteOns (const juce::MidiFile& mf)
{
    int total = 0;
    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        const auto* seq = mf.getTrack (t);
        if (seq == nullptr) continue;
        for (int i = 0; i < seq->getNumEvents(); ++i)
        {
            const auto& msg = seq->getEventPointer (i)->message;
            if (msg.isNoteOn() && msg.getVelocity() > 0)
                ++total;
        }
    }
    return total;
}

/* Parse `bytes` into a juce::MidiFile.  Returns empty on failure --
 * callers must not assume successful parse. */
juce::MidiFile parseSmf (const juce::MemoryBlock& bytes)
{
    juce::MidiFile mf;
    if (bytes.getSize() == 0)
        return mf;
    juce::MemoryInputStream in (bytes.getData(), bytes.getSize(), false);
    if (! mf.readFrom (in))
        return juce::MidiFile();
    return mf;
}

} // namespace

//==============================================================================

MidiSource::MidiSource (juce::Uuid id,
                        const juce::File& originalPath,
                        juce::MemoryBlock smfBytes,
                        int noteCount)
    : Source (id),
      originalPath_ (originalPath),
      smfBytes_     (std::move (smfBytes))
{
    if (noteCount >= 0)
    {
        noteCount_ = noteCount;
    }
    else
    {
        noteCount_ = countNoteOns (parseSmf (smfBytes_));
    }
}

//==============================================================================

double MidiSource::durationBeats (double /*sampleRate*/, double /*bpm*/) const
{
    const auto mf = toMidiFile();
    const short timeFormat = mf.getTimeFormat();
    if (timeFormat <= 0)
    {
        /* SMPTE-coded MIDI file -- timecode-based timing.  Treat each
         * tick as a sub-quarter step; fallback heuristic since SMPTE
         * music files are rare. */
        return 0.0;
    }

    const double ppq = (double) timeFormat;
    double maxBeats = 0.0;
    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        const auto* seq = mf.getTrack (t);
        if (seq == nullptr) continue;
        if (seq->getNumEvents() <= 0) continue;
        const double endTick = seq->getEndTime();
        maxBeats = std::max (maxBeats, endTick / ppq);
    }
    return maxBeats;
}

juce::MidiFile MidiSource::toMidiFile() const
{
    return parseSmf (smfBytes_);
}

std::vector<MidiNote> MidiSource::extractNotes (const juce::MidiFile& mf)
{
    std::vector<MidiNote> notes;
    const short timeFormat = mf.getTimeFormat();
    /* PPQ-based timing.  juce::MidiFile::getTimeFormat() > 0 means
     * "pulses per quarter note"; negative is SMPTE -- treat ticks as
     * raw beats (pragmatic fallback). */
    const double ppq = (timeFormat > 0) ? (double) timeFormat : 1.0;

    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        const auto* seq = mf.getTrack (t);
        if (seq == nullptr) continue;

        const int n = seq->getNumEvents();
        for (int i = 0; i < n; ++i)
        {
            const auto& msg = seq->getEventPointer (i)->message;
            if (! msg.isNoteOn() || msg.getVelocity() == 0)
                continue;

            const double onBeat = msg.getTimeStamp() / ppq;
            double lengthBeats = 0.25; /* fallback default if no matching off */
            const int offIdx = seq->getIndexOfMatchingKeyUp (i);
            if (offIdx >= 0 && offIdx < n)
            {
                const auto& offMsg = seq->getEventPointer (offIdx)->message;
                const double offBeat = offMsg.getTimeStamp() / ppq;
                lengthBeats = std::max (0.0625, offBeat - onBeat); /* clamp to a 64th */
            }

            MidiNote note;
            note.pitch       = msg.getNoteNumber();
            note.velocity    = std::max (1, (int) msg.getVelocity());
            note.channel     = std::max (1, std::min (16, msg.getChannel()));
            note.onBeat      = onBeat;
            note.lengthBeats = lengthBeats;
            notes.push_back (note);
        }
    }
    return notes;
}

} // namespace element
