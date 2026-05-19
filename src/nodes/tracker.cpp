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
    bool wantRecording = false;
    if (auto* const playhead = getPlayHead())
    {
        if (auto pos = playhead->getPosition())
        {
            wantPlaying = pos->getIsPlaying();
            wantRecording = pos->getIsRecording();
            if (auto bpm = pos->getBpm())
                mod_->bpm = (float) *bpm;
        }
    }

    if (wantPlaying != lastPlayingState_)
    {
        module_play (mod_, wantPlaying ? 1 : 0);
        lastPlayingState_ = wantPlaying;
    }
    mod_->recording = wantRecording ? 1 : 0;

    /* Drain incoming MIDI from upstream (port 0 is our MIDI input).
     * Push events into clt->midi_in_buffer for the engine to consume in
     * module_advance — record / trigger / indicator paths run from
     * there. The buffer is then cleared so we can write our output. */
    if (rc.midi.getNumBuffers() > 0)
    {
        if (auto* in = rc.midi.getWriteBuffer (0))
        {
            for (const auto& metaEvent : *in)
            {
                const auto msg = metaEvent.getMessage();
                const auto* raw = msg.getRawData();
                const int rawLen = msg.getRawDataSize();
                if (rawLen <= 0) continue;
                midi_event mev = midi_decode_event (const_cast<unsigned char*> (raw), rawLen);
                mev.time = 0;
                midi_in_buffer_add (mod_->clt, mev);
            }
            in->clear();
        }
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
    /* 16-row × 2-track demo. Both tracks emit on the single MIDI output
     * port; they separate downstream by MIDI channel (1 vs 2). Use a
     * MidiChannelSplitter / MidiRouter to fan out per-channel. */
    constexpr int kLen = 16;
    sequence* seq = sequence_new (kLen);

    /* Track 0 — port 0, MIDI channel 1, descending bass line. */
    track* trk0 = track_new (0, 0, kLen, kLen, TRACK_DEF_CTRLPR);
    track_set_row (trk0, 0,  0, 1, 36, 110, 0); // C2
    track_set_row (trk0, 0,  4, 1, 43, 100, 0); // G2
    track_set_row (trk0, 0,  8, 1, 41,  95, 0); // F2
    track_set_row (trk0, 0, 12, 1, 39,  90, 0); // D#2
    sequence_add_track (seq, trk0);

    /* Track 1 — port 0 (shared), MIDI channel 2, melodic 8th-note arp. */
    track* trk1 = track_new (0, 1, kLen, kLen, TRACK_DEF_CTRLPR);
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
    /* 1 MIDI input (for live recording / pattern triggers) +
     * 1 MIDI output (channel-multiplexed per track). */
    newPorts.add (PortType::Midi, 0, 0, "midi_in",  "MIDI In",  true);
    newPorts.add (PortType::Midi, 1, 0, "midi_out", "MIDI Out", false);
    createdPorts_ = true;
    setPorts (newPorts);
}

void TrackerNode::getState (juce::MemoryBlock& block)
{
    juce::ValueTree tree ("tracker");

    {
        juce::ScopedLock sl (engineLock_);
        if (mod_ != nullptr)
        {
            tree.setProperty ("bpm", (double) mod_->bpm, nullptr);

            /* Current pattern index — used to restore the active
             * pattern on load. */
            int currIdx = 0;
            for (int i = 0; i < mod_->nseq; ++i)
                if (mod_->seq[i] == mod_->curr_seq) { currIdx = i; break; }
            tree.setProperty ("currentPattern", currIdx, nullptr);

            for (int p = 0; p < mod_->nseq; ++p)
            {
                auto* seq = mod_->seq[p];
                if (! seq) continue;

                juce::ValueTree pat ("pattern");
                pat.setProperty ("length", seq->length, nullptr);
                pat.setProperty ("rpb",    seq->rpb,    nullptr);

                for (int t = 0; t < seq->ntrk; ++t)
                {
                    auto* trk = seq->trk[t];
                    if (! trk) continue;

                    juce::ValueTree tt ("track");
                    tt.setProperty ("port",    trk->port,    nullptr);
                    tt.setProperty ("channel", trk->channel, nullptr);
                    tt.setProperty ("ncols",   trk->ncols,   nullptr);
                    tt.setProperty ("muted",   trk->playing == 0, nullptr);

                    for (int c = 0; c < trk->ncols; ++c)
                    {
                        for (int r = 0; r < seq->length; ++r)
                        {
                            const auto& row = trk->rows[c][r];
                            if (row.type == 0) continue;

                            juce::ValueTree rowNode ("row");
                            rowNode.setProperty ("c", c,             nullptr);
                            rowNode.setProperty ("r", r,             nullptr);
                            rowNode.setProperty ("t", row.type,      nullptr);
                            rowNode.setProperty ("n", row.note,      nullptr);
                            rowNode.setProperty ("v", row.velocity,  nullptr);
                            tt.appendChild (rowNode, nullptr);
                        }
                    }
                    pat.appendChild (tt, nullptr);
                }
                tree.appendChild (pat, nullptr);
            }
        }
    }

    juce::MemoryOutputStream stream (block, false);
    {
        juce::GZIPCompressorOutputStream gzip (stream);
        tree.writeToStream (gzip);
    }
}

/* === Undo / redo via state-memento ====================================== */

void TrackerNode::pushUndo()
{
    juce::MemoryBlock block;
    getState (block);
    if (block.getSize() == 0) return;

    if (undoStack_.size() >= (int) kMaxUndo)
        undoStack_.remove (0);
    undoStack_.add (block);
    redoStack_.clearQuick();
}

bool TrackerNode::canUndo() const noexcept { return ! undoStack_.isEmpty(); }
bool TrackerNode::canRedo() const noexcept { return ! redoStack_.isEmpty(); }

void TrackerNode::undo()
{
    if (undoStack_.isEmpty()) return;

    juce::MemoryBlock current;
    getState (current);
    redoStack_.add (current);
    if (redoStack_.size() > (int) kMaxUndo)
        redoStack_.remove (0);

    auto target = undoStack_.removeAndReturn (undoStack_.size() - 1);
    setState (target.getData(), (int) target.getSize());
}

void TrackerNode::redo()
{
    if (redoStack_.isEmpty()) return;

    juce::MemoryBlock current;
    getState (current);
    undoStack_.add (current);
    if (undoStack_.size() > (int) kMaxUndo)
        undoStack_.remove (0);

    auto target = redoStack_.removeAndReturn (redoStack_.size() - 1);
    setState (target.getData(), (int) target.getSize());
}

void TrackerNode::clearUndoHistory()
{
    undoStack_.clearQuick();
    redoStack_.clearQuick();
}

/* ===================================================================== */

void TrackerNode::setState (const void* data, int size)
{
    if (data == nullptr || size <= 0) return;

    const auto tree = juce::ValueTree::readFromGZIPData (data, (size_t) size);
    if (! tree.isValid() || tree.getType() != juce::Identifier ("tracker"))
        return;

    juce::ScopedLock sl (engineLock_);

    /* Tear down current module + rebuild from saved state. */
    if (mod_ != nullptr)
    {
        module_play (mod_, 0);
        module_free (mod_);
        mod_ = nullptr;
    }

    mod_ = module_new();
    if (mod_ == nullptr) return;

    const float bpm = (float) (double) tree.getProperty ("bpm", 120.0);
    const int   currIdx = (int) tree.getProperty ("currentPattern", 0);

    /* Walk pattern children. Backwards-compat: if the tree has 'track'
     * children directly (old single-pattern format) fall through to a
     * legacy path. */
    bool sawPattern = false;
    for (int p = 0; p < tree.getNumChildren(); ++p)
    {
        const auto pat = tree.getChild (p);
        if (pat.getType() != juce::Identifier ("pattern")) continue;
        sawPattern = true;

        const int length = (int) pat.getProperty ("length", 16);
        const int rpb    = (int) pat.getProperty ("rpb",    4);

        sequence* seq = sequence_new (length);
        seq->rpb = rpb;

        for (int i = 0; i < pat.getNumChildren(); ++i)
        {
            const auto tt = pat.getChild (i);
            if (tt.getType() != juce::Identifier ("track")) continue;

            const int port    = (int) tt.getProperty ("port",    0);
            const int channel = (int) tt.getProperty ("channel", 0);
            const bool muted  = (bool) tt.getProperty ("muted", false);

            track* trk = track_new (port, channel, length, length, TRACK_DEF_CTRLPR);

            for (int j = 0; j < tt.getNumChildren(); ++j)
            {
                const auto rowNode = tt.getChild (j);
                if (rowNode.getType() != juce::Identifier ("row")) continue;

                const int c     = (int) rowNode.getProperty ("c", 0);
                const int r     = (int) rowNode.getProperty ("r", 0);
                const int rtype = (int) rowNode.getProperty ("t", 0);
                const int note  = (int) rowNode.getProperty ("n", 0);
                const int vel   = (int) rowNode.getProperty ("v", 100);

                if (r >= 0 && r < length)
                    track_set_row (trk, c, r, rtype, note, vel, 0);
            }
            if (muted) trk->playing = 0;
            sequence_add_track (seq, trk);
        }
        module_add_sequence (mod_, seq);
    }

    /* Legacy single-pattern format support — load 'track' children
     * directly under root into one pattern. */
    if (! sawPattern)
    {
        const int length = (int) tree.getProperty ("length", 16);
        const int rpb    = (int) tree.getProperty ("rpb",    4);
        sequence* seq = sequence_new (length);
        seq->rpb = rpb;
        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            const auto tt = tree.getChild (i);
            if (tt.getType() != juce::Identifier ("track")) continue;
            const int port    = (int) tt.getProperty ("port",    0);
            const int channel = (int) tt.getProperty ("channel", 0);
            track* trk = track_new (port, channel, length, length, TRACK_DEF_CTRLPR);
            for (int j = 0; j < tt.getNumChildren(); ++j)
            {
                const auto rowNode = tt.getChild (j);
                if (rowNode.getType() != juce::Identifier ("row")) continue;
                const int c = (int) rowNode.getProperty ("c", 0);
                const int r = (int) rowNode.getProperty ("r", 0);
                if (r < 0 || r >= length) continue;
                track_set_row (trk, c, r,
                               (int) rowNode.getProperty ("t", 0),
                               (int) rowNode.getProperty ("n", 0),
                               (int) rowNode.getProperty ("v", 100), 0);
            }
            sequence_add_track (seq, trk);
        }
        module_add_sequence (mod_, seq);
    }

    /* Activate curr_seq, pause others (one-pattern-at-a-time semantics). */
    if (mod_->nseq > 0)
    {
        const int idx = juce::jlimit (0, mod_->nseq - 1, currIdx);
        for (int i = 0; i < mod_->nseq; ++i)
            sequence_set_playing (mod_->seq[i], 0);
        mod_->curr_seq = mod_->seq[idx];
        sequence_set_playing (mod_->curr_seq, 1);
    }
    mod_->bpm = bpm;

    mod_->clt->jack_sample_rate = (jack_nframes_t) currentSampleRate_;
    mod_->clt->jack_buffer_size = (jack_nframes_t) currentBufferSize_;
    mod_->clt->jack_last_frame  = 0;
    sampleCounter_     = 0;
    lastPlayingState_  = false;
}

} // namespace element
