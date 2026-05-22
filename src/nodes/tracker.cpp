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
        installDefaultPattern();
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

    /* ---- Phase-4 launch scheduler: drain message-thread requests
     * and flip per-sequence playing flags BEFORE module_advance, so
     * sequences launched on this block's quant boundary start
     * emitting immediately rather than the block after. */
    double blockStartBeat = -1.0, blockEndBeat = -1.0;
    if (mod_->bpm > 0.0f)
    {
        if (auto* const ph = getPlayHead())
        {
            if (auto pos = ph->getPosition())
            {
                if (auto inSamples = pos->getTimeInSamples())
                {
                    const double samplesPerBeat = currentSampleRate_ * 60.0 / (double) mod_->bpm;
                    if (samplesPerBeat > 0.0)
                    {
                        blockStartBeat = (double) *inSamples / samplesPerBeat;
                        blockEndBeat   = blockStartBeat + (double) nsamples / samplesPerBeat;
                    }
                }
                else if (auto ppq = pos->getPpqPosition())
                {
                    const double samplesPerBeat = currentSampleRate_ * 60.0 / (double) mod_->bpm;
                    if (samplesPerBeat > 0.0)
                    {
                        blockStartBeat = *ppq;
                        blockEndBeat   = blockStartBeat + (double) nsamples / samplesPerBeat;
                    }
                }
            }
        }
    }
    drainLaunchFifo();
    applyPendingForBlock (blockStartBeat, blockEndBeat);

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

void TrackerNode::installDefaultPattern()
{
    /* Empty 16-row × 2-track default pattern, created on first
     * prepareToPlay when no saved state exists.  Both tracks emit on
     * the single MIDI output port and separate downstream by MIDI
     * channel (1 vs 2); a MidiChannelSplitter / MidiRouter fans them
     * out.  Pattern is rowless — the user fills it in. */
    constexpr int kLen = 16;
    sequence* seq = sequence_new (kLen);

    track* trk0 = track_new (0, 0, kLen, kLen, TRACK_DEF_CTRLPR);  /* ch 1 */
    sequence_add_track (seq, trk0);

    track* trk1 = track_new (0, 1, kLen, kLen, TRACK_DEF_CTRLPR);  /* ch 2 */
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

            /* Session-view user mute / solo intent.  Effective
             * Processor::isMuted is reconciled by SessionView from
             * these + every other tracker's solo state. */
            tree.setProperty ("userMuted", userMuted_, nullptr);
            tree.setProperty ("soloed",    soloed_,    nullptr);

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
                            if (row.fx[0] != 0)
                                { rowNode.setProperty ("f0", row.fx[0],      nullptr);
                                  rowNode.setProperty ("p0", row.fxParam[0], nullptr); }
                            if (row.fx[1] != 0)
                                { rowNode.setProperty ("f1", row.fx[1],      nullptr);
                                  rowNode.setProperty ("p1", row.fxParam[1], nullptr); }
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

void TrackerNode::advanceToPattern (int patternIdx)
{
    juce::ScopedLock sl (engineLock_);
    if (mod_ == nullptr) return;
    if (patternIdx < 0 || patternIdx >= mod_->nseq) return;
    mod_->pending_pattern_jump = patternIdx;
    mod_->pending_break_row = 0;
}

int TrackerNode::currentPatternIndex() const noexcept
{
    juce::ScopedLock sl (const_cast<juce::CriticalSection&> (engineLock_));
    if (mod_ == nullptr) return -1;
    for (int s = 0; s < mod_->nseq; ++s)
        if (mod_->seq[s] == mod_->curr_seq)
            return s;
    return -1;
}

int TrackerNode::numPatterns() const noexcept
{
    juce::ScopedLock sl (const_cast<juce::CriticalSection&> (engineLock_));
    return mod_ != nullptr ? mod_->nseq : 0;
}

/* === Session-view API ================================================== */

int TrackerNode::createSequence (int rowsLength)
{
    juce::ScopedLock sl (engineLock_);
    if (mod_ == nullptr) return -1;

    const int len = juce::jlimit (1, SEQUENCE_MAX_LENGTH, rowsLength);
    sequence* seq = sequence_new (len);
    if (seq == nullptr) return -1;

    /* One track per sequence — matches the "one clip = one track"
     * mapping the session-view design rests on (see §2.2 of
     * ~/wine-nspa-notes/session-view-design.md).  Multi-track clip
     * groups can be a later feature. */
    track* trk = track_new (0, 0, len, len, TRACK_DEF_CTRLPR);
    sequence_add_track (seq, trk);
    module_add_sequence (mod_, seq);

    /* sequence_new defaults playing=0; the clip launcher flips it
     * when the clip is banged.  Leave it untouched here. */
    return mod_->nseq - 1;
}

int TrackerNode::cloneSequence (int sourceIdx)
{
    juce::ScopedLock sl (engineLock_);
    if (mod_ == nullptr) return -1;
    if (sourceIdx < 0 || sourceIdx >= mod_->nseq) return -1;

    sequence* src = mod_->seq[sourceIdx];
    if (src == nullptr) return -1;

    sequence* clone = sequence_clone (src);
    if (clone == nullptr) return -1;

    /* Force playing=0 on the clone regardless of source state — the
     * clip launcher decides when it should fire, not the cloner. */
    sequence_set_playing (clone, 0);

    module_add_sequence (mod_, clone);
    return mod_->nseq - 1;
}

void TrackerNode::removeSequence (int sequenceIdx)
{
    juce::ScopedLock sl (engineLock_);
    if (mod_ == nullptr || mod_->nseq <= 1) return;
    if (sequenceIdx < 0 || sequenceIdx >= mod_->nseq) return;

    sequence* doomed = mod_->seq[sequenceIdx];
    const bool removingCurr = (mod_->curr_seq == doomed);

    /* Capture the replacement curr_seq pointer BEFORE module_del_sequence
     * runs — afterwards the splice has shifted everything and seq[1]
     * doesn't exist if we removed index 0.  After deletion, what was
     * seq[1] occupies seq[0]; the pointer we stored still resolves. */
    sequence* nextCurr = removingCurr
        ? mod_->seq[sequenceIdx == 0 ? 1 : 0]
        : nullptr;

    module_del_sequence (mod_, sequenceIdx);

    if (removingCurr)
        mod_->curr_seq = nextCurr;

    /* Keep wrap-detection cache aligned with vht's seq[] indexing. */
    if (sequenceIdx < lastSeqPos_.size())
        lastSeqPos_.remove (sequenceIdx);
}

void TrackerNode::setSequencePlaying (int sequenceIdx, bool on)
{
    juce::ScopedLock sl (engineLock_);
    if (mod_ == nullptr) return;
    if (sequenceIdx < 0 || sequenceIdx >= mod_->nseq) return;

    sequence* seq = mod_->seq[sequenceIdx];
    if (seq == nullptr) return;

    sequence_set_playing (seq, on ? 1 : 0);

    if (on)
    {
        /* Launch = rewind to row 0 (Ableton/Bitwig convention).  Reset
         * sequence pos + every track's pos so track_advance starts at
         * the top on the next module_advance pass. */
        seq->pos = 0.0;
        for (int t = 0; t < seq->ntrk; ++t)
            if (seq->trk[t] != nullptr)
                seq->trk[t]->pos = 0.0;
    }

    /* Reset wrap-cache for this index so the next wrap query sees the
     * fresh pos rather than reporting a phantom wrap from the previous
     * playthrough. */
    if (sequenceIdx < lastSeqPos_.size())
        lastSeqPos_.set (sequenceIdx, 0.0);
}

bool TrackerNode::isSequencePlaying (int sequenceIdx) const noexcept
{
    juce::ScopedLock sl (const_cast<juce::CriticalSection&> (engineLock_));
    if (mod_ == nullptr || sequenceIdx < 0 || sequenceIdx >= mod_->nseq) return false;
    const sequence* seq = mod_->seq[sequenceIdx];
    return seq != nullptr && seq->playing != 0;
}

double TrackerNode::getSequencePositionRows (int sequenceIdx) const noexcept
{
    juce::ScopedLock sl (const_cast<juce::CriticalSection&> (engineLock_));
    if (mod_ == nullptr || sequenceIdx < 0 || sequenceIdx >= mod_->nseq) return 0.0;
    const sequence* seq = mod_->seq[sequenceIdx];
    return seq != nullptr ? seq->pos : 0.0;
}

int TrackerNode::getSequenceLengthRows (int sequenceIdx) const noexcept
{
    juce::ScopedLock sl (const_cast<juce::CriticalSection&> (engineLock_));
    if (mod_ == nullptr || sequenceIdx < 0 || sequenceIdx >= mod_->nseq) return 0;
    const sequence* seq = mod_->seq[sequenceIdx];
    return seq != nullptr ? seq->length : 0;
}

/* === Audio-thread launch scheduler ===================================== */

void TrackerNode::schedulePlaying (int sequenceIdx, double beatTarget, bool wantPlaying) noexcept
{
    /* Single-writer (message thread) — no lock needed.  juce::AbstractFifo
     * gives us SPSC counters; we own the backing storage. */
    int start1, size1, start2, size2;
    launchFifo_.prepareToWrite (1, start1, size1, start2, size2);
    if (size1 + size2 < 1) return;   // FIFO full — drop silently

    const LaunchReq r { sequenceIdx, beatTarget, wantPlaying ? 1 : 0 };
    if (size1 > 0)
        launchFifoStorage_[(size_t) start1] = r;
    else
        launchFifoStorage_[(size_t) start2] = r;
    launchFifo_.finishedWrite (1);
}

void TrackerNode::drainLaunchFifo() noexcept
{
    const int ready = launchFifo_.getNumReady();
    if (ready == 0) return;

    int start1, size1, start2, size2;
    launchFifo_.prepareToRead (ready, start1, size1, start2, size2);

    auto apply = [this] (const LaunchReq& r) noexcept
    {
        if (r.sequenceIdx < 0) return;
        /* Grow pending array to cover this seqIdx.  Cheap; only on
         * first request after a new sequence is created. */
        while (pendingActions_.size() <= r.sequenceIdx)
            pendingActions_.add ({});
        auto& slot = pendingActions_.getReference (r.sequenceIdx);
        slot.beatTarget  = r.beatTarget;
        slot.wantPlaying = (r.wantPlaying != 0);
        slot.valid       = true;
    };

    for (int i = 0; i < size1; ++i)
        apply (launchFifoStorage_[(size_t) (start1 + i)]);
    for (int i = 0; i < size2; ++i)
        apply (launchFifoStorage_[(size_t) (start2 + i)]);

    launchFifo_.finishedRead (ready);
}

void TrackerNode::applyPendingForBlock (double blockStartBeat, double blockEndBeat) noexcept
{
    if (mod_ == nullptr) return;

    const int n = juce::jmin (pendingActions_.size(), mod_->nseq);
    for (int i = 0; i < n; ++i)
    {
        auto& p = pendingActions_.getReference (i);
        if (! p.valid) continue;

        /* Immediate (-ve target) fires now; quantised targets fire
         * inside the block that contains them.  If transport info
         * is unavailable (blockStartBeat<0) only immediate fires. */
        const bool inBlock = (p.beatTarget < 0.0)
                          || (blockStartBeat >= 0.0
                              && p.beatTarget >= blockStartBeat
                              && p.beatTarget <  blockEndBeat);
        if (! inBlock) continue;

        sequence* seq = mod_->seq[i];
        if (seq != nullptr)
        {
            sequence_set_playing (seq, p.wantPlaying ? 1 : 0);
            if (p.wantPlaying)
            {
                /* Rewind to row 0 on launch (Ableton/Bitwig convention). */
                seq->pos = 0.0;
                for (int t = 0; t < seq->ntrk; ++t)
                    if (seq->trk[t] != nullptr)
                        seq->trk[t]->pos = 0.0;
            }
        }

        if (i < lastSeqPos_.size())
            lastSeqPos_.set (i, 0.0);
        p.valid = false;
    }
}

bool TrackerNode::sequenceWrappedSinceLastQuery (int sequenceIdx) noexcept
{
    juce::ScopedLock sl (engineLock_);
    if (mod_ == nullptr || sequenceIdx < 0 || sequenceIdx >= mod_->nseq) return false;

    /* Lazy grow the per-sequence cache to current nseq.  Shrinks
     * happen in removeSequence; this only ever appends zeros. */
    while (lastSeqPos_.size() < mod_->nseq)
        lastSeqPos_.add (0.0);

    const sequence* seq = mod_->seq[sequenceIdx];
    if (seq == nullptr) return false;

    const double cur  = seq->pos;
    const double prev = lastSeqPos_.getUnchecked (sequenceIdx);
    lastSeqPos_.set (sequenceIdx, cur);

    /* sequence_advance resets pos to 0 at the end of a sequence (or
     * loop point).  Wrap = current pos strictly less than the prior
     * pos.  False on fresh launches where prev was reset to 0. */
    return cur < prev;
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

    /* Restore user mute / solo intent.  Missing props default to
     * false (older saves predate the field). */
    userMuted_ = (bool) tree.getProperty ("userMuted", false);
    soloed_    = (bool) tree.getProperty ("soloed",    false);

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
                {
                    track_set_row (trk, c, r, rtype, note, vel, 0);
                    trk->rows[c][r].fx[0]      = (int) rowNode.getProperty ("f0", 0);
                    trk->rows[c][r].fxParam[0] = (int) rowNode.getProperty ("p0", 0);
                    trk->rows[c][r].fx[1]      = (int) rowNode.getProperty ("f1", 0);
                    trk->rows[c][r].fxParam[1] = (int) rowNode.getProperty ("p1", 0);
                }
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
