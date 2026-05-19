// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/tracker.hpp"

namespace element {

TrackerNode::TrackerNode()
    : MidiFilterNode (0)
{
    setName ("Tracker");
}

TrackerNode::~TrackerNode()
{
    if (mod_ != nullptr)
    {
        module_free (mod_);
        mod_ = nullptr;
    }
}

void TrackerNode::prepareToRender (double sampleRate, int maxBufferSize)
{
    juce::ScopedLock sl (engineLock_);

    currentSampleRate_ = sampleRate;
    currentBufferSize_ = maxBufferSize;
    sampleCounter_ = 0;

    if (mod_ == nullptr)
    {
        mod_ = module_new();
        installTestPattern();
    }

    mod_->clt->jack_sample_rate = (jack_nframes_t) sampleRate;
    mod_->clt->jack_buffer_size = (jack_nframes_t) maxBufferSize;
    mod_->clt->jack_last_frame = 0;

    /* No autoplay — Element's transport drives playback. render() reads
     * the playhead each block and gates module_play on transitions. */
    lastPlayingState_ = false;
}

void TrackerNode::releaseResources()
{
    juce::ScopedLock sl (engineLock_);
    if (mod_ != nullptr)
    {
        module_play (mod_, 0);
    }
}

void TrackerNode::render (RenderContext& rc)
{
    if (rc.midi.getNumBuffers() <= 0 || mod_ == nullptr)
        return;

    const int nsamples = rc.audio.getNumSamples();

    juce::ScopedLock sl (engineLock_);

    /* Consume Element's transport — playhead is set on this node by
     * GraphNode::setPlayHead() which delegates to the AudioEngine's
     * Transport (audioengine.cpp:911). */
    bool wantPlaying = lastPlayingState_;
    if (auto* const playhead = getPlayHead())
    {
        if (auto pos = playhead->getPosition())
        {
            wantPlaying = pos->getIsPlaying();
            if (auto bpm = pos->getBpm())
                mod_->bpm = (float) *bpm;
        }
    }

    if (wantPlaying != lastPlayingState_)
    {
        module_play (mod_, wantPlaying ? 1 : 0);
        lastPlayingState_ = wantPlaying;
    }

    /* Advance engine by one audio buffer. Frame counter is monotonic
     * sample count since prepareToRender; engine derives row positions
     * from (curr_frames - zero_time) / sample_rate. */
    const jack_nframes_t curr_frames = (jack_nframes_t) sampleCounter_;
    mod_->clt->jack_buffer_size = (jack_nframes_t) nsamples;

    module_advance (mod_, curr_frames);

    /* Drain queued + immediate output events into the per-port JUCE
     * MidiBuffers. Replaces vht's midi_buffer_flush + JACK output write. */
    drainEngineToMidi (rc, nsamples);

    mod_->clt->jack_last_frame = curr_frames + (jack_nframes_t) nsamples;
    sampleCounter_ += (juce::uint64) nsamples;
}

void TrackerNode::drainEngineToMidi (RenderContext& rc, int numSamples)
{
    const int nbuffers = rc.midi.getNumBuffers();

    midi_buff_excl_in (mod_->clt);

    /* First, merge any pending queued events into the main per-port
     * buffer (this is what midi_buffer_flush_port did inline). */
    for (int p = 0; p < MIDI_CLIENT_MAX_PORTS; ++p)
    {
        for (int q = 0; q < mod_->clt->curr_midi_queue_event[p]; ++q)
        {
            if (mod_->clt->curr_midi_event[p] < MIDI_EVT_BUFFER_LENGTH)
            {
                mod_->clt->midi_buffer[p][mod_->clt->curr_midi_event[p]++] =
                    mod_->clt->midi_queue_buffer[p][q];
            }
        }
        mod_->clt->curr_midi_queue_event[p] = 0;
    }

    /* Now emit each port's events into its mapped JUCE MidiBuffer.
     * Sort first so events land in time order. */
    for (int p = 0; p < MIDI_CLIENT_MAX_PORTS && p < nbuffers; ++p)
    {
        const int nevts = mod_->clt->curr_midi_event[p];
        if (nevts == 0)
            continue;

        if (nevts > 1)
        {
            qsort (mod_->clt->midi_buffer[p], (size_t) nevts,
                   sizeof (midi_event), midi_buffer_compare);
        }

        juce::MidiBuffer* const out = rc.midi.getWriteBuffer (p);
        if (out == nullptr)
            continue;

        for (int i = 0; i < nevts; ++i)
        {
            const midi_event& evt = mod_->clt->midi_buffer[p][i];
            int sampleOffset = (int) evt.time;
            if (sampleOffset < 0) sampleOffset = 0;
            if (sampleOffset >= numSamples) sampleOffset = numSamples - 1;

            switch (evt.type)
            {
                case note_on:
                    out->addEvent (juce::MidiMessage::noteOn (
                                       (int) evt.channel + 1, (int) evt.note,
                                       juce::uint8 (evt.velocity)),
                                   sampleOffset);
                    break;
                case note_off:
                    out->addEvent (juce::MidiMessage::noteOff (
                                       (int) evt.channel + 1, (int) evt.note),
                                   sampleOffset);
                    break;
                case control_change:
                    out->addEvent (juce::MidiMessage::controllerEvent (
                                       (int) evt.channel + 1, (int) evt.note,
                                       (int) evt.velocity),
                                   sampleOffset);
                    break;
                case pitch_wheel:
                    out->addEvent (juce::MidiMessage::pitchWheel (
                                       (int) evt.channel + 1,
                                       (int) evt.note | ((int) evt.velocity << 7)),
                                   sampleOffset);
                    break;
                case program_change:
                    out->addEvent (juce::MidiMessage::programChange (
                                       (int) evt.channel + 1, (int) evt.note),
                                   sampleOffset);
                    break;
                default:
                    break;
            }
        }
    }

    midi_buffer_clear (mod_->clt);
    midi_buff_excl_out (mod_->clt);
}

void TrackerNode::installTestPattern()
{
    /* 16-row × 2-track demo. Each track routes to its own JUCE MidiBuffer
     * output (track 0 → port 0, track 1 → port 1) so downstream nodes can
     * be wired independently in Element's graph.  Channels 1 & 2 also
     * differ in case a downstream node prefers channel-routing. */
    constexpr int kLen = 16;
    sequence* seq = sequence_new (kLen);

    /* Track 0 — port 0, MIDI channel 1, descending bass line. */
    track* trk0 = track_new (0, 0, kLen, kLen, TRACK_DEF_CTRLPR);
    track_set_row (trk0, 0,  0, 1, 36, 110, 0); // C2
    track_set_row (trk0, 0,  4, 1, 43, 100, 0); // G2
    track_set_row (trk0, 0,  8, 1, 41,  95, 0); // F2
    track_set_row (trk0, 0, 12, 1, 39,  90, 0); // D#2
    sequence_add_track (seq, trk0);

    /* Track 1 — port 1, MIDI channel 2, melodic 8th-note arp. */
    track* trk1 = track_new (1, 1, kLen, kLen, TRACK_DEF_CTRLPR);
    track_set_row (trk1, 0,  0, 1, 60, 100, 0); // C4
    track_set_row (trk1, 0,  2, 1, 64, 100, 0); // E4
    track_set_row (trk1, 0,  4, 1, 67, 100, 0); // G4
    track_set_row (trk1, 0,  6, 1, 72, 100, 0); // C5
    track_set_row (trk1, 0,  8, 1, 67, 100, 0); // G4
    track_set_row (trk1, 0, 10, 1, 64, 100, 0); // E4
    track_set_row (trk1, 0, 12, 1, 60, 100, 0); // C4
    track_set_row (trk1, 0, 14, 1, 64, 100, 0); // E4
    sequence_add_track (seq, trk1);

    module_add_sequence (mod_, seq);
    mod_->curr_seq = seq;
    mod_->bpm = 120.0f;

    /* sequence_new() defaults seq->playing = 0. Without this,
     * sequence_advance() runs but skips track_advance(), so no MIDI
     * is emitted even when module->playing is set. */
    sequence_set_playing (seq, 1);
}

void TrackerNode::refreshPorts()
{
    if (createdPorts_) return;

    PortList newPorts;
    /* 4 MIDI output ports — track N routes to port N for the first 4
     * tracks. Beyond that, tracks share port 3 (engine still emits on
     * the per-track port field). Element graph shows 4 distinct output
     * pins so each track can wire to its own downstream node per
     * DESIGN.md Option A. */
    int idx = 0;
    for (int p = 0; p < 4; ++p)
    {
        newPorts.add (PortType::Midi, idx++, p,
                      juce::String::formatted ("midi_out_%d", p),
                      juce::String::formatted ("Out %d", p + 1),
                      false);
    }
    createdPorts_ = true;
    setPorts (newPorts);
}

void TrackerNode::setState (const void* data, int size)
{
    (void) data; (void) size;
    /* State persistence comes in Phase 2 with the pattern editor. */
}

void TrackerNode::getState (juce::MemoryBlock& block)
{
    (void) block;
}

} // namespace element
