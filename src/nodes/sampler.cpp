// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/sampler.hpp"
#include "services/diskopservice.hpp"

/* Vendored ft2-clone mixer (BSD-3-Clause). See src/engine/sampler/. */
extern "C" {
#include "engine/sampler/ft2_audio.h"
#include "engine/sampler/ft2_mix.h"
#include "engine/sampler/ft2_mix_interpolation.h"
}

namespace element {

namespace {

/* mixFunc indices: layout in ft2_mix.c bottom of file:
 *   no-ramp 8-bit:   0..14  (NoLoop/Loop/Pingpong) × (None,S8,Lin,S16,Cubic)
 *   no-ramp 16-bit: 15..29  same
 *   ramped  8-bit:  30..44
 *   ramped 16-bit:  45..59
 * Within a (bit-depth, ramp) row of 15: per (interp 0..4) × (loop 0..2).
 * Layout per interp: NoLoop, Loop, Pingpong. */
constexpr int kBase16NoRamp = 15;
constexpr int kBase16Ramped = 45;

constexpr int interpOffset (int interpMode)
{
    switch (interpMode)
    {
        case 0: return  0;
        case 1: return  6;
        case 2: return 12;
        case 3: return  9;
        default: return 0;
    }
}

std::unique_ptr<int16_t[]> convertFloatToInt16 (const float* src, int n)
{
    std::unique_ptr<int16_t[]> out (new int16_t[(size_t) n]);
    for (int i = 0; i < n; ++i)
    {
        float v = src[i] * 32767.0f;
        if (v >  32767.f) v =  32767.f;
        if (v < -32767.f) v = -32767.f;
        out[(size_t) i] = (int16_t) v;
    }
    return out;
}

void ensureMixerInterpolationTablesReady()
{
    static std::once_flag flag;
    std::call_once (flag, [] { setupMixerInterpolationTables(); });
}

/* FT2 auto-vibrato sine table (256 entries, -64..+64).  Sourced from
 * ft2_tables.c::autoVibSineTab.  Used by AutoVibrato pitch modulation. */
static const int8_t kAutoVibSineTab[256] = {
      0,   2,   3,   5,   6,   8,   9,  11,  12,  14,  16,  17,  19,  20,  22,  23,
     24,  26,  27,  29,  30,  32,  33,  34,  36,  37,  38,  39,  41,  42,  43,  44,
     45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  56,  57,  58,  59,
     59,  60,  60,  61,  61,  62,  62,  62,  63,  63,  63,  64,  64,  64,  64,  64,
     64,  64,  64,  64,  64,  64,  63,  63,  63,  62,  62,  62,  61,  61,  60,  60,
     59,  59,  58,  57,  56,  56,  55,  54,  53,  52,  51,  50,  49,  48,  47,  46,
     45,  44,  43,  42,  41,  39,  38,  37,  36,  34,  33,  32,  30,  29,  27,  26,
     24,  23,  22,  20,  19,  17,  16,  14,  12,  11,   9,   8,   6,   5,   3,   2,
      0,  -2,  -3,  -5,  -6,  -8,  -9, -11, -12, -14, -16, -17, -19, -20, -22, -23,
    -24, -26, -27, -29, -30, -32, -33, -34, -36, -37, -38, -39, -41, -42, -43, -44,
    -45, -46, -47, -48, -49, -50, -51, -52, -53, -54, -55, -56, -56, -57, -58, -59,
    -59, -60, -60, -61, -61, -62, -62, -62, -63, -63, -63, -64, -64, -64, -64, -64,
    -64, -64, -64, -64, -64, -64, -63, -63, -63, -62, -62, -62, -61, -61, -60, -60,
    -59, -59, -58, -57, -56, -56, -55, -54, -53, -52, -51, -50, -49, -48, -47, -46,
    -45, -44, -43, -42, -41, -39, -38, -37, -36, -34, -33, -32, -30, -29, -27, -26,
    -24, -23, -22, -20, -19, -17, -16, -14, -12, -11,  -9,  -8,  -6,  -5,  -3,  -2
};

} // anonymous namespace


/* ===========================================================================
 * SamplerInstrument
 * ========================================================================*/

SamplerInstrument::SamplerInstrument()
{
    for (auto& s : slots) s.reset();

    /* Default envelopes: disabled.  Voices fall back to SamplerNode's
     * global ADSR until the user enables the FT2 envelope.  A minimal
     * 2-point full-volume shape is preloaded so that "enable" produces
     * audible output immediately rather than silence. */
    volumeEnv.length = 2;
    volumeEnv.points[0] = { 0,   64 };
    volumeEnv.points[1] = { 50,  64 };
    volumeEnv.sustainPoint = 0;
    volumeEnv.loopStart = 0;
    volumeEnv.loopEnd = 1;
    volumeEnv.flags = 0;

    panEnv.length = 2;
    panEnv.points[0] = { 0,   32 };
    panEnv.points[1] = { 50,  32 };
    panEnv.sustainPoint = 0;
    panEnv.loopStart = 0;
    panEnv.loopEnd = 1;
    panEnv.flags = 0;
}

bool SamplerInstrument::loadSampleToSlot (int slot, const File& file,
                                          AudioFormatManager& fmt)
{
    if (slot < 0 || slot >= kNumSlots) return false;
    if (! file.existsAsFile()) return false;

    std::unique_ptr<AudioFormatReader> reader (fmt.createReaderFor (file));
    if (reader == nullptr) return false;

    const int64_t maxLen = (int64_t) 10 * 60 * (int64_t) reader->sampleRate;
    const int n = (int) std::min ((int64_t) reader->lengthInSamples, maxLen);
    const bool stereo = reader->numChannels >= 2;

    AudioBuffer<float> tmp ((int) reader->numChannels, n);
    reader->read (&tmp, 0, n, 0, true, true);

    auto s = std::make_unique<SamplerSampleSlot>();
    s->name             = file.getFileNameWithoutExtension();
    s->numSamples       = n;
    s->sourceSampleRate = reader->sampleRate;
    s->rootNote         = 60;
    s->relativeNote     = 0;
    s->isStereo         = stereo;
    s->data16L          = convertFloatToInt16 (tmp.getReadPointer (0), n);
    if (stereo)
        s->data16R = convertFloatToInt16 (tmp.getReadPointer (1), n);

    slots[(size_t) slot] = std::move (s);
    if (! keymapUserModified) autoSpreadKeymap();
    return true;
}

void SamplerInstrument::clearSlot (int slot)
{
    if (slot < 0 || slot >= kNumSlots) return;
    slots[(size_t) slot].reset();
    if (! keymapUserModified) autoSpreadKeymap();
}

int SamplerInstrument::slotForNote (int midiNote) const noexcept
{
    if (midiNote < 0 || midiNote > 127) return -1;
    return (int) noteToSlot[midiNote];
}

void SamplerInstrument::setSlotForNote (int midiNote, int slot)
{
    if (midiNote < 0 || midiNote > 127) return;
    noteToSlot[midiNote] = (uint8_t) jlimit (0, kNumSlots - 1, slot);
    keymapUserModified = true;
}

void SamplerInstrument::autoSpreadKeymap()
{
    Array<int> loadedSlots;
    for (int i = 0; i < kNumSlots; ++i)
        if (slots[(size_t) i] != nullptr) loadedSlots.add (i);

    if (loadedSlots.isEmpty())
    {
        for (int n = 0; n < 128; ++n) noteToSlot[n] = 0;
        return;
    }

    const int n = loadedSlots.size();
    for (int note = 0; note < 128; ++note)
    {
        const int idx = (note * n) / 128;
        noteToSlot[note] = (uint8_t) loadedSlots[idx];
    }
}

SamplerSampleSlot*       SamplerInstrument::getSlot (int slot)
    { return (slot >= 0 && slot < kNumSlots) ? slots[(size_t) slot].get() : nullptr; }
const SamplerSampleSlot* SamplerInstrument::getSlot (int slot) const
    { return (slot >= 0 && slot < kNumSlots) ? slots[(size_t) slot].get() : nullptr; }

int SamplerInstrument::firstLoadedSlot() const noexcept
{
    for (int i = 0; i < kNumSlots; ++i)
        if (slots[(size_t) i] != nullptr) return i;
    return -1;
}

int SamplerInstrument::numLoaded() const noexcept
{
    int c = 0;
    for (const auto& s : slots) if (s != nullptr) ++c;
    return c;
}


/* ===========================================================================
 * Ft2SamplerSound — JUCE shim that holds a back-reference to SamplerNode
 * so voices can look up the instrument bound to the playing MIDI channel
 * at note-on time.  Multi-instrument routing point.
 * ========================================================================*/
class Ft2SamplerSound : public SynthesiserSound
{
public:
    explicit Ft2SamplerSound (SamplerNode& n) : node (n) {}
    bool appliesToNote    (int) override { return true; }
    bool appliesToChannel (int) override { return true; }
    SamplerNode& getNode() const { return node; }
private:
    SamplerNode& node;
};


/* ===========================================================================
 * ChannelTrackingSynth — JUCE's SynthesiserVoice doesn't expose the
 * playing MIDI channel, but we need it for multi-instrument routing.
 * Forward the channel to the owning SamplerNode before invoking noteOn,
 * where voices read it back during startNote().
 * ========================================================================*/
class ChannelTrackingSynth : public Synthesiser
{
public:
    explicit ChannelTrackingSynth (SamplerNode& owner) : node (owner) {}
    void noteOn (int midiChannel, int midiNoteNumber, float velocity) override
    {
        node.setLastNoteChannel (midiChannel);
        Synthesiser::noteOn (midiChannel, midiNoteNumber, velocity);
    }
    /* JUCE's Synthesiser swallows program-change events without exposing
     * an override hook — we run our own MIDI scan in SamplerNode::processBlock
     * before renderNextBlock to handle them.  See SamplerNode::processBlock. */
private:
    SamplerNode& node;
};


/* ===========================================================================
 * Ft2SamplerVoice — ft2 mixer + FT2 vol/pan envelopes + fadeout + autoVib.
 *
 * Tick model: every kSamplesPerEnvTick output samples we advance the
 * envelope state by one FT2 tick (50Hz nominal).  Within a render block
 * we process [up to] one tick at a time, splitting numSamples at tick
 * boundaries.  Envelope output multiplies into voice gain; fadeout
 * multiplies on top once keyOff is engaged; autoVib modulates voice.delta
 * before each tick segment is rendered.
 * ========================================================================*/
class Ft2SamplerVoice : public SynthesiserVoice
{
public:
    explicit Ft2SamplerVoice (SamplerNode& owner_) : owner (owner_) {}

    bool canPlaySound (SynthesiserSound* s) override
    {
        return dynamic_cast<Ft2SamplerSound*> (s) != nullptr;
    }

    int  getCurrentSamplePos() const noexcept { return voice.position; }
    bool isPlayingActive() const noexcept     { return voice.active; }
    const SamplerSampleSlot* getCurrentSlot() const noexcept { return slotPtr; }

    void startNote (int midiNote, float velocity,
                    SynthesiserSound* sound, int /*pitchWheel*/) override
    {
        auto* s = dynamic_cast<Ft2SamplerSound*> (sound);
        if (s == nullptr) { clearCurrentNote(); return; }

        const int chan = juce::jlimit (1, 16, s->getNode().getLastNoteChannel());
        instrument = s->getNode().getInstrumentForChannel (chan);
        if (instrument == nullptr) { clearCurrentNote(); return; }

        const int slotIdx = instrument->slotForNote (midiNote);
        const auto* slot = instrument->getSlot (slotIdx);
        if (slot == nullptr || ! slot->isLoaded()) { clearCurrentNote(); return; }

        slotPtr      = slot;
        slotIsStereo = slot->isStereo && slot->data16R != nullptr;
        playedNote   = midiNote;
        velocityLin  = juce::jlimit (0.0f, 1.0f, velocity);

        const bool loopRequested = (slot->loopMode == SamplerLoopMode::kForward)
                                || (slot->loopMode == SamplerLoopMode::kPingpong);
        const bool loopActive = loopRequested
                               && slot->loopLength > 0
                               && slot->loopStart >= 0
                               && slot->loopStart < slot->numSamples;
        const bool loop     = loopActive && slot->loopMode == SamplerLoopMode::kForward;
        const bool pingpong = loopActive && slot->loopMode == SamplerLoopMode::kPingpong;

        voice = {};
        voice.base16     = slot->data16L.get();
        voice.sampleEnd  = slot->numSamples;
        if (loopActive)
        {
            voice.loopStart  = jlimit (0, slot->numSamples - 1, slot->loopStart);
            voice.loopLength = jlimit (1, slot->numSamples - voice.loopStart,
                                         slot->loopLength);
            voice.sampleEnd  = voice.loopStart + voice.loopLength;
            voice.loopType   = pingpong ? 2 : 1;
        }
        voice.position   = 0;
        voice.positionFrac = 0;
        voice.active     = true;
        voice.mixFuncOffset = (uint8_t) owner.getMixFuncIndexForCurrentMode (loop, pingpong);

        /* Pitch: 12tet semitones + slot finetune + slot relativeNote. */
        const double semis    = (double) (midiNote - slot->rootNote)
                              + (double) slot->relativeNote
                              + (double) slot->finetune / 128.0;
        basePitchMul = std::pow (2.0, semis / 12.0);
        setVoiceDelta (basePitchMul);

        const float gain = velocityLin * slot->volume;
        const float pan  = jlimit (0.0f, 1.0f, slot->panning);
        voice.fVolume = gain;
        slotPan = pan;
        setVoiceGain (gain, pan);

        /* Reset FT2 envelope + fadeout + autoVib state. */
        keyOff = false;
        volEnvPos = 0;  volEnvTick = 0;  volEnvDelta = 0;
        panEnvPos = 0;  panEnvTick = 0;  panEnvDelta = 0;
        volEnvValue = 0;
        panEnvValue = 0;
        fadeoutVol = 32768;
        autoVibPos = 0;  autoVibAmp = 0;  autoVibSweepCounter = 0;
        if (instrument != nullptr && instrument->autoVib.depth > 0)
        {
            if (instrument->autoVib.sweep > 0)
            {
                autoVibAmp = 0;
                autoVibSweepCounter = (instrument->autoVib.depth << 8) / instrument->autoVib.sweep;
            }
            else
            {
                autoVibAmp = (uint16_t) (instrument->autoVib.depth << 8);
                autoVibSweepCounter = 0;
            }
        }
        envSampleAccum = 0;

        /* FT2 envelope path uses sample-position-driven envelopes; the
         * fallback ADSR is only used when both vol+pan envelopes are
         * disabled. */
        const auto a = owner.getAdsr();
        ADSR::Parameters p { a.attack, a.decay, a.sustain, a.release };
        adsr.setSampleRate (getSampleRate());
        adsr.setParameters (p);
        adsr.reset();
        adsr.noteOn();
    }

    void stopNote (float /*velocity*/, bool allowTailOff) override
    {
        if (allowTailOff)
        {
            keyOff = true;
            adsr.noteOff();
            /* If no fadeout configured AND no env sustain, end voice when
             * sample / ADSR completes naturally. */
        }
        else
        {
            adsr.reset();
            voice.active = false;
            clearCurrentNote();
        }
    }

    void pitchWheelMoved (int)            override {}
    void controllerMoved (int, int)       override {}

    void renderNextBlock (AudioBuffer<float>& out,
                          int startSample, int numSamples) override
    {
        if (! voice.active && ! adsr.isActive()) return;
        if (out.getNumChannels() < 2)            return;
        if (numSamples <= 0)                     return;

        const int samplesPerTick = owner.getSamplesPerEnvTick();

        AudioBuffer<float> scratch (2, numSamples);
        scratch.clear();

        audio.fMixBufferL = scratch.getWritePointer (0);
        audio.fMixBufferR = scratch.getWritePointer (1);

        /* Walk the block in tick-aligned chunks.  Each chunk: tick
         * envelope state once (if a tick boundary fell inside), then
         * mix sample. */
        int pos = 0;
        while (pos < numSamples && voice.active)
        {
            const int chunk = juce::jmin (numSamples - pos,
                                          samplesPerTick - envSampleAccum);
            if (chunk <= 0) { ++envSampleAccum; continue; }

            mixChunk (scratch, pos, chunk);
            pos += chunk;
            envSampleAccum += chunk;
            if (envSampleAccum >= samplesPerTick)
            {
                envSampleAccum -= samplesPerTick;
                tickEnvelopes();
                /* Refresh voice volumes/pitch from envelope outputs. */
                applyEnvelopeToVoice();
            }
        }

        /* Mix scratch → out with the JUCE fallback ADSR multiplier (only
         * active when neither envelope is enabled — in that case the FT2
         * envelope tickers leave fEnvVolMul at 1.0 and we let the ADSR
         * shape the tail). */
        auto* outL = out.getWritePointer (0) + startSample;
        auto* outR = out.getWritePointer (1) + startSample;
        const auto* sL = scratch.getReadPointer (0);
        const auto* sR = scratch.getReadPointer (1);

        const bool envEnabled = (instrument != nullptr)
                              && (instrument->volumeEnv.flags & FT2Envelope::kEnabled);

        if (envEnabled)
        {
            /* FT2 envelope path: scratch already carries env-modulated
             * volume + fadeout via setVoiceGain on each tick. */
            for (int i = 0; i < numSamples; ++i)
            {
                outL[i] += sL[i];
                outR[i] += sR[i];
            }
        }
        else
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float env = adsr.getNextSample();
                outL[i] += sL[i] * env;
                outR[i] += sR[i] * env;
            }
        }

        /* End-of-voice gate: FT2 path ends when fadeoutVol falls to 0
         * (post-keyOff) OR when sample ends.  Non-env path: ADSR done. */
        if (envEnabled)
        {
            if (! voice.active || (keyOff && fadeoutVol == 0
                                   && instrument->fadeoutRate > 0))
            {
                voice.active = false;
                clearCurrentNote();
            }
        }
        else
        {
            if (! voice.active || ! adsr.isActive())
            {
                voice.active = false;
                clearCurrentNote();
            }
        }
    }

private:
    /* === per-tick chunk mixer.  No envelope arithmetic here — only the
     *     ft2 voice mixer kernels. ============================================ */
    void mixChunk (AudioBuffer<float>& scratch, int startInScratch, int count)
    {
        if (count <= 0 || slotPtr == nullptr) return;
        const uint32_t soff = (uint32_t) startInScratch;
        const uint32_t cnt  = (uint32_t) count;

        if (slotIsStereo)
        {
            voice_t snap = voice;
            voice.fCurrVolumeL   = gLL;
            voice.fCurrVolumeR   = gLR;
            voice.fTargetVolumeL = gLL;
            voice.fTargetVolumeR = gLR;
            voice.base16 = slotPtr->data16L.get();
            mixFuncTab[voice.mixFuncOffset] (&voice, soff, cnt);

            voice_t afterFirst = voice;

            voice = snap;
            voice.fCurrVolumeL   = gRL;
            voice.fCurrVolumeR   = gRR;
            voice.fTargetVolumeL = gRL;
            voice.fTargetVolumeR = gRR;
            voice.base16 = slotPtr->data16R.get();
            mixFuncTab[voice.mixFuncOffset] (&voice, soff, cnt);

            voice.active = voice.active && afterFirst.active;
        }
        else
        {
            mixFuncTab[voice.mixFuncOffset] (&voice, soff, cnt);
        }
    }

    /* === FT2 envelope tick (port of updateVolPanAutoVib). =================== */
    void tickEnvelopes()
    {
        if (instrument == nullptr) return;

        /* fadeout on keyOff */
        if (keyOff && instrument->fadeoutRate > 0)
        {
            if (instrument->fadeoutRate > fadeoutVol) fadeoutVol = 0;
            else                                      fadeoutVol -= instrument->fadeoutRate;
        }

        advanceEnvelope (instrument->volumeEnv, volEnvPos, volEnvTick,
                         volEnvValue, volEnvDelta);
        advanceEnvelope (instrument->panEnv,    panEnvPos, panEnvTick,
                         panEnvValue, panEnvDelta);

        /* autoVibrato sweep ramp-in */
        if (instrument->autoVib.depth > 0 && autoVibSweepCounter > 0 && ! keyOff)
        {
            uint32_t amp = (uint32_t) autoVibAmp + autoVibSweepCounter;
            const uint32_t cap = (uint32_t) instrument->autoVib.depth << 8;
            if (amp >= cap) { autoVibAmp = (uint16_t) cap; autoVibSweepCounter = 0; }
            else            { autoVibAmp = (uint16_t) amp; }
        }
        autoVibPos = (uint8_t) (autoVibPos + instrument->autoVib.rate);
    }

    /* Advance an envelope state by one FT2 tick.  Mirrors the per-tick
     * branch of updateVolPanAutoVib (sustain hold + loop + linear segment
     * interpolation). */
    void advanceEnvelope (const FT2Envelope& env, uint8_t& pos, uint16_t& tick,
                          int16_t& value, int16_t& delta)
    {
        if (! (env.flags & FT2Envelope::kEnabled)) return;
        if (env.length == 0) return;

        ++tick;

        if (pos < env.length && tick == (uint16_t) env.points[pos].x)
        {
            value = (int16_t) ((int8_t) env.points[pos].y << 8);
            uint8_t newPos = (uint8_t) (pos + 1);

            if (env.flags & FT2Envelope::kLoop)
            {
                --newPos;
                if (newPos == env.loopEnd)
                {
                    const bool holdAtSustain = (env.flags & FT2Envelope::kSustain)
                                            && newPos == env.sustainPoint
                                            && ! keyOff;
                    if (! holdAtSustain)
                    {
                        newPos = env.loopStart;
                        tick   = (uint16_t) env.points[newPos].x;
                        value  = (int16_t) ((int8_t) env.points[newPos].y << 8);
                    }
                }
                ++newPos;
            }

            if (newPos < env.length)
            {
                bool interp = true;
                if ((env.flags & FT2Envelope::kSustain) && ! keyOff)
                {
                    if ((uint8_t) (newPos - 1) == env.sustainPoint)
                    {
                        --newPos;
                        delta = 0;
                        interp = false;
                    }
                }

                if (interp)
                {
                    pos = newPos;
                    const int16_t x0 = env.points[newPos - 1].x;
                    const int16_t x1 = env.points[newPos].x;
                    const int16_t xDiff = x1 - x0;
                    if (xDiff > 0)
                    {
                        const int16_t y0 = env.points[newPos - 1].y;
                        const int16_t y1 = env.points[newPos].y;
                        const int8_t yDiff = (int8_t) (y1 - y0);
                        delta = (int16_t) ((yDiff << 8) / xDiff);
                    }
                    else
                    {
                        delta = 0;
                    }
                }
            }
            else
            {
                delta = 0;
            }
            return;
        }

        /* Inter-point linear interpolation. */
        value = (int16_t) (value + delta);
        const uint8_t hi = (uint8_t) (value >> 8);
        if (hi > 64)
        {
            value = (hi <= 160) ? (int16_t) (64 * 256) : 0;
            delta = 0;
        }
    }

    /** Combine velocity / slot vol / FT2 vol-env / fadeout into voice
     *  gain.  Re-derive panning from FT2 pan-env.  Apply autoVib pitch
     *  modulation to voice.delta. */
    void applyEnvelopeToVoice()
    {
        if (slotPtr == nullptr || instrument == nullptr) return;

        const FT2Envelope& ve = instrument->volumeEnv;
        const FT2Envelope& pe = instrument->panEnv;

        float fEnvVol = 1.0f;
        if (ve.flags & FT2Envelope::kEnabled)
        {
            float v = (uint16_t) volEnvValue * (1.0f / (64.0f * 256.0f));
            if (v > 1.0f) v = 1.0f;
            if (v < 0.0f) v = 0.0f;
            fEnvVol = v;
        }
        const float fFadeout = fadeoutVol * (1.0f / 32768.0f);
        const float gain = velocityLin * slotPtr->volume * fEnvVol * fFadeout;

        float pan = slotPan;
        if (pe.flags & FT2Envelope::kEnabled)
        {
            /* pan env value -32..+32 (after centre subtract).  Combine
             * with base pan as additive offset 0..1. */
            float pv = (uint16_t) panEnvValue * (1.0f / (64.0f * 256.0f));
            if (pv > 1.0f) pv = 1.0f;
            if (pv < 0.0f) pv = 0.0f;
            const float delta = pv - 0.5f;
            const float panMul = 1.0f - std::abs (slotPan - 0.5f) * 2.0f;
            pan = juce::jlimit (0.0f, 1.0f, slotPan + delta * panMul);
        }

        setVoiceGain (gain, pan);

        /* autoVib pitch modulation. */
        if (instrument->autoVib.depth > 0)
        {
            int16_t vibVal;
            switch (instrument->autoVib.type)
            {
                case 1: vibVal = (autoVibPos > 127) ? 64 : -64; break;
                case 2: vibVal = (int16_t) ((((autoVibPos >> 1) + 64) & 127) - 64); break;
                case 3: vibVal = (int16_t) (((-(autoVibPos >> 1) + 64) & 127) - 64); break;
                default: vibVal = kAutoVibSineTab[autoVibPos];
            }
            const int32_t scaled = (vibVal * (int16_t) autoVibAmp) >> (6 + 8);
            /* scaled is in "period" units; for our delta-driven voice
             * apply as a fractional semitone perturbation.  64 ~= ±1 semi
             * at full depth at the sine peak; scale to cents:
             * cents ≈ scaled * (100/64). */
            const double cents = scaled * (100.0 / 64.0);
            const double mult  = std::pow (2.0, cents / 1200.0);
            setVoiceDelta (basePitchMul * mult);
        }
    }

    void setVoiceDelta (double pitchMul)
    {
        const double playRate = pitchMul * slotPtr->sourceSampleRate / getSampleRate();
        voice.delta = (uint64_t) jlimit (0.0, 1e18, playRate * 4294967296.0);
    }

    void setVoiceGain (float gain, float pan)
    {
        voice.fVolume = gain;
        if (slotIsStereo)
        {
            const float r = jmax (0.0f, 2.0f * (pan - 0.5f));
            const float l = jmax (0.0f, 2.0f * (0.5f - pan));
            gLL = gain * (1.0f - r);
            gRR = gain * (1.0f - l);
            gLR = gain * r;
            gRL = gain * l;
            voice.fCurrVolumeL = gLL;
            voice.fCurrVolumeR = gLR;
        }
        else
        {
            voice.fCurrVolumeL = gain * (1.0f - pan);
            voice.fCurrVolumeR = gain *          pan;
        }
        voice.fTargetVolumeL = voice.fCurrVolumeL;
        voice.fTargetVolumeR = voice.fCurrVolumeR;
        voice.fVolumeLDelta  = 0.0f;
        voice.fVolumeRDelta  = 0.0f;
        voice.volumeRampLength = 0;
    }

    SamplerNode& owner;
    voice_t voice {};
    ADSR adsr;
    SamplerInstrument::Ptr instrument;

    /* Static (per-note) state. */
    const SamplerSampleSlot* slotPtr = nullptr;
    bool slotIsStereo = false;
    int   playedNote = 60;
    float velocityLin = 1.0f;
    float slotPan = 0.5f;
    double basePitchMul = 1.0;

    /* Stereo render-gain cache. */
    float gLL = 1.0f, gLR = 0.0f, gRL = 0.0f, gRR = 1.0f;

    /* FT2 envelope state. */
    bool      keyOff = false;
    uint8_t   volEnvPos = 0;
    uint16_t  volEnvTick = 0;
    int16_t   volEnvValue = 0;
    int16_t   volEnvDelta = 0;
    uint8_t   panEnvPos = 0;
    uint16_t  panEnvTick = 0;
    int16_t   panEnvValue = 0;
    int16_t   panEnvDelta = 0;
    uint16_t  fadeoutVol = 32768;
    uint8_t   autoVibPos = 0;
    uint16_t  autoVibAmp = 0;
    uint16_t  autoVibSweepCounter = 0;

    /* Tick-quantizer: samples since last envelope tick. */
    int       envSampleAccum = 0;
};


/* ===========================================================================
 * Helpers — note names.
 * ========================================================================*/

static String midiNoteName (int n)
{
    if (n < 0 || n > 127) return "--";
    static const char* names[12] = {
        "C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"
    };
    return String (names[n % 12]) + String ((n / 12) - 1);
}

/* ===========================================================================
 * SampleWaveformView — waveform + draggable loop markers + live playheads.
 * ========================================================================*/
class SampleWaveformView : public Component,
                           private Timer
{
public:
    using PlayheadQuery = std::function<std::vector<int> (const SamplerSampleSlot*)>;

    SampleWaveformView (std::function<SamplerSampleSlot*()> getSlot_,
                        PlayheadQuery playheadsFor_)
        : getSlot     (std::move (getSlot_)),
          playheadsFor (std::move (playheadsFor_))
    {
        startTimerHz (30);
    }

    ~SampleWaveformView() override { stopTimer(); }

    void paint (Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (Colour { 0xff'10'10'10 });
        g.fillRect (bounds);
        g.setColour (Colour { 0xff'2a'2a'2a });
        g.drawRect (bounds, 1.0f);

        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded())
        {
            g.setColour (Colour { 0xff'5a'5a'5a });
            g.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 11.0f, Font::plain));
            g.drawText ("(load a sample to set loop points)",
                        bounds.toFloat(), Justification::centred);
            return;
        }

        const int   w = juce::roundToInt (bounds.getWidth());
        const int   n = slot->numSamples;
        const float h = bounds.getHeight();
        const float midY = bounds.getCentreY();
        const float halfH = h * 0.45f;
        const auto* d = slot->data16L.get();
        if (n <= 0 || w <= 0 || d == nullptr) return;

        g.setColour (Colour { 0xff'5a'a5'd0 });
        for (int x = 0; x < w; ++x)
        {
            const int s0 = (int) ((int64_t) x * n / w);
            const int s1 = (int) std::max ((int64_t) s0 + 1, (int64_t) (x + 1) * n / w);
            int mn = INT16_MAX, mx = INT16_MIN;
            for (int i = s0; i < s1 && i < n; ++i)
            {
                const int v = d[i];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            const float yMin = midY - (mn / 32768.0f) * halfH;
            const float yMax = midY - (mx / 32768.0f) * halfH;
            g.drawLine (bounds.getX() + x, yMax, bounds.getX() + x, yMin);
        }

        if (slot->loopMode != SamplerLoopMode::kNone && slot->loopLength > 0)
        {
            const float x0 = bounds.getX() + (float) bounds.getWidth() * slot->loopStart / (float) n;
            const float x1 = bounds.getX() + (float) bounds.getWidth() * (slot->loopStart + slot->loopLength) / (float) n;
            g.setColour (Colour { 0x33'ff'a0'40 });
            g.fillRect (x0, bounds.getY(), x1 - x0, bounds.getHeight());
            g.setColour (Colour { 0xff'ff'a0'40 });
            g.fillRect (x0 - 1.0f, bounds.getY(), 2.0f, bounds.getHeight());
            g.fillRect (x1 - 1.0f, bounds.getY(), 2.0f, bounds.getHeight());
            g.fillEllipse (x0 - 4.0f, bounds.getY() - 0.0f, 8.0f, 8.0f);
            g.fillEllipse (x1 - 4.0f, bounds.getBottom() - 8.0f, 8.0f, 8.0f);
        }

        if (playheadsFor)
        {
            const auto positions = playheadsFor (slot);
            g.setColour (Colour { 0xff'40'ff'80 });
            for (int pos : positions)
            {
                if (pos < 0 || pos >= n) continue;
                const float x = bounds.getX() + (float) bounds.getWidth() * pos / (float) n;
                g.fillRect (x - 0.5f, bounds.getY(), 1.5f, bounds.getHeight());
            }
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded()) return;
        if (slot->loopMode == SamplerLoopMode::kNone || slot->loopLength <= 0)
        {
            slot->loopMode   = SamplerLoopMode::kForward;
            slot->loopStart  = pixelToSample (e.x, slot->numSamples);
            slot->loopLength = juce::jmax (1, slot->numSamples - slot->loopStart);
            draggingMarker   = 1;
            repaint();
            return;
        }
        const int x0 = sampleToPixel (slot->loopStart, slot->numSamples);
        const int x1 = sampleToPixel (slot->loopStart + slot->loopLength, slot->numSamples);
        draggingMarker = (std::abs (e.x - x0) < std::abs (e.x - x1)) ? 0 : 1;
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (draggingMarker < 0) return;
        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded()) return;

        const int newPos = pixelToSample (e.x, slot->numSamples);
        if (draggingMarker == 0)
        {
            const int oldEnd = slot->loopStart + slot->loopLength;
            slot->loopStart  = juce::jlimit (0, oldEnd - 1, newPos);
            slot->loopLength = oldEnd - slot->loopStart;
        }
        else
        {
            slot->loopLength = juce::jlimit (1, slot->numSamples - slot->loopStart,
                                              newPos - slot->loopStart);
        }
        repaint();
    }

    void mouseUp (const MouseEvent&) override { draggingMarker = -1; }

private:
    void timerCallback() override
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        std::vector<int> cur = slot != nullptr ? playheadsFor (slot) : std::vector<int>();
        if (cur == lastPlayheads_)
            return;
        lastPlayheads_ = std::move (cur);
        repaint();
    }

    int pixelToSample (int x, int n) const
    {
        const auto bounds = getLocalBounds().reduced (2);
        const int w = bounds.getWidth();
        if (w <= 0) return 0;
        return (int) juce::jlimit ((int64_t) 0, (int64_t) n,
                                    (int64_t) (x - bounds.getX()) * n / w);
    }
    int sampleToPixel (int s, int n) const
    {
        const auto bounds = getLocalBounds().reduced (2);
        if (n <= 0) return bounds.getX();
        return bounds.getX() + (int) ((int64_t) bounds.getWidth() * s / n);
    }

    std::function<SamplerSampleSlot*()> getSlot;
    PlayheadQuery playheadsFor;
    std::vector<int> lastPlayheads_;
    int draggingMarker = -1;
};


/* ===========================================================================
 * FT2EnvelopeView — multi-point envelope editor.  Up to 12 points,
 * draggable; sustain + loop markers drawn vertically.  Single envelope per
 * view; we instantiate two (volume + pan) in the editor.
 * ========================================================================*/
class FT2EnvelopeView : public Component
{
public:
    FT2EnvelopeView (FT2Envelope& env_, const String& title_,
                     std::function<void()> onChange_)
        : env (env_), title (title_), onChange (std::move (onChange_)) {}

    void paint (Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat().reduced (4.0f);
        g.setColour (Colour { 0xff'12'12'12 });
        g.fillRect (area);
        g.setColour ((env.flags & FT2Envelope::kEnabled) ? Colour { 0xff'4a'7a'b5 }
                                                         : Colour { 0xff'30'30'30 });
        g.drawRect (area, 1.0f);

        /* Title + flag chips. */
        g.setColour (Colour { 0xff'9a'9a'9a });
        g.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 10.0f, Font::plain));
        const String flagStr = String::formatted ("%s%s%s",
            (env.flags & FT2Envelope::kEnabled) ? "ON "  : "off ",
            (env.flags & FT2Envelope::kSustain) ? "SUS " : "",
            (env.flags & FT2Envelope::kLoop)    ? "LOOP" : "");
        g.drawText (title + "  " + flagStr, area.reduced (4.0f, 2.0f),
                    Justification::topLeft);

        if (env.length < 2) return;

        const float xMax = (float) env.points[env.length - 1].x;
        const float yMax = 64.0f;
        auto pxAt = [&] (float x) { return area.getX() + (x / xMax) * area.getWidth(); };
        auto pyAt = [&] (float y) { return area.getY() + (1.0f - y / yMax) * area.getHeight(); };

        /* Sustain marker (vertical line). */
        if ((env.flags & FT2Envelope::kSustain) && env.sustainPoint < env.length)
        {
            const float xs = pxAt ((float) env.points[env.sustainPoint].x);
            g.setColour (Colour { 0x88'ff'40'40 });
            g.fillRect (xs - 0.5f, area.getY(), 1.0f, area.getHeight());
        }
        /* Loop region shade. */
        if ((env.flags & FT2Envelope::kLoop)
            && env.loopStart < env.length && env.loopEnd < env.length
            && env.loopEnd >= env.loopStart)
        {
            const float xa = pxAt ((float) env.points[env.loopStart].x);
            const float xb = pxAt ((float) env.points[env.loopEnd].x);
            g.setColour (Colour { 0x33'40'a0'40 });
            g.fillRect (xa, area.getY(), xb - xa, area.getHeight());
        }

        /* Polyline + handles. */
        Path curve;
        curve.startNewSubPath (pxAt ((float) env.points[0].x), pyAt ((float) env.points[0].y));
        for (int i = 1; i < env.length; ++i)
            curve.lineTo (pxAt ((float) env.points[i].x), pyAt ((float) env.points[i].y));
        g.setColour ((env.flags & FT2Envelope::kEnabled) ? Colour { 0xff'5a'a5'd0 }
                                                         : Colour { 0xff'4a'4a'4a });
        g.strokePath (curve, PathStrokeType (2.0f));

        for (int i = 0; i < env.length; ++i)
        {
            const float x = pxAt ((float) env.points[i].x);
            const float y = pyAt ((float) env.points[i].y);
            g.setColour ((i == env.sustainPoint && (env.flags & FT2Envelope::kSustain))
                           ? Colour { 0xff'ff'40'40 }
                           : Colour { 0xff'ff'a0'40 });
            g.fillEllipse (x - 4.0f, y - 4.0f, 8.0f, 8.0f);
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        const auto area = getLocalBounds().toFloat().reduced (4.0f);
        draggingPoint = -1;
        if (env.length < 1) return;
        const float xMax = (float) env.points[env.length - 1].x;
        if (xMax <= 0) return;

        for (int i = 0; i < env.length; ++i)
        {
            const float x = area.getX() + ((float) env.points[i].x / xMax) * area.getWidth();
            const float y = area.getY() + (1.0f - (float) env.points[i].y / 64.0f) * area.getHeight();
            if (Point<float> (x, y).getDistanceFrom (e.position) < 10.0f)
            {
                draggingPoint = i;
                if (e.mods.isPopupMenu())
                {
                    showPointMenu (i);
                    return;
                }
                return;
            }
        }
        if (e.mods.isPopupMenu()) showCanvasMenu();
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (draggingPoint < 0 || draggingPoint >= env.length) return;
        const auto area = getLocalBounds().toFloat().reduced (4.0f);
        const float xMax = (float) jmax<int> (1, env.points[env.length - 1].x);

        const float xRel = jlimit (0.0f, 1.0f,
                                   (e.position.x - area.getX()) / area.getWidth());
        const float yRel = 1.0f - jlimit (0.0f, 1.0f,
                                   (e.position.y - area.getY()) / area.getHeight());
        const int newX = (int) (xRel * xMax);
        const int newY = (int) (yRel * 64.0f);

        /* Point 0 X-locked at 0.  Other points x must stay strictly
         * ascending. */
        if (draggingPoint == 0)
        {
            env.points[0].y = (int16_t) jlimit (0, 64, newY);
        }
        else
        {
            const int prevX = env.points[draggingPoint - 1].x;
            const int nextX = (draggingPoint + 1 < env.length)
                                ? env.points[draggingPoint + 1].x
                                : 324;
            env.points[draggingPoint].x = (int16_t) jlimit (prevX + 1, nextX - 1, newX);
            env.points[draggingPoint].y = (int16_t) jlimit (0, 64, newY);
        }

        repaint();
        if (onChange) onChange();
    }

    void mouseUp (const MouseEvent&) override { draggingPoint = -1; }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        /* Add point between two neighbours, or replace y if on existing. */
        const auto area = getLocalBounds().toFloat().reduced (4.0f);
        if (env.length >= 12 || env.length < 1) return;
        const float xMax = (float) env.points[env.length - 1].x;
        const float xRel = jlimit (0.0f, 1.0f, (e.position.x - area.getX()) / area.getWidth());
        const float yRel = 1.0f - jlimit (0.0f, 1.0f, (e.position.y - area.getY()) / area.getHeight());
        const int newX = (int) (xRel * xMax);
        const int newY = (int) (yRel * 64.0f);

        int insertAt = 1;
        for (; insertAt < env.length; ++insertAt)
            if (env.points[insertAt].x > newX) break;

        for (int i = env.length; i > insertAt; --i)
            env.points[i] = env.points[i - 1];
        env.points[insertAt] = { (int16_t) newX, (int16_t) newY };
        env.length = (uint8_t) (env.length + 1);

        repaint();
        if (onChange) onChange();
    }

private:
    void showPointMenu (int pointIdx)
    {
        PopupMenu m;
        m.addItem (1, "Sustain point",
                   pointIdx > 0 && pointIdx < env.length,
                   (env.flags & FT2Envelope::kSustain) && env.sustainPoint == pointIdx);
        m.addItem (2, "Loop start",
                   pointIdx < env.length,
                   (env.flags & FT2Envelope::kLoop) && env.loopStart == pointIdx);
        m.addItem (3, "Loop end",
                   pointIdx < env.length && pointIdx >= env.loopStart,
                   (env.flags & FT2Envelope::kLoop) && env.loopEnd == pointIdx);
        m.addSeparator();
        m.addItem (4, "Remove point", env.length > 2 && pointIdx > 0);
        const int sel = m.showAt (this);
        switch (sel)
        {
            case 1:
                env.sustainPoint = (uint8_t) pointIdx;
                env.flags ^= FT2Envelope::kSustain;
                break;
            case 2:
                env.loopStart = (uint8_t) pointIdx;
                env.flags |= FT2Envelope::kLoop;
                break;
            case 3:
                env.loopEnd = (uint8_t) pointIdx;
                env.flags |= FT2Envelope::kLoop;
                break;
            case 4:
                for (int i = pointIdx; i + 1 < env.length; ++i)
                    env.points[i] = env.points[i + 1];
                env.length = (uint8_t) (env.length - 1);
                if (env.sustainPoint >= env.length) env.sustainPoint = (uint8_t) (env.length - 1);
                if (env.loopStart   >= env.length) env.loopStart   = (uint8_t) (env.length - 1);
                if (env.loopEnd     >= env.length) env.loopEnd     = (uint8_t) (env.length - 1);
                break;
            default: return;
        }
        repaint();
        if (onChange) onChange();
    }

    void showCanvasMenu()
    {
        PopupMenu m;
        m.addItem (1, "Enable envelope",  true, (env.flags & FT2Envelope::kEnabled));
        m.addItem (2, "Clear loop",       true, (env.flags & FT2Envelope::kLoop));
        m.addItem (3, "Clear sustain",    true, (env.flags & FT2Envelope::kSustain));
        const int sel = m.showAt (this);
        switch (sel)
        {
            case 1: env.flags ^= FT2Envelope::kEnabled; break;
            case 2: env.flags &= ~FT2Envelope::kLoop;   break;
            case 3: env.flags &= ~FT2Envelope::kSustain; break;
            default: return;
        }
        repaint();
        if (onChange) onChange();
    }

    FT2Envelope& env;
    String title;
    std::function<void()> onChange;
    int draggingPoint = -1;
};


/* ===========================================================================
 * SamplerKeymap — FT2-style key-to-slot map editor.  Shows 2 octaves of
 * piano keys at a time (24 keys), each tinted by its assigned slot
 * (0..15 → 16-step palette).  Click a key (or drag-paint a range) to
 * assign the currently active slot to that MIDI note range.
 *
 * Octave-shift buttons live in the parent InstPage; this component
 * just renders the visible window starting at `baseOctave_` and
 * dispatches mouse events to keymap mutations on the bound instrument.
 * ========================================================================*/
class SamplerKeymap : public Component
{
public:
    using GetInstrument = std::function<SamplerInstrument::Ptr()>;
    using GetActiveSlot = std::function<int()>;

    SamplerKeymap (GetInstrument getInst_, GetActiveSlot getSlot_)
        : getInst (std::move (getInst_)),
          getSlot (std::move (getSlot_)) {}

    void setBaseOctave (int oct)
    {
        baseOctave_ = juce::jlimit (-1, 8, oct);
        repaint();
    }
    int  getBaseOctave() const noexcept { return baseOctave_; }

    void paint (Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (Colour { 0xff'10'10'10 });
        g.fillRect (bounds);

        const auto inst = getInst ? getInst() : nullptr;
        const int  baseNote = (baseOctave_ + 1) * 12;     /* MIDI: C-1 = 0 */
        constexpr int kWhitePerOct = 7;
        const int totalWhite = kWhitePerOct * 2;          /* 14 whites in 2 oct */
        const float whiteW = bounds.getWidth() / (float) totalWhite;
        const float whiteH = bounds.getHeight();
        const float blackH = whiteH * 0.62f;
        const float blackW = whiteW * 0.6f;

        static const bool isWhite[12] = { true,false,true,false,true,true,false,
                                          true,false,true,false,true };
        static const int blackOffsetPC[12] = { -1, 0, -1, 1, -1, -1, 3, -1, 4, -1, 5, -1 };

        /* Pass 1: whites — gray-white base + slot-color accent strip. */
        int whiteIdx = 0;
        for (int n = 0; n < 24; ++n)
        {
            const int note = baseNote + n;
            const int pc   = note % 12;
            if (! isWhite[pc]) continue;
            const float x  = bounds.getX() + whiteIdx * whiteW;
            const int assignedSlot = inst ? inst->slotForNote (note) : -1;
            const bool hasAssign   = inst && assignedSlot >= 0
                                  && inst->getSlot (assignedSlot)
                                  && inst->getSlot (assignedSlot)->isLoaded();

            g.setColour (Colour { 0xff'b8'b8'b8 });   /* light gray base */
            g.fillRect (x + 1.0f, bounds.getY(), whiteW - 1.0f, whiteH);
            g.setColour (Colour { 0xff'2a'2a'2a });
            g.drawRect (x, bounds.getY(), whiteW, whiteH, 1.0f);

            /* Slot-color accent — top stripe only, not the whole key. */
            if (hasAssign)
            {
                const auto accent = slotColour (assignedSlot);
                g.setColour (accent);
                g.fillRect (x + 1.0f, bounds.getY(), whiteW - 1.0f, 10.0f);
                g.setColour (Colour { 0xff'30'30'30 });
                g.drawHorizontalLine (juce::roundToInt (bounds.getY() + 10),
                                      x + 1.0f, x + whiteW);
            }

            /* Slot # — large, centred on the lower half of the key. */
            g.setColour (hasAssign ? Colours::black : Colour { 0xff'70'70'70 });
            g.setFont (FontOptions (Font::getDefaultMonospacedFontName(),
                                    13.0f, Font::bold));
            const String s = hasAssign ? String (assignedSlot + 1)
                                       : String (CharPointer_UTF8 ("\xe2\x80\x94"));
            g.drawText (s, Rectangle<float> (x, bounds.getBottom() - 22,
                                              whiteW, 18),
                        Justification::centred);
            ++whiteIdx;
        }

        /* Pass 2: blacks — dark base + slot-color accent top stripe. */
        for (int n = 0; n < 24; ++n)
        {
            const int note = baseNote + n;
            const int pc   = note % 12;
            if (isWhite[pc]) continue;
            const int bIdx = blackOffsetPC[pc];
            const int oct  = n / 12;
            const float xCentre = bounds.getX()
                                  + (oct * kWhitePerOct + bIdx + 1) * whiteW;
            const float x = xCentre - blackW * 0.5f;
            const int assignedSlot = inst ? inst->slotForNote (note) : -1;
            const bool hasAssign   = inst && assignedSlot >= 0
                                  && inst->getSlot (assignedSlot)
                                  && inst->getSlot (assignedSlot)->isLoaded();

            g.setColour (Colour { 0xff'1c'1c'1c });
            g.fillRect (x, bounds.getY(), blackW, blackH);
            if (hasAssign)
            {
                const auto accent = slotColour (assignedSlot).withMultipliedBrightness (0.85f);
                g.setColour (accent);
                g.fillRect (x, bounds.getY(), blackW, 6.0f);
            }
            g.setColour (Colour { 0xff'1a'1a'1a });
            g.drawRect (x, bounds.getY(), blackW, blackH, 1.0f);

            /* Slot # in white on black key. */
            if (hasAssign)
            {
                g.setColour (Colour { 0xff'e8'e8'e8 });
                g.setFont (FontOptions (Font::getDefaultMonospacedFontName(),
                                        10.0f, Font::bold));
                g.drawText (String (assignedSlot + 1),
                            Rectangle<float> (x, bounds.getY() + blackH - 16,
                                              blackW, 14),
                            Justification::centred);
            }
        }

        /* Octave labels. */
        g.setColour (Colour { 0xff'70'70'70 });
        g.setFont (FontOptions (Font::getDefaultMonospacedFontName(),
                                10.0f, Font::bold));
        for (int oct = 0; oct < 2; ++oct)
        {
            const float x = bounds.getX() + oct * kWhitePerOct * whiteW + 2.0f;
            g.drawText (String ("C") + String (baseOctave_ + oct),
                        Rectangle<float> (x, bounds.getY(), 24, 14),
                        Justification::topLeft);
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        const int note = noteAt (e.x, e.y);
        if (note < 0) return;
        const auto inst = getInst ? getInst() : nullptr;
        if (inst == nullptr) return;

        if (e.mods.isPopupMenu())
        {
            /* Right-click → pick slot inline (no need to leave Inst page). */
            PopupMenu m;
            for (int i = 0; i < SamplerInstrument::kNumSlots; ++i)
            {
                const auto* slot = inst->getSlot (i);
                const String label = String::formatted ("%02d  %s", i + 1,
                    (slot && slot->isLoaded()) ? slot->name.toRawUTF8() : "(empty)");
                m.addItem (i + 1, label,
                           true,
                           inst->slotForNote (note) == i);
            }
            const int chosen = m.showAt (this);
            if (chosen >= 1 && chosen <= SamplerInstrument::kNumSlots)
                inst->setSlotForNote (note, chosen - 1);
            repaint();
            return;
        }

        const int slot  = getSlot ? getSlot() : -1;
        if (slot < 0 || slot >= SamplerInstrument::kNumSlots) return;
        inst->setSlotForNote (note, slot);
        dragAnchorNote_ = note;
        repaint();
    }

    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
    {
        const int note = noteAt (e.x, e.y);
        if (note < 0) return;
        const auto inst = getInst ? getInst() : nullptr;
        if (inst == nullptr) return;
        const int cur = inst->slotForNote (note);
        int next = cur + (w.deltaY > 0 ? +1 : -1);
        if (next < 0)                                    next = SamplerInstrument::kNumSlots - 1;
        if (next >= SamplerInstrument::kNumSlots)        next = 0;
        inst->setSlotForNote (note, next);
        repaint();
    }
    void mouseDrag (const MouseEvent& e) override
    {
        const int note = noteAt (e.x, e.y);
        if (note < 0) return;
        if (dragAnchorNote_ < 0) return;
        const auto inst = getInst ? getInst() : nullptr;
        if (inst == nullptr) return;
        const int slot  = getSlot ? getSlot() : -1;
        if (slot < 0 || slot >= SamplerInstrument::kNumSlots) return;
        const int lo = juce::jmin (dragAnchorNote_, note);
        const int hi = juce::jmax (dragAnchorNote_, note);
        for (int n = lo; n <= hi; ++n)
            inst->setSlotForNote (n, slot);
        repaint();
    }
    void mouseUp (const MouseEvent&) override { dragAnchorNote_ = -1; }

private:
    int noteAt (int px, int py) const
    {
        const auto bounds = getLocalBounds().reduced (2);
        if (! bounds.contains (px, py)) return -1;
        const int baseNote = (baseOctave_ + 1) * 12;
        constexpr int kWhitePerOct = 7;
        const int totalWhite = kWhitePerOct * 2;
        const float whiteW = bounds.getWidth() / (float) totalWhite;
        const float whiteH = bounds.getHeight();
        const float blackH = whiteH * 0.6f;
        const float blackW = whiteW * 0.6f;

        /* Test blacks first (they overlap whites). */
        static const bool isWhite[12] = { true,false,true,false,true,true,false,
                                          true,false,true,false,true };
        static const int blackOffsetPC[12] = { -1, 0, -1, 1, -1, -1, 3, -1, 4, -1, 5, -1 };
        const float relY = py - bounds.getY();
        if (relY < blackH)
        {
            for (int n = 0; n < 24; ++n)
            {
                const int note = baseNote + n;
                const int pc   = note % 12;
                if (isWhite[pc]) continue;
                const int bIdx = blackOffsetPC[pc];
                const int oct  = n / 12;
                const float xCentre = bounds.getX()
                                      + (oct * kWhitePerOct + bIdx + 1) * whiteW;
                const float xL = xCentre - blackW * 0.5f;
                if (px >= xL && px <= xL + blackW) return note;
            }
        }
        /* Whites. */
        int whiteIdx = 0;
        for (int n = 0; n < 24; ++n)
        {
            const int note = baseNote + n;
            const int pc   = note % 12;
            if (! isWhite[pc]) continue;
            const float xL = bounds.getX() + whiteIdx * whiteW;
            if (px >= xL && px <= xL + whiteW) return note;
            ++whiteIdx;
        }
        return -1;
    }

    /** Colour for slot index in the 16-step palette. */
    static Colour slotColour (int slot)
    {
        if (slot < 0) return Colour { 0xff'30'30'30 };
        const float h = (slot % 16) / 16.0f;
        return Colour::fromHSV (h, 0.55f, 0.85f, 1.0f);
    }

    GetInstrument getInst;
    GetActiveSlot getSlot;
    int baseOctave_   = 4;   /* shows C4..B5 by default */
    int dragAnchorNote_ = -1;
};


/* ===========================================================================
 * SamplerInstPage — the "Edit instrument" page of the paged Sampler
 * editor.  Shows two envelope editors (vol+pan) at larger size, sustain
 * + loop point controls per envelope, fadeout + auto-vib sliders, the
 * 2-octave keymap with octave-shift navigation, and predef-envelope
 * preset buttons.
 * ========================================================================*/
class SamplerInstPage : public Component
{
public:
    using GetInstrument = std::function<SamplerInstrument::Ptr()>;
    using GetActiveSlot = std::function<int()>;
    using SetActiveSlot = std::function<void (int)>;

    SamplerInstPage (GetInstrument gi, GetActiveSlot gs, SetActiveSlot ss)
        : getInst (std::move (gi)),
          getActiveSlot (gs),
          setActiveSlot (std::move (ss)),
          keymap (getInst, std::move (gs))
    {
        addAndMakeVisible (keymap);

        slotPickLbl.setText ("Paint slot:", dontSendNotification);
        slotPickLbl.setColour (Label::textColourId, Colour { 0xff'b0'b0'b0 });
        slotPickLbl.setFont (FontOptions (Font::getDefaultMonospacedFontName(),
                                          11.0f, Font::plain));
        addAndMakeVisible (slotPickLbl);

        slotPickValue.setJustificationType (Justification::centred);
        slotPickValue.setColour (Label::textColourId, Colour { 0xff'd0'80'40 });
        slotPickValue.setFont (FontOptions (Font::getDefaultMonospacedFontName(),
                                            13.0f, Font::bold));
        addAndMakeVisible (slotPickValue);

        configureNudge (slotDownBtn, "-", [this] {
            const int s = getActiveSlot();
            setActiveSlot (juce::jmax (0, s - 1));
            refresh();
        });
        configureNudge (slotUpBtn, "+", [this] {
            const int s = getActiveSlot();
            setActiveSlot (juce::jmin (SamplerInstrument::kNumSlots - 1, s + 1));
            refresh();
        });

        for (int p = 1; p <= 6; ++p)
        {
            auto btn = std::make_unique<TextButton> (String (p));
            const int preset = p;
            btn->onClick = [this, preset] { applyPredefinedEnvelope (preset); };
            btn->setColour (TextButton::buttonColourId, Colour { 0xff'24'24'24 });
            btn->setColour (TextButton::textColourOffId, Colour { 0xff'd0'd0'd0 });
            addAndMakeVisible (*btn);
            predefBtns.add (std::move (btn));
        }

        configureNudge (volAddBtn, "Add",  [this] { addEnvPoint (true);   });
        configureNudge (volDelBtn, "Del",  [this] { delLastEnvPoint (true); });
        configureNudge (panAddBtn, "Add",  [this] { addEnvPoint (false);  });
        configureNudge (panDelBtn, "Del",  [this] { delLastEnvPoint (false); });

        configureNudge (volSusUp,  "+", [this] { nudgeEnvPoint (true,  +1, false); });
        configureNudge (volSusDn,  "-", [this] { nudgeEnvPoint (true,  -1, false); });
        configureNudge (volLoopStartUp, "+", [this] { nudgeEnvLoop (true,  true,  +1); });
        configureNudge (volLoopStartDn, "-", [this] { nudgeEnvLoop (true,  true,  -1); });
        configureNudge (volLoopEndUp,   "+", [this] { nudgeEnvLoop (true,  false, +1); });
        configureNudge (volLoopEndDn,   "-", [this] { nudgeEnvLoop (true,  false, -1); });

        configureNudge (panSusUp,  "+", [this] { nudgeEnvPoint (false, +1, false); });
        configureNudge (panSusDn,  "-", [this] { nudgeEnvPoint (false, -1, false); });
        configureNudge (panLoopStartUp, "+", [this] { nudgeEnvLoop (false, true,  +1); });
        configureNudge (panLoopStartDn, "-", [this] { nudgeEnvLoop (false, true,  -1); });
        configureNudge (panLoopEndUp,   "+", [this] { nudgeEnvLoop (false, false, +1); });
        configureNudge (panLoopEndDn,   "-", [this] { nudgeEnvLoop (false, false, -1); });

        for (auto* l : { &volSusLbl, &volLoopLbl, &panSusLbl, &panLoopLbl,
                         &fadeLbl, &avSpeedLbl, &avDepthLbl, &avSweepLbl,
                         &predefLbl, &keymapLbl })
        {
            l->setColour (Label::textColourId, Colour { 0xff'b0'b0'b0 });
            l->setFont (FontOptions (Font::getDefaultMonospacedFontName(), 11.0f, Font::plain));
            addAndMakeVisible (*l);
        }
        volSusLbl   .setText ("Sust",    dontSendNotification);
        volLoopLbl  .setText ("Loop",    dontSendNotification);
        panSusLbl   .setText ("Sust",    dontSendNotification);
        panLoopLbl  .setText ("Loop",    dontSendNotification);
        fadeLbl     .setText ("Fadeout", dontSendNotification);
        avSpeedLbl  .setText ("Vib rate",dontSendNotification);
        avDepthLbl  .setText ("Vib depth", dontSendNotification);
        avSweepLbl  .setText ("Vib sweep", dontSendNotification);
        predefLbl   .setText ("Predef:", dontSendNotification);
        keymapLbl   .setText ("Keymap:", dontSendNotification);

        configureSlider (fadeoutSlider, 0.0, 4095.0, 1.0, [this] (double v) {
            if (auto inst = getInst()) inst->fadeoutRate = (uint16_t) v;
        });
        configureSlider (avRateSlider, 0.0, 63.0, 1.0, [this] (double v) {
            if (auto inst = getInst()) inst->autoVib.rate = (uint8_t) v;
        });
        configureSlider (avDepthSlider, 0.0, 15.0, 1.0, [this] (double v) {
            if (auto inst = getInst()) inst->autoVib.depth = (uint8_t) v;
        });
        configureSlider (avSweepSlider, 0.0, 255.0, 1.0, [this] (double v) {
            if (auto inst = getInst()) inst->autoVib.sweep = (uint8_t) v;
        });

        avTypeCombo.addItem ("Sine",    1);
        avTypeCombo.addItem ("Square",  2);
        avTypeCombo.addItem ("Ramp+",   3);
        avTypeCombo.addItem ("Ramp-",   4);
        avTypeCombo.setColour (ComboBox::backgroundColourId, Colour { 0xff'24'24'24 });
        avTypeCombo.setColour (ComboBox::textColourId,        Colour { 0xff'd0'd0'd0 });
        avTypeCombo.onChange = [this] {
            if (auto inst = getInst())
                inst->autoVib.type = (uint8_t) (avTypeCombo.getSelectedId() - 1);
        };
        addAndMakeVisible (avTypeCombo);

        configureFlag (volEnabledBtn,  "Env enabled", [this] (bool v) {
            toggleFlag (true, FT2Envelope::kEnabled, v);
        });
        configureFlag (volSustainBtn,  "Sustain",     [this] (bool v) {
            toggleFlag (true, FT2Envelope::kSustain, v);
        });
        configureFlag (volLoopBtn,     "Loop",        [this] (bool v) {
            toggleFlag (true, FT2Envelope::kLoop, v);
        });
        configureFlag (panEnabledBtn,  "Env enabled", [this] (bool v) {
            toggleFlag (false, FT2Envelope::kEnabled, v);
        });
        configureFlag (panSustainBtn,  "Sustain",     [this] (bool v) {
            toggleFlag (false, FT2Envelope::kSustain, v);
        });
        configureFlag (panLoopBtn,     "Loop",        [this] (bool v) {
            toggleFlag (false, FT2Envelope::kLoop, v);
        });

        configureNudge (octDownBtn, "<oct", [this] {
            keymap.setBaseOctave (keymap.getBaseOctave() - 1);
        });
        configureNudge (octUpBtn,   "oct>", [this] {
            keymap.setBaseOctave (keymap.getBaseOctave() + 1);
        });

        rebuildEnvViews();
    }

    /** Re-bind the envelope editors when the active instrument changes. */
    void rebuildEnvViews()
    {
        volEnvView.reset();
        panEnvView.reset();
        if (auto inst = getInst())
        {
            volEnvView.reset (new FT2EnvelopeView (inst->volumeEnv, "Volume",
                [this] { repaint(); }));
            panEnvView.reset (new FT2EnvelopeView (inst->panEnv, "Panning",
                [this] { repaint(); }));
            addAndMakeVisible (volEnvView.get());
            addAndMakeVisible (panEnvView.get());
        }
        refreshFlagStates();
        resized();
    }

    void refresh()
    {
        repaint();
        if (volEnvView) volEnvView->repaint();
        if (panEnvView) panEnvView->repaint();
        if (auto inst = getInst())
        {
            fadeoutSlider.setValue ((double) inst->fadeoutRate, dontSendNotification);
            avRateSlider .setValue ((double) inst->autoVib.rate,  dontSendNotification);
            avDepthSlider.setValue ((double) inst->autoVib.depth, dontSendNotification);
            avSweepSlider.setValue ((double) inst->autoVib.sweep, dontSendNotification);
            avTypeCombo  .setSelectedId ((int) inst->autoVib.type + 1, dontSendNotification);
            refreshFlagStates();
        }
        slotPickValue.setText (String::formatted ("%02d", getActiveSlot() + 1),
                               dontSendNotification);
        keymap.repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);

        /* Top strip: predef preset buttons + active-slot picker. */
        auto top = r.removeFromTop (24);
        predefLbl.setBounds (top.removeFromLeft (60));
        for (auto& b : predefBtns) {
            b->setBounds (top.removeFromLeft (28));
            top.removeFromLeft (2);
        }
        /* Slot picker right side. */
        slotUpBtn   .setBounds (top.removeFromRight (24));
        top.removeFromRight (2);
        slotPickValue.setBounds (top.removeFromRight (38));
        top.removeFromRight (2);
        slotDownBtn .setBounds (top.removeFromRight (24));
        top.removeFromRight (4);
        slotPickLbl .setBounds (top.removeFromRight (90));
        r.removeFromTop (6);

        /* Envelope edit area: 2 horizontal panels, stacked. */
        const int envH = juce::jmax (90, (r.getHeight() - 230) / 2);
        layoutEnv (r.removeFromTop (envH), true);
        r.removeFromTop (4);
        layoutEnv (r.removeFromTop (envH), false);
        r.removeFromTop (8);

        /* Per-instrument params row. */
        auto row = r.removeFromTop (24);
        fadeLbl    .setBounds (row.removeFromLeft (60));
        fadeoutSlider.setBounds (row.removeFromLeft (140)); row.removeFromLeft (12);
        avSpeedLbl .setBounds (row.removeFromLeft (70));
        avRateSlider.setBounds (row.removeFromLeft (100));  row.removeFromLeft (12);
        avDepthLbl .setBounds (row.removeFromLeft (80));
        avDepthSlider.setBounds (row.removeFromLeft (100));  row.removeFromLeft (12);
        avSweepLbl .setBounds (row.removeFromLeft (80));
        avSweepSlider.setBounds (row.removeFromLeft (100));  row.removeFromLeft (12);
        avTypeCombo.setBounds (row.removeFromLeft (90));
        r.removeFromTop (10);

        /* Keymap header (label + oct controls). */
        auto kmTop = r.removeFromTop (22);
        keymapLbl.setBounds (kmTop.removeFromLeft (60));
        octUpBtn  .setBounds (kmTop.removeFromRight (50));   kmTop.removeFromRight (4);
        octDownBtn.setBounds (kmTop.removeFromRight (50));
        r.removeFromTop (2);
        keymap.setBounds (r.removeFromTop (juce::jmax (60, r.getHeight())));
    }

private:
    void layoutEnv (Rectangle<int> area, bool isVol)
    {
        auto canvas = area.removeFromLeft (area.getWidth() - 168);
        if (isVol && volEnvView) volEnvView->setBounds (canvas);
        if (! isVol && panEnvView) panEnvView->setBounds (canvas);

        area.removeFromLeft (8);
        /* Right strip: Add/Del row + flag toggles + sustain/loop nudges. */
        auto& sus  = isVol ? volSusLbl  : panSusLbl;
        auto& loop = isVol ? volLoopLbl : panLoopLbl;
        auto& enabledBtn = isVol ? volEnabledBtn : panEnabledBtn;
        auto& susBtn     = isVol ? volSustainBtn : panSustainBtn;
        auto& loopBtn    = isVol ? volLoopBtn    : panLoopBtn;
        auto& addBtn = isVol ? volAddBtn : panAddBtn;
        auto& delBtn = isVol ? volDelBtn : panDelBtn;
        auto& susUp  = isVol ? volSusUp  : panSusUp;
        auto& susDn  = isVol ? volSusDn  : panSusDn;
        auto& lsUp   = isVol ? volLoopStartUp : panLoopStartUp;
        auto& lsDn   = isVol ? volLoopStartDn : panLoopStartDn;
        auto& leUp   = isVol ? volLoopEndUp   : panLoopEndUp;
        auto& leDn   = isVol ? volLoopEndDn   : panLoopEndDn;

        auto col = area.reduced (0);
        /* Add / Del envelope points row. */
        auto adRow = col.removeFromTop (20);
        addBtn.setBounds (adRow.removeFromLeft (74));
        adRow.removeFromLeft (4);
        delBtn.setBounds (adRow.removeFromLeft (74));
        col.removeFromTop (6);

        enabledBtn.setBounds (col.removeFromTop (20)); col.removeFromTop (3);
        susBtn    .setBounds (col.removeFromTop (20)); col.removeFromTop (3);
        loopBtn   .setBounds (col.removeFromTop (20)); col.removeFromTop (6);

        auto susRow = col.removeFromTop (20);
        sus.setBounds (susRow.removeFromLeft (50));
        susUp.setBounds (susRow.removeFromRight (22));
        susRow.removeFromRight (2);
        susDn.setBounds (susRow.removeFromRight (22));
        col.removeFromTop (3);
        auto lsRow = col.removeFromTop (20);
        loop.setBounds (lsRow.removeFromLeft (50));
        lsUp.setBounds (lsRow.removeFromRight (22));
        lsRow.removeFromRight (2);
        lsDn.setBounds (lsRow.removeFromRight (22));
        col.removeFromTop (3);
        auto leRow = col.removeFromTop (20);
        leRow.removeFromLeft (50);
        leUp.setBounds (leRow.removeFromRight (22));
        leRow.removeFromRight (2);
        leDn.setBounds (leRow.removeFromRight (22));
    }

    void configureNudge (TextButton& b, const String& t, std::function<void()> on)
    {
        b.setButtonText (t);
        b.onClick = std::move (on);
        b.setColour (TextButton::buttonColourId, Colour { 0xff'22'22'22 });
        b.setColour (TextButton::textColourOffId, Colour { 0xff'd0'd0'd0 });
        addAndMakeVisible (b);
    }
    void configureSlider (Slider& s, double lo, double hi, double step,
                          std::function<void (double)> on)
    {
        s.setSliderStyle (Slider::LinearBar);
        s.setRange (lo, hi, step);
        s.setColour (Slider::backgroundColourId, Colour { 0xff'24'24'24 });
        s.setColour (Slider::trackColourId,      Colour { 0xff'4a'7a'b5 });
        s.onValueChange = [&s, on] { on (s.getValue()); };
        addAndMakeVisible (s);
    }
    void configureFlag (TextButton& b, const String& t,
                        std::function<void (bool)> on)
    {
        b.setButtonText (t);
        b.setClickingTogglesState (true);
        b.setColour (TextButton::buttonOnColourId, Colour { 0xff'4a'7a'b5 });
        b.setColour (TextButton::textColourOffId,  Colour { 0xff'a0'a0'a0 });
        b.setColour (TextButton::textColourOnId,   Colours::white);
        b.onClick = [&b, on] { on (b.getToggleState()); };
        addAndMakeVisible (b);
    }

    void toggleFlag (bool isVol, uint8_t bit, bool wantOn)
    {
        auto inst = getInst();
        if (inst == nullptr) return;
        FT2Envelope& e = isVol ? inst->volumeEnv : inst->panEnv;
        if (wantOn) e.flags |=  bit;
        else        e.flags &= ~bit;
        if (volEnvView) volEnvView->repaint();
        if (panEnvView) panEnvView->repaint();
    }
    void refreshFlagStates()
    {
        auto inst = getInst();
        if (inst == nullptr) return;
        const auto& ve = inst->volumeEnv;
        const auto& pe = inst->panEnv;
        volEnabledBtn.setToggleState (ve.flags & FT2Envelope::kEnabled, dontSendNotification);
        volSustainBtn.setToggleState (ve.flags & FT2Envelope::kSustain, dontSendNotification);
        volLoopBtn   .setToggleState (ve.flags & FT2Envelope::kLoop,    dontSendNotification);
        panEnabledBtn.setToggleState (pe.flags & FT2Envelope::kEnabled, dontSendNotification);
        panSustainBtn.setToggleState (pe.flags & FT2Envelope::kSustain, dontSendNotification);
        panLoopBtn   .setToggleState (pe.flags & FT2Envelope::kLoop,    dontSendNotification);
    }

    /** Append a sensible new point at xMax+20, y = previous point's y.
     *  Mirrors FT2's "Add" button — extends the envelope tail. */
    void addEnvPoint (bool isVol)
    {
        auto inst = getInst();
        if (inst == nullptr) return;
        FT2Envelope& e = isVol ? inst->volumeEnv : inst->panEnv;
        if (e.length >= 12) return;
        if (e.length == 0)
        {
            e.points[0] = { 0, 64 };
            e.length = 1;
        }
        const int prevX = e.points[e.length - 1].x;
        const int prevY = e.points[e.length - 1].y;
        const int newX  = juce::jmin (324, prevX + 20);
        e.points[e.length] = { (int16_t) newX, (int16_t) prevY };
        e.length = (uint8_t) (e.length + 1);
        if (volEnvView) volEnvView->repaint();
        if (panEnvView) panEnvView->repaint();
    }
    /** Remove the trailing point.  FT2's "Del". */
    void delLastEnvPoint (bool isVol)
    {
        auto inst = getInst();
        if (inst == nullptr) return;
        FT2Envelope& e = isVol ? inst->volumeEnv : inst->panEnv;
        if (e.length <= 2) return;     /* keep at least 2 (degenerate avoided) */
        e.length = (uint8_t) (e.length - 1);
        if (e.sustainPoint >= e.length) e.sustainPoint = (uint8_t) (e.length - 1);
        if (e.loopStart   >= e.length) e.loopStart   = (uint8_t) (e.length - 1);
        if (e.loopEnd     >= e.length) e.loopEnd     = (uint8_t) (e.length - 1);
        if (volEnvView) volEnvView->repaint();
        if (panEnvView) panEnvView->repaint();
    }

    void nudgeEnvPoint (bool isVol, int delta, bool /*loopMode*/)
    {
        auto inst = getInst();
        if (inst == nullptr) return;
        FT2Envelope& e = isVol ? inst->volumeEnv : inst->panEnv;
        e.sustainPoint = (uint8_t) juce::jlimit (0, juce::jmax (0, (int) e.length - 1),
                                                   (int) e.sustainPoint + delta);
        if (volEnvView) volEnvView->repaint();
        if (panEnvView) panEnvView->repaint();
    }
    void nudgeEnvLoop (bool isVol, bool isStart, int delta)
    {
        auto inst = getInst();
        if (inst == nullptr) return;
        FT2Envelope& e = isVol ? inst->volumeEnv : inst->panEnv;
        const int maxIdx = juce::jmax (0, (int) e.length - 1);
        if (isStart)
            e.loopStart = (uint8_t) juce::jlimit (0, maxIdx, (int) e.loopStart + delta);
        else
            e.loopEnd   = (uint8_t) juce::jlimit ((int) e.loopStart, maxIdx,
                                                   (int) e.loopEnd + delta);
        if (volEnvView) volEnvView->repaint();
        if (panEnvView) panEnvView->repaint();
    }

    /** FT2-style predef shapes — applied to the volume envelope. */
    void applyPredefinedEnvelope (int preset)
    {
        auto inst = getInst();
        if (inst == nullptr) return;
        FT2Envelope& e = inst->volumeEnv;
        e.flags = FT2Envelope::kEnabled;
        e.sustainPoint = 0;
        e.loopStart    = 0;
        e.loopEnd      = 0;
        switch (preset)
        {
            case 1: /* Pluck: A-D, fast decay */
                e.length = 3;
                e.points[0] = {  0,  0 };
                e.points[1] = {  2, 64 };
                e.points[2] = { 30,  0 };
                break;
            case 2: /* AD-sustain */
                e.length = 4;
                e.points[0] = {  0,  0 };
                e.points[1] = {  4, 64 };
                e.points[2] = { 30, 50 };
                e.points[3] = { 50, 50 };
                e.sustainPoint = 3;
                e.flags |= FT2Envelope::kSustain;
                break;
            case 3: /* Pad: slow attack, sustained */
                e.length = 4;
                e.points[0] = {   0,  0 };
                e.points[1] = {  50, 50 };
                e.points[2] = { 100, 64 };
                e.points[3] = { 150, 64 };
                e.sustainPoint = 3;
                e.flags |= FT2Envelope::kSustain;
                break;
            case 4: /* Tremolo loop */
                e.length = 4;
                e.points[0] = {  0,  0 };
                e.points[1] = {  4, 64 };
                e.points[2] = { 20, 20 };
                e.points[3] = { 40, 64 };
                e.loopStart = 1; e.loopEnd = 3;
                e.flags |= FT2Envelope::kLoop;
                break;
            case 5: /* Sustain — FT2 default. */
                e.length = 2;
                e.points[0] = { 0,  64 };
                e.points[1] = { 50, 64 };
                break;
            case 6: /* Decay-only (no sustain) */
                e.length = 2;
                e.points[0] = {  0, 64 };
                e.points[1] = { 80,  0 };
                break;
            default: break;
        }
        refreshFlagStates();
        if (volEnvView) volEnvView->repaint();
    }

    GetInstrument getInst;
    GetActiveSlot getActiveSlot;
    SetActiveSlot setActiveSlot;
    SamplerKeymap keymap;
    std::unique_ptr<FT2EnvelopeView> volEnvView, panEnvView;

    Label slotPickLbl, slotPickValue;
    TextButton slotDownBtn, slotUpBtn;

    OwnedArray<TextButton> predefBtns;
    Label predefLbl, keymapLbl;
    Label volSusLbl, volLoopLbl, panSusLbl, panLoopLbl;
    Label fadeLbl, avSpeedLbl, avDepthLbl, avSweepLbl;

    TextButton volEnabledBtn, volSustainBtn, volLoopBtn;
    TextButton panEnabledBtn, panSustainBtn, panLoopBtn;
    TextButton volAddBtn, volDelBtn, panAddBtn, panDelBtn;
    TextButton volSusUp,  volSusDn,  panSusUp,  panSusDn;
    TextButton volLoopStartUp, volLoopStartDn, volLoopEndUp, volLoopEndDn;
    TextButton panLoopStartUp, panLoopStartDn, panLoopEndUp, panLoopEndDn;

    Slider fadeoutSlider, avRateSlider, avDepthSlider, avSweepSlider;
    ComboBox avTypeCombo;
    TextButton octDownBtn, octUpBtn;
};


/* ===========================================================================
 * SampleEditCanvas — large waveform display with range selection +
 * viewport zoom.  Coordinates are int sample indices (0..numSamples).
 * Range selection: [selStart_..selEnd_) — half-open.  Viewport:
 * [viewStart_..viewEnd_).  Renders only the visible window.
 * ========================================================================*/
class SampleEditCanvas : public Component
{
public:
    using GetSlot = std::function<SamplerSampleSlot*()>;
    using RangeChangedCb = std::function<void()>;

    explicit SampleEditCanvas (GetSlot gs, RangeChangedCb cb = {})
        : getSlot (std::move (gs)), onRangeChanged (std::move (cb)) {}

    void setViewport (int s, int e)
    {
        viewStart_ = s; viewEnd_ = e;
        repaint();
    }
    void setSelection (int s, int e)
    {
        selStart_ = juce::jmin (s, e);
        selEnd_   = juce::jmax (s, e);
        if (onRangeChanged) onRangeChanged();
        repaint();
    }
    void clearSelection() { selStart_ = selEnd_ = -1; if (onRangeChanged) onRangeChanged(); repaint(); }
    int  getSelStart() const noexcept { return selStart_; }
    int  getSelEnd()   const noexcept { return selEnd_; }
    int  getViewStart() const noexcept { return viewStart_; }
    int  getViewEnd()   const noexcept { return viewEnd_; }
    bool hasSelection() const noexcept { return selStart_ >= 0 && selEnd_ > selStart_; }

    void paint (Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (Colour { 0xff'0c'0c'0c });
        g.fillRect (bounds);
        g.setColour (Colour { 0xff'2a'2a'2a });
        g.drawRect (bounds, 1.0f);

        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded())
        {
            g.setColour (Colour { 0xff'5a'5a'5a });
            g.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 12.0f, Font::plain));
            g.drawText ("(no sample loaded for this slot)",
                        bounds, Justification::centred);
            return;
        }

        const int n = slot->numSamples;
        if (viewEnd_ <= 0 || viewEnd_ > n) viewEnd_ = n;
        if (viewStart_ < 0 || viewStart_ >= viewEnd_) viewStart_ = 0;

        const int vRange = viewEnd_ - viewStart_;
        if (vRange <= 0) return;

        const int   w  = juce::roundToInt (bounds.getWidth());
        const float h  = bounds.getHeight();
        const float midY = bounds.getCentreY();
        const float halfH = h * 0.45f;
        const auto* d = slot->data16L.get();
        if (w <= 0) return;

        /* Selection shading (under the waveform). */
        if (hasSelection())
        {
            const float sx0 = sampleToPixel (selStart_, w);
            const float sx1 = sampleToPixel (selEnd_,   w);
            if (sx1 > sx0)
            {
                g.setColour (Colour { 0x44'40'a0'ff });
                g.fillRect (bounds.getX() + sx0, bounds.getY(),
                            sx1 - sx0, bounds.getHeight());
            }
        }

        /* Loop region shade (over selection, under waveform). */
        if (slot->loopMode != SamplerLoopMode::kNone && slot->loopLength > 0)
        {
            const float lx0 = sampleToPixel (slot->loopStart, w);
            const float lx1 = sampleToPixel (slot->loopStart + slot->loopLength, w);
            if (lx1 > lx0)
            {
                g.setColour (Colour { 0x33'ff'a0'40 });
                g.fillRect (bounds.getX() + lx0, bounds.getY(),
                            lx1 - lx0, bounds.getHeight());
            }
        }

        /* Centerline. */
        g.setColour (Colour { 0xff'2a'2a'2a });
        g.drawHorizontalLine ((int) midY, bounds.getX(), bounds.getRight());

        /* Min/max envelope per pixel column. */
        g.setColour (Colour { 0xff'5a'a5'd0 });
        for (int x = 0; x < w; ++x)
        {
            const int64_t s0 = viewStart_ + (int64_t) x * vRange / w;
            const int64_t s1 = juce::jmax (s0 + 1,
                                            (int64_t) viewStart_ + (int64_t)(x + 1) * vRange / w);
            int mn = INT16_MAX, mx = INT16_MIN;
            for (int64_t i = s0; i < s1 && i < n; ++i)
            {
                const int v = d[i];
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            const float yMin = midY - (mn / 32768.0f) * halfH;
            const float yMax = midY - (mx / 32768.0f) * halfH;
            g.drawLine (bounds.getX() + x, yMax, bounds.getX() + x, yMin);
        }

        /* Selection edge markers. */
        if (hasSelection())
        {
            const float sx0 = sampleToPixel (selStart_, w);
            const float sx1 = sampleToPixel (selEnd_,   w);
            g.setColour (Colour { 0xff'40'a0'ff });
            g.fillRect (bounds.getX() + sx0 - 1.0f, bounds.getY(), 2.0f, bounds.getHeight());
            g.fillRect (bounds.getX() + sx1 - 1.0f, bounds.getY(), 2.0f, bounds.getHeight());
        }

        /* Loop region markers. */
        if (slot->loopMode != SamplerLoopMode::kNone && slot->loopLength > 0)
        {
            const float lx0 = sampleToPixel (slot->loopStart, w);
            const float lx1 = sampleToPixel (slot->loopStart + slot->loopLength, w);
            g.setColour (Colour { 0xff'ff'a0'40 });
            g.fillRect (bounds.getX() + lx0 - 1.0f, bounds.getY(), 2.0f, bounds.getHeight());
            g.fillRect (bounds.getX() + lx1 - 1.0f, bounds.getY(), 2.0f, bounds.getHeight());
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded()) return;
        const int s = pixelToSample (e.x);
        if (e.mods.isShiftDown() && hasSelection())
            setSelection (selStart_, s);
        else
            { dragAnchor_ = s; setSelection (s, s); }
    }
    void mouseDrag (const MouseEvent& e) override
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded()) return;
        if (dragAnchor_ < 0) return;
        setSelection (dragAnchor_, pixelToSample (e.x));
    }
    void mouseUp (const MouseEvent&) override { dragAnchor_ = -1; }
    void mouseDoubleClick (const MouseEvent&) override
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded()) return;
        setSelection (0, slot->numSamples);
    }

private:
    int pixelToSample (int x) const
    {
        const auto bounds = getLocalBounds().reduced (2);
        const int w = bounds.getWidth();
        if (w <= 0) return viewStart_;
        const int vRange = viewEnd_ - viewStart_;
        return juce::jlimit (viewStart_, viewEnd_,
                             viewStart_ + (int) ((int64_t) (x - bounds.getX()) * vRange / w));
    }
    float sampleToPixel (int s, int w) const
    {
        const int vRange = viewEnd_ - viewStart_;
        if (vRange <= 0 || w <= 0) return 0.0f;
        return (float) ((int64_t) (s - viewStart_) * w / vRange);
    }

    GetSlot getSlot;
    RangeChangedCb onRangeChanged;
    int viewStart_ = 0, viewEnd_ = 0;
    int selStart_  = -1, selEnd_  = -1;
    int dragAnchor_ = -1;
};


/* ===========================================================================
 * SamplerSamplePage — Sample editor.  Big waveform + range select + zoom
 * + cut/copy/paste/crop + length/loop info + loop-mode radio.
 *
 * Operations mutate the int16 buffer in place; loopStart/loopLength are
 * adjusted to stay in range when the buffer shrinks/grows.  Clipboard is
 * per-pane state (one shared buffer for all slots within this editor
 * instance — fine for a single-window app).
 * ========================================================================*/
class SamplerSamplePage : public Component
{
public:
    using GetSlot = std::function<SamplerSampleSlot*()>;
    using OnChange = std::function<void()>;

    SamplerSamplePage (GetSlot gs, OnChange cb)
        : getSlot (std::move (gs)),
          onChange (std::move (cb)),
          canvas ([this] { return getSlot ? getSlot() : nullptr; },
                  [this] { refreshInfo(); })
    {
        addAndMakeVisible (canvas);

        configureBtn (cutBtn,    "Cut",       [this] { doCut(); });
        configureBtn (copyBtn,   "Copy",      [this] { doCopy(); });
        configureBtn (pasteBtn,  "Paste",     [this] { doPaste(); });
        configureBtn (cropBtn,   "Crop",      [this] { doCrop(); });
        configureBtn (selAllBtn, "Sel All",   [this] { selectAll(); });
        configureBtn (selNoneBtn,"Sel None",  [this] { canvas.clearSelection(); refreshInfo(); });
        configureBtn (zoomInBtn, "Zoom in",   [this] { zoomToSelection(); });
        configureBtn (zoomOutBtn,"Zoom out",  [this] { zoomOut(); });
        configureBtn (zoomAllBtn,"Show all",  [this] { zoomAll(); });
        configureBtn (loopSetBtn,"Loop=Sel",  [this] { loopFromSelection(); });

        loopRadio.addItem ("Loop: Off",      1);
        loopRadio.addItem ("Loop: Forward",  2);
        loopRadio.addItem ("Loop: Pingpong", 3);
        loopRadio.setColour (ComboBox::backgroundColourId, Colour { 0xff'24'24'24 });
        loopRadio.setColour (ComboBox::textColourId,        Colour { 0xff'd0'd0'd0 });
        loopRadio.onChange = [this] {
            if (auto* s = getSlot ? getSlot() : nullptr)
                s->loopMode = (SamplerLoopMode) (loopRadio.getSelectedId() - 1);
            if (onChange) onChange();
            refreshInfo();
            canvas.repaint();
        };
        addAndMakeVisible (loopRadio);

        for (auto* l : { &infoLength, &infoSel, &infoLoop, &infoView })
        {
            l->setColour (Label::textColourId, Colour { 0xff'b0'b0'b0 });
            l->setFont (FontOptions (Font::getDefaultMonospacedFontName(), 11.0f, Font::plain));
            addAndMakeVisible (*l);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);

        /* Top: action bar. */
        auto topRow = r.removeFromTop (24);
        const int btnW = 70;
        cutBtn   .setBounds (topRow.removeFromLeft (btnW)); topRow.removeFromLeft (3);
        copyBtn  .setBounds (topRow.removeFromLeft (btnW)); topRow.removeFromLeft (3);
        pasteBtn .setBounds (topRow.removeFromLeft (btnW)); topRow.removeFromLeft (3);
        cropBtn  .setBounds (topRow.removeFromLeft (btnW)); topRow.removeFromLeft (10);
        selAllBtn.setBounds (topRow.removeFromLeft (btnW)); topRow.removeFromLeft (3);
        selNoneBtn.setBounds(topRow.removeFromLeft (btnW)); topRow.removeFromLeft (10);
        zoomInBtn.setBounds (topRow.removeFromLeft (btnW)); topRow.removeFromLeft (3);
        zoomOutBtn.setBounds(topRow.removeFromLeft (btnW)); topRow.removeFromLeft (3);
        zoomAllBtn.setBounds(topRow.removeFromLeft (btnW)); topRow.removeFromLeft (10);
        loopSetBtn.setBounds(topRow.removeFromLeft (90));   topRow.removeFromLeft (10);
        loopRadio .setBounds(topRow.removeFromLeft (150));
        r.removeFromTop (8);

        /* Bottom info row. */
        auto bot = r.removeFromBottom (20);
        infoLength.setBounds (bot.removeFromLeft (180));
        infoSel   .setBounds (bot.removeFromLeft (260));
        infoLoop  .setBounds (bot.removeFromLeft (200));
        infoView  .setBounds (bot);
        r.removeFromBottom (4);

        /* Rest: canvas. */
        canvas.setBounds (r);
    }

    /** Called when the page becomes visible or slot changes. */
    void refresh()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s != nullptr && s->numSamples > 0)
        {
            if (canvas.getViewEnd() <= 0 || canvas.getViewEnd() > s->numSamples)
                canvas.setViewport (0, s->numSamples);
        }
        else
        {
            canvas.setViewport (0, 0);
            canvas.clearSelection();
        }
        loopRadio.setSelectedId (s ? (int) s->loopMode + 1 : 1, dontSendNotification);
        refreshInfo();
        canvas.repaint();
    }

private:
    void refreshInfo()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        infoLength.setText (s ? String::formatted ("Length: %d", s->numSamples)
                              : "Length: —", dontSendNotification);
        if (s && canvas.hasSelection())
            infoSel.setText (String::formatted ("Sel: %d..%d (%d)",
                                                  canvas.getSelStart(),
                                                  canvas.getSelEnd(),
                                                  canvas.getSelEnd() - canvas.getSelStart()),
                              dontSendNotification);
        else
            infoSel.setText ("Sel: (none)", dontSendNotification);
        infoLoop.setText (s ? String::formatted ("Loop: %d + %d",
                                                  s->loopStart, s->loopLength)
                            : "Loop: —", dontSendNotification);
        infoView.setText (String::formatted ("View: %d..%d",
                                              canvas.getViewStart(), canvas.getViewEnd()),
                          dontSendNotification);
    }

    void configureBtn (TextButton& b, const String& t, std::function<void()> on)
    {
        b.setButtonText (t);
        b.onClick = std::move (on);
        b.setColour (TextButton::buttonColourId,    Colour { 0xff'24'24'24 });
        b.setColour (TextButton::textColourOffId,   Colour { 0xff'd0'd0'd0 });
        addAndMakeVisible (b);
    }

    void selectAll()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr || ! s->isLoaded()) return;
        canvas.setSelection (0, s->numSamples);
        refreshInfo();
    }
    void zoomAll()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr) return;
        canvas.setViewport (0, juce::jmax (0, s->numSamples));
        refreshInfo();
    }
    void zoomToSelection()
    {
        if (! canvas.hasSelection()) return;
        canvas.setViewport (canvas.getSelStart(), canvas.getSelEnd());
        refreshInfo();
    }
    void zoomOut()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr) return;
        const int vs = canvas.getViewStart();
        const int ve = canvas.getViewEnd();
        const int span = ve - vs;
        const int cx = (vs + ve) / 2;
        const int newSpan = juce::jmin (s->numSamples, span * 2);
        const int newVs = juce::jmax (0, cx - newSpan / 2);
        const int newVe = juce::jmin (s->numSamples, newVs + newSpan);
        canvas.setViewport (newVs, newVe);
        refreshInfo();
    }

    /** Cut: copy selection to clipboard, then splice out of the buffer. */
    void doCut()
    {
        if (! copyToClipboard()) return;
        spliceOutSelection();
    }
    bool doCopy() { return copyToClipboard(); }

    bool copyToClipboard()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr || ! canvas.hasSelection()) return false;
        const int a = canvas.getSelStart();
        const int b = canvas.getSelEnd();
        const int n = juce::jmax (0, b - a);
        if (n <= 0) return false;
        clipL.assign (s->data16L.get() + a, s->data16L.get() + b);
        if (s->isStereo && s->data16R)
        { clipR.assign (s->data16R.get() + a, s->data16R.get() + b); clipStereo = true; }
        else
        { clipR.clear(); clipStereo = false; }
        return true;
    }

    /** Paste: replace selection (or insert at sel-start if no range). */
    void doPaste()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr || clipL.empty()) return;

        const int oldN = s->numSamples;
        const int a = juce::jmax (0, canvas.hasSelection() ? canvas.getSelStart() : oldN);
        const int b = juce::jmax (a, canvas.hasSelection() ? canvas.getSelEnd()   : a);
        const int newN = oldN - (b - a) + (int) clipL.size();

        std::unique_ptr<int16_t[]> newL (new int16_t[(size_t) newN]);
        std::unique_ptr<int16_t[]> newR;
        if (s->isStereo) newR.reset (new int16_t[(size_t) newN]);

        /* Pre-selection. */
        std::memcpy (newL.get(), s->data16L.get(), (size_t) a * sizeof (int16_t));
        if (s->isStereo && s->data16R)
            std::memcpy (newR.get(), s->data16R.get(), (size_t) a * sizeof (int16_t));
        /* Clipboard. */
        std::memcpy (newL.get() + a, clipL.data(), clipL.size() * sizeof (int16_t));
        if (s->isStereo && newR)
        {
            if (clipStereo && ! clipR.empty())
                std::memcpy (newR.get() + a, clipR.data(), clipR.size() * sizeof (int16_t));
            else
                std::memcpy (newR.get() + a, clipL.data(), clipL.size() * sizeof (int16_t));
        }
        /* Post-selection. */
        const int tailLen = oldN - b;
        if (tailLen > 0)
        {
            std::memcpy (newL.get() + a + (int) clipL.size(),
                         s->data16L.get() + b, (size_t) tailLen * sizeof (int16_t));
            if (s->isStereo && newR)
                std::memcpy (newR.get() + a + (int) clipL.size(),
                             s->data16R.get() + b, (size_t) tailLen * sizeof (int16_t));
        }

        s->data16L  = std::move (newL);
        s->data16R  = std::move (newR);
        s->numSamples = newN;
        fixupLoopAfterEdit (s, a, (int) clipL.size() - (b - a));
        canvas.setSelection (a, a + (int) clipL.size());
        zoomAll();
        notifyChange();
    }

    void spliceOutSelection()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr || ! canvas.hasSelection()) return;
        const int a = canvas.getSelStart();
        const int b = canvas.getSelEnd();
        const int oldN = s->numSamples;
        const int newN = oldN - (b - a);
        if (newN <= 0)
        {
            /* Wipe sample entirely. */
            s->data16L.reset();
            s->data16R.reset();
            s->numSamples = 0;
            s->loopStart  = 0;
            s->loopLength = 0;
            canvas.clearSelection();
            zoomAll();
            notifyChange();
            return;
        }
        std::unique_ptr<int16_t[]> newL (new int16_t[(size_t) newN]);
        std::unique_ptr<int16_t[]> newR;
        std::memcpy (newL.get(), s->data16L.get(), (size_t) a * sizeof (int16_t));
        std::memcpy (newL.get() + a, s->data16L.get() + b,
                     (size_t) (oldN - b) * sizeof (int16_t));
        if (s->isStereo && s->data16R)
        {
            newR.reset (new int16_t[(size_t) newN]);
            std::memcpy (newR.get(), s->data16R.get(), (size_t) a * sizeof (int16_t));
            std::memcpy (newR.get() + a, s->data16R.get() + b,
                         (size_t) (oldN - b) * sizeof (int16_t));
        }
        s->data16L  = std::move (newL);
        s->data16R  = std::move (newR);
        s->numSamples = newN;
        fixupLoopAfterEdit (s, a, -(b - a));
        canvas.clearSelection();
        zoomAll();
        notifyChange();
    }

    /** Crop: keep selection, drop everything else. */
    void doCrop()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr || ! canvas.hasSelection()) return;
        const int a = canvas.getSelStart();
        const int b = canvas.getSelEnd();
        const int newN = b - a;
        if (newN <= 0) return;
        std::unique_ptr<int16_t[]> newL (new int16_t[(size_t) newN]);
        std::unique_ptr<int16_t[]> newR;
        std::memcpy (newL.get(), s->data16L.get() + a, (size_t) newN * sizeof (int16_t));
        if (s->isStereo && s->data16R)
        {
            newR.reset (new int16_t[(size_t) newN]);
            std::memcpy (newR.get(), s->data16R.get() + a, (size_t) newN * sizeof (int16_t));
        }
        s->data16L  = std::move (newL);
        s->data16R  = std::move (newR);
        s->numSamples = newN;
        s->loopStart  = juce::jlimit (0, newN - 1,
                                       s->loopStart - a);
        s->loopLength = juce::jlimit (0, newN - s->loopStart, s->loopLength);
        canvas.setSelection (0, newN);
        zoomAll();
        notifyChange();
    }

    void loopFromSelection()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr || ! canvas.hasSelection()) return;
        const int a = canvas.getSelStart();
        const int b = canvas.getSelEnd();
        s->loopStart  = juce::jlimit (0, s->numSamples - 1, a);
        s->loopLength = juce::jlimit (1, s->numSamples - s->loopStart, b - a);
        if (s->loopMode == SamplerLoopMode::kNone)
            s->loopMode = SamplerLoopMode::kForward;
        loopRadio.setSelectedId ((int) s->loopMode + 1, dontSendNotification);
        refreshInfo();
        canvas.repaint();
        notifyChange();
    }

    static void fixupLoopAfterEdit (SamplerSampleSlot* s, int editAt, int delta)
    {
        if (s == nullptr) return;
        const int loopEnd = s->loopStart + s->loopLength;
        const auto shift = [editAt, delta] (int& v) {
            if (v >= editAt) v += delta;
            if (v < 0) v = 0;
        };
        int newStart = s->loopStart;
        int newEnd   = loopEnd;
        shift (newStart);
        shift (newEnd);
        s->loopStart  = juce::jlimit (0, juce::jmax (0, s->numSamples - 1), newStart);
        s->loopLength = juce::jlimit (0, s->numSamples - s->loopStart,
                                       newEnd - s->loopStart);
    }

    void notifyChange()
    {
        if (onChange) onChange();
        refreshInfo();
        canvas.repaint();
    }

    GetSlot getSlot;
    OnChange onChange;
    SampleEditCanvas canvas;

    TextButton cutBtn, copyBtn, pasteBtn, cropBtn;
    TextButton selAllBtn, selNoneBtn;
    TextButton zoomInBtn, zoomOutBtn, zoomAllBtn;
    TextButton loopSetBtn;
    ComboBox   loopRadio;
    Label      infoLength, infoSel, infoLoop, infoView;

    std::vector<int16_t> clipL, clipR;
    bool                 clipStereo = false;
};


/* ===========================================================================
 * SamplerFXPage — sample-DSP effects panel.  Each effect operates on the
 * currently-selected range of the slot's buffer (or the whole sample if
 * no selection).  Algorithms ported from ft2-clone's
 * ft2_sample_ed_features.c (BSD-3) — math kept, SDL UI dropped.
 *
 * Effects implemented:
 *   Amplify   (gain × samples)
 *   Normalize (peak-scale to 1.0)
 *   LP filter (single-pole, normalised cutoff 0..1)
 *   HP filter (single-pole, normalised cutoff 0..1)
 *   Generator (sine / square / triangle / saw — replace or mix)
 *   X-Fade    (cross-fade loop boundary smoothing)
 *   Reverse   (flip selection)
 * ========================================================================*/
class SamplerFXPage : public Component
{
public:
    using GetSlot  = std::function<SamplerSampleSlot*()>;
    using OnChange = std::function<void()>;

    SamplerFXPage (GetSlot gs, OnChange cb)
        : getSlot (std::move (gs)),
          onChange (std::move (cb))
    {
        configureBtn (amplifyBtn,  "Amplify",   [this] { applyAmplify(); });
        configureBtn (normalizeBtn,"Normalize", [this] { applyNormalize(); });
        configureBtn (reverseBtn,  "Reverse",   [this] { applyReverse(); });
        configureBtn (lpBtn,       "Low-pass",  [this] { applyFilter (true); });
        configureBtn (hpBtn,       "High-pass", [this] { applyFilter (false); });
        configureBtn (genBtn,      "Generate",  [this] { applyGenerator(); });
        configureBtn (xfadeBtn,    "X-Fade loop", [this] { applyXFade(); });

        configureSlider (gainSlider,   0.0,   400.0, 0.1,  "%");
        gainSlider.setValue (100.0);
        configureSlider (cutoffSlider, 0.001, 1.0,   0.001, "norm");
        cutoffSlider.setValue (0.25);
        configureSlider (cyclesSlider, 0.5,   64.0,  0.5,  "cyc");
        cyclesSlider.setValue (1.0);
        configureSlider (amplitudeSlider, 0.0, 100.0, 0.1, "%");
        amplitudeSlider.setValue (100.0);
        configureSlider (xfadeLenSlider, 1.0,  4096.0, 1.0, "smp");
        xfadeLenSlider.setValue (64.0);

        genWaveCombo.addItem ("Sine",     1);
        genWaveCombo.addItem ("Square",   2);
        genWaveCombo.addItem ("Triangle", 3);
        genWaveCombo.addItem ("Sawtooth", 4);
        genWaveCombo.setSelectedId (1, dontSendNotification);
        genWaveCombo.setColour (ComboBox::backgroundColourId, Colour { 0xff'24'24'24 });
        genWaveCombo.setColour (ComboBox::textColourId,        Colour { 0xff'd0'd0'd0 });
        addAndMakeVisible (genWaveCombo);

        genMixToggle.setButtonText ("Mix (vs replace)");
        genMixToggle.setColour (ToggleButton::textColourId, Colour { 0xff'b0'b0'b0 });
        addAndMakeVisible (genMixToggle);

        for (auto* l : { &gainLbl, &cutoffLbl, &cyclesLbl, &amplLbl, &xfadeLenLbl, &statusLbl })
        {
            l->setColour (Label::textColourId, Colour { 0xff'b0'b0'b0 });
            l->setFont (FontOptions (Font::getDefaultMonospacedFontName(), 11.0f, Font::plain));
            addAndMakeVisible (*l);
        }
        gainLbl    .setText ("Gain (%):",   dontSendNotification);
        cutoffLbl  .setText ("Cutoff:",     dontSendNotification);
        cyclesLbl  .setText ("Cycles:",     dontSendNotification);
        amplLbl    .setText ("Amplitude:",  dontSendNotification);
        xfadeLenLbl.setText ("X-Fade len:", dontSendNotification);

        statusLbl.setText ("FX operate on the selection (or whole sample if no selection).",
                           dontSendNotification);
        statusLbl.setJustificationType (Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);

        auto row = [&] (const Label& l, Slider& s, TextButton& btn) {
            auto rr = r.removeFromTop (28);
            const_cast<Label&> (l).setBounds (rr.removeFromLeft (110));
            btn.setBounds (rr.removeFromRight (110));
            rr.removeFromRight (8);
            s.setBounds (rr);
            r.removeFromTop (4);
        };

        row (gainLbl,        gainSlider,      amplifyBtn);
        {
            auto rr = r.removeFromTop (28);
            rr.removeFromLeft (110);
            normalizeBtn.setBounds (rr.removeFromRight (110));
            reverseBtn  .setBounds (rr.removeFromRight (110));
            r.removeFromTop (4);
        }
        row (cutoffLbl,      cutoffSlider,    lpBtn);
        {
            auto rr = r.removeFromTop (28);
            rr.removeFromLeft (110);
            hpBtn.setBounds (rr.removeFromRight (110));
            r.removeFromTop (8);
        }

        /* Generator block: combo + cycles + amplitude + mix + button. */
        {
            auto rr = r.removeFromTop (28);
            rr.removeFromLeft (110);
            genWaveCombo.setBounds (rr.removeFromLeft (120));
            rr.removeFromLeft (8);
            genMixToggle.setBounds (rr.removeFromLeft (140));
            r.removeFromTop (4);
        }
        row (cyclesLbl,    cyclesSlider,    genBtn);
        row (amplLbl,      amplitudeSlider, xfadeBtn);
        row (xfadeLenLbl,  xfadeLenSlider,  xfadeBtn);   /* shares button row visually */

        r.removeFromTop (8);
        statusLbl.setBounds (r.removeFromTop (20));
    }

private:
    void configureBtn (TextButton& b, const String& t, std::function<void()> on)
    {
        b.setButtonText (t);
        b.onClick = std::move (on);
        b.setColour (TextButton::buttonColourId,    Colour { 0xff'24'24'24 });
        b.setColour (TextButton::textColourOffId,   Colour { 0xff'd0'd0'd0 });
        addAndMakeVisible (b);
    }
    void configureSlider (Slider& s, double lo, double hi, double step, const String& suffix)
    {
        s.setSliderStyle (Slider::LinearBar);
        s.setRange (lo, hi, step);
        s.setTextValueSuffix ("  " + suffix);
        s.setColour (Slider::backgroundColourId, Colour { 0xff'24'24'24 });
        s.setColour (Slider::trackColourId,      Colour { 0xff'4a'7a'b5 });
        addAndMakeVisible (s);
    }

    /** Operate on the active selection if any; otherwise whole sample. */
    bool getRange (int& a, int& b)
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr || ! s->isLoaded()) return false;
        a = 0;
        b = s->numSamples;
        if (selStart_ >= 0 && selEnd_ > selStart_)
        {
            a = juce::jmax (0, selStart_);
            b = juce::jmin (s->numSamples, selEnd_);
        }
        return b > a;
    }

    /** External link — called by the editor when the user selects a
     *  range in the Sample page.  FX page then operates on that range. */
public:
    void setSelection (int a, int b) { selStart_ = a; selEnd_ = b; }
private:

    void mutateInt16 (std::function<int16_t (int16_t)> fn,
                      std::function<int16_t (int16_t)> fnR = {})
    {
        auto* s = getSlot ? getSlot() : nullptr;
        int a, b;
        if (! getRange (a, b)) return;
        for (int i = a; i < b; ++i)
            s->data16L[i] = fn (s->data16L[i]);
        if (s->isStereo && s->data16R)
            for (int i = a; i < b; ++i)
                s->data16R[i] = (fnR ? fnR : fn) (s->data16R[i]);
        notifyChange();
    }

    void applyAmplify()
    {
        const double g = gainSlider.getValue() / 100.0;
        mutateInt16 ([g] (int16_t v) {
            const int x = (int) std::lround (v * g);
            return (int16_t) juce::jlimit (-32768, 32767, x);
        });
    }

    void applyNormalize()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        int a, b;
        if (! getRange (a, b)) return;
        int peak = 1;
        for (int i = a; i < b; ++i)
            peak = juce::jmax (peak, (int) std::abs ((int) s->data16L[i]));
        if (s->isStereo && s->data16R)
            for (int i = a; i < b; ++i)
                peak = juce::jmax (peak, (int) std::abs ((int) s->data16R[i]));
        if (peak <= 0) return;
        const double target = 32767.0 * (amplitudeSlider.getValue() / 100.0);
        const double g = target / (double) peak;
        mutateInt16 ([g] (int16_t v) {
            const int x = (int) std::lround (v * g);
            return (int16_t) juce::jlimit (-32768, 32767, x);
        });
    }

    void applyReverse()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        int a, b;
        if (! getRange (a, b)) return;
        for (int i = a, j = b - 1; i < j; ++i, --j)
        {
            std::swap (s->data16L[i], s->data16L[j]);
            if (s->isStereo && s->data16R)
                std::swap (s->data16R[i], s->data16R[j]);
        }
        notifyChange();
    }

    /** Single-pole IIR filter.  cutoff = normalised 0..1 (1 = nyquist).
     *  y[n] = y[n-1] + alpha * (x[n] - y[n-1])  — LP.
     *  HP form: y[n] = a * (y[n-1] + x[n] - x[n-1]). */
    void applyFilter (bool lowPass)
    {
        auto* s = getSlot ? getSlot() : nullptr;
        int a, b;
        if (! getRange (a, b)) return;
        const double cutoff = juce::jlimit (0.001, 1.0, cutoffSlider.getValue());
        const double alpha  = cutoff;

        auto runLP = [alpha] (int16_t* d, int from, int to) {
            double y = (double) d[from];
            for (int i = from; i < to; ++i)
            {
                y += alpha * ((double) d[i] - y);
                d[i] = (int16_t) juce::jlimit (-32768, 32767, (int) std::lround (y));
            }
        };
        auto runHP = [alpha] (int16_t* d, int from, int to) {
            double y = 0.0, prev = (double) d[from];
            const double aCoef = 1.0 - alpha;
            for (int i = from; i < to; ++i)
            {
                const double x = (double) d[i];
                y = aCoef * (y + x - prev);
                prev = x;
                d[i] = (int16_t) juce::jlimit (-32768, 32767, (int) std::lround (y));
            }
        };
        if (lowPass)
        {
            runLP (s->data16L.get(), a, b);
            if (s->isStereo && s->data16R) runLP (s->data16R.get(), a, b);
        }
        else
        {
            runHP (s->data16L.get(), a, b);
            if (s->isStereo && s->data16R) runHP (s->data16R.get(), a, b);
        }
        notifyChange();
    }

    void applyGenerator()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        int a, b;
        if (! getRange (a, b)) return;
        const int   length = b - a;
        const double cycles = cyclesSlider.getValue();
        const double amp    = (amplitudeSlider.getValue() / 100.0) * 32767.0;
        const bool mix      = genMixToggle.getToggleState();
        const int wave      = genWaveCombo.getSelectedId();   /* 1..4 */

        auto sampleAt = [&] (int i) -> int16_t {
            const double phase = (double) i / (double) length * cycles;
            const double frac  = phase - std::floor (phase);
            double y = 0.0;
            switch (wave)
            {
                case 1: y = std::sin (frac * juce::MathConstants<double>::twoPi); break;
                case 2: y = frac < 0.5 ? 1.0 : -1.0; break;
                case 3: y = frac < 0.5 ? (-1.0 + 4.0 * frac)
                                       : ( 3.0 - 4.0 * frac); break;
                case 4: y = 2.0 * frac - 1.0; break;
                default: break;
            }
            return (int16_t) juce::jlimit (-32768, 32767,
                                            (int) std::lround (y * amp));
        };

        for (int i = a; i < b; ++i)
        {
            const int16_t g = sampleAt (i - a);
            if (mix)
            {
                const int sum = (int) s->data16L[i] + (int) g;
                s->data16L[i] = (int16_t) juce::jlimit (-32768, 32767, sum);
                if (s->isStereo && s->data16R)
                {
                    const int sumR = (int) s->data16R[i] + (int) g;
                    s->data16R[i] = (int16_t) juce::jlimit (-32768, 32767, sumR);
                }
            }
            else
            {
                s->data16L[i] = g;
                if (s->isStereo && s->data16R) s->data16R[i] = g;
            }
        }
        notifyChange();
    }

    /** X-fade the loop boundary.  Linear cross-fade across N samples
     *  around loopStart and loopStart+loopLength using neighbours. */
    void applyXFade()
    {
        auto* s = getSlot ? getSlot() : nullptr;
        if (s == nullptr || ! s->isLoaded()) return;
        if (s->loopLength <= 0) return;
        const int N = juce::jlimit (1, juce::jmin (s->loopLength, 4096),
                                     (int) xfadeLenSlider.getValue());
        const int loopEnd = s->loopStart + s->loopLength;
        const int leftIn  = juce::jmax (0, s->loopStart - N / 2);
        const int rightIn = juce::jmin (s->numSamples, loopEnd + N / 2);

        auto fade = [&] (int16_t* d) {
            for (int i = 0; i < N && (loopEnd + i) < s->numSamples; ++i)
            {
                const double t = (double) i / (double) N;
                const int srcA = loopEnd + i;
                const int srcB = s->loopStart + i;
                const int x = (int) std::lround (d[srcA] * (1.0 - t) + d[srcB] * t);
                d[srcA] = (int16_t) juce::jlimit (-32768, 32767, x);
            }
        };
        fade (s->data16L.get());
        if (s->isStereo && s->data16R) fade (s->data16R.get());
        juce::ignoreUnused (leftIn, rightIn);
        notifyChange();
    }

    void notifyChange()
    {
        if (onChange) onChange();
    }

    GetSlot  getSlot;
    OnChange onChange;
    int      selStart_ = -1, selEnd_ = -1;

    TextButton amplifyBtn, normalizeBtn, reverseBtn, lpBtn, hpBtn, genBtn, xfadeBtn;
    Slider     gainSlider, cutoffSlider, cyclesSlider, amplitudeSlider, xfadeLenSlider;
    ComboBox   genWaveCombo;
    ToggleButton genMixToggle;
    Label      gainLbl, cutoffLbl, cyclesLbl, amplLbl, xfadeLenLbl, statusLbl;
};


/* ===========================================================================
 * SamplerNodeEditor — paged container
 *   - Top nav: Bank | Inst | Sample | FX  +  instrument selector
 *   - Body: one of four pages, switched by the nav tabs
 *
 *   Bank   — slot list, per-slot params, waveform view (existing).
 *   Inst   — full envelope editor + keymap + per-instrument extras.
 *   Sample — sample editor (waveform + range + cut/copy/paste). [F2]
 *   FX     — sample effects (lp/hp/resonance/generators/normalize). [F3]
 * ========================================================================*/
class SamplerNodeEditor : public AudioProcessorEditor,
                          private Timer,
                          private ListBoxModel
{
public:
    enum class Page { kBank = 0, kInst, kSample, kFX };

    explicit SamplerNodeEditor (SamplerNode& s) : AudioProcessorEditor (s), node (s),
        waveformView ([this] { return currentSlot(); },
                      [this] (const SamplerSampleSlot* slot) {
                          return node.collectPlayheadsForSlot (slot);
                      }),
        instPage_   ([this] { return node.getInstrument (activeInstrument); },
                     [this] { return activeSlot; },
                     [this] (int s) {
                         activeSlot = juce::jlimit (0, SamplerInstrument::kNumSlots - 1, s);
                         slotList.selectRow (activeSlot, true, false);
                         refresh();
                     }),
        samplePage_ ([this] { return currentSlot(); },
                     [this] {
                         waveformView.repaint();
                         slotList.repaint();
                     }),
        fxPage_     ([this] { return currentSlot(); },
                     [this] {
                         waveformView.repaint();
                         slotList.repaint();
                     })
    {
        /* === Nav tabs (Bank / Inst / Sample / FX). === */
        auto setupNav = [this] (TextButton& b, const String& text, Page p) {
            b.setButtonText (text);
            b.setClickingTogglesState (true);
            b.setRadioGroupId (0xb01);
            b.setColour (TextButton::buttonColourId,    Colour { 0xff'24'24'24 });
            b.setColour (TextButton::buttonOnColourId,  Colour { 0xff'4a'7a'b5 });
            b.setColour (TextButton::textColourOffId,   Colour { 0xff'b0'b0'b0 });
            b.setColour (TextButton::textColourOnId,    Colours::white);
            b.onClick = [this, p] { switchPage (p); };
            addAndMakeVisible (b);
        };
        setupNav (bankNavBtn,   "Bank",   Page::kBank);
        setupNav (instNavBtn,   "Inst",   Page::kInst);
        setupNav (sampleNavBtn, "Sample", Page::kSample);
        setupNav (fxNavBtn,     "FX",     Page::kFX);
        bankNavBtn.setToggleState (true, dontSendNotification);

        addChildComponent (instPage_);
        addChildComponent (samplePage_);
        addChildComponent (fxPage_);
        instCombo.onChange = [this] {
            const int sel = instCombo.getSelectedId() - 1;
            if (sel >= 0 && sel < node.getNumInstruments())
            {
                activeInstrument = sel;
                rebuildEnvelopeViews();
                refresh();
            }
        };
        addAndMakeVisible (instCombo);

        instAddBtn.setButtonText ("+");
        instAddBtn.onClick = [this] {
            node.addInstrument();
            activeInstrument = node.getNumInstruments() - 1;
            rebuildInstCombo();
            rebuildEnvelopeViews();
            refresh();
        };
        addAndMakeVisible (instAddBtn);

        instDelBtn.setButtonText ("-");
        instDelBtn.onClick = [this] {
            if (node.getNumInstruments() <= 1) return;
            node.removeInstrument (activeInstrument);
            if (activeInstrument >= node.getNumInstruments())
                activeInstrument = node.getNumInstruments() - 1;
            rebuildInstCombo();
            rebuildEnvelopeViews();
            refresh();
        };
        addAndMakeVisible (instDelBtn);

        slotList.setModel (this);
        slotList.setRowHeight (24);
        slotList.setColour (ListBox::backgroundColourId, Colour { 0xff'14'14'14 });
        slotList.setColour (ListBox::outlineColourId,    Colour { 0xff'2a'2a'2a });
        slotList.setOutlineThickness (1);
        addAndMakeVisible (slotList);

        loadBtn.setButtonText ("Load from Disk Op");
        loadBtn.onClick = [this] { onLoad(); };
        addAndMakeVisible (loadBtn);

        clearBtn.setButtonText ("Clear");
        clearBtn.onClick = [this] {
            if (auto inst = node.getInstrument (activeInstrument))
                inst->clearSlot (activeSlot);
            refresh();
        };
        addAndMakeVisible (clearBtn);

        configureParamSlider (rootSlider, "Root", 0.0, 127.0, 1.0,
            [this](double v) { if (auto* s = currentSlot()) s->rootNote = (int) v; });
        configureParamSlider (relSlider,  "Rel",  -96.0, 96.0, 1.0,
            [this](double v) { if (auto* s = currentSlot()) s->relativeNote = (int) v; });
        configureParamSlider (fineSlider, "Fine", -128.0, 127.0, 1.0,
            [this](double v) { if (auto* s = currentSlot()) s->finetune = (int) v; });
        configureParamSlider (volSlider, "Vol",   0.0, 1.0, 0.001,
            [this](double v) { if (auto* s = currentSlot()) s->volume = (float) v; });
        configureParamSlider (panSlider, "Pan",   0.0, 1.0, 0.001,
            [this](double v) { if (auto* s = currentSlot()) s->panning = (float) v; });

        loopCombo.addItem ("Loop: Off",      1);
        loopCombo.addItem ("Loop: Forward",  2);
        loopCombo.addItem ("Loop: Ping-pong", 3);
        loopCombo.onChange = [this] {
            if (auto* s = currentSlot())
                s->loopMode = (SamplerLoopMode) (loopCombo.getSelectedId() - 1);
            waveformView.repaint();
        };
        addAndMakeVisible (loopCombo);

        addAndMakeVisible (waveformView);

        /* Tab strip: Vol Env / Pan Env / AutoVib+Fadeout. */
        tabBar.addTab ("Vol",     Colour { 0xff'18'18'18 }, &volEnvWrap,    false);
        tabBar.addTab ("Pan",     Colour { 0xff'18'18'18 }, &panEnvWrap,    false);
        tabBar.addTab ("AutoVib", Colour { 0xff'18'18'18 }, &autoVibWrap,   false);
        tabBar.setTabBarDepth (22);
        addAndMakeVisible (tabBar);

        /* Add / Del envelope-point buttons inside the Bank-page Vol/Pan
         * tabs.  Shared logic with the Inst page so envelopes are
         * editable from either place. */
        auto setupEnvBtn = [] (TextButton& b, const String& t) {
            b.setButtonText (t);
            b.setColour (TextButton::buttonColourId, Colour { 0xff'24'24'24 });
            b.setColour (TextButton::textColourOffId, Colour { 0xff'd0'd0'd0 });
        };
        setupEnvBtn (volTabAddBtn, "Add"); setupEnvBtn (volTabDelBtn, "Del");
        setupEnvBtn (panTabAddBtn, "Add"); setupEnvBtn (panTabDelBtn, "Del");
        volTabAddBtn.onClick = [this] { addEnvPointBank (true);  };
        volTabDelBtn.onClick = [this] { delEnvPointBank (true);  };
        panTabAddBtn.onClick = [this] { addEnvPointBank (false); };
        panTabDelBtn.onClick = [this] { delEnvPointBank (false); };

        configureParamSlider (fadeoutSlider, "Fade", 0.0, 4095.0, 1.0,
            [this](double v) {
                if (auto inst = node.getInstrument (activeInstrument))
                    inst->fadeoutRate = (uint16_t) v;
            });
        autoVibWrap.addAndMakeVisible (fadeoutSlider);

        configureParamSlider (avDepthSlider, "AV Depth", 0.0, 15.0, 1.0,
            [this](double v) {
                if (auto inst = node.getInstrument (activeInstrument))
                    inst->autoVib.depth = (uint8_t) v;
            });
        configureParamSlider (avRateSlider, "AV Rate", 0.0, 63.0, 1.0,
            [this](double v) {
                if (auto inst = node.getInstrument (activeInstrument))
                    inst->autoVib.rate = (uint8_t) v;
            });
        configureParamSlider (avSweepSlider, "AV Sweep", 0.0, 255.0, 1.0,
            [this](double v) {
                if (auto inst = node.getInstrument (activeInstrument))
                    inst->autoVib.sweep = (uint8_t) v;
            });
        avTypeCombo.addItem ("Sine", 1);
        avTypeCombo.addItem ("Square", 2);
        avTypeCombo.addItem ("Ramp Up", 3);
        avTypeCombo.addItem ("Ramp Dn", 4);
        avTypeCombo.onChange = [this] {
            if (auto inst = node.getInstrument (activeInstrument))
                inst->autoVib.type = (uint8_t) (avTypeCombo.getSelectedId() - 1);
        };
        autoVibWrap.addAndMakeVisible (avDepthSlider);
        autoVibWrap.addAndMakeVisible (avRateSlider);
        autoVibWrap.addAndMakeVisible (avSweepSlider);
        autoVibWrap.addAndMakeVisible (avTypeCombo);

        autoVibWrap.onResized = [this] {
            auto r = autoVibWrap.getLocalBounds().reduced (4);
            const int rowH = 22;
            fadeoutSlider.setBounds (r.removeFromTop (rowH)); r.removeFromTop (2);
            avDepthSlider.setBounds (r.removeFromTop (rowH)); r.removeFromTop (2);
            avRateSlider .setBounds (r.removeFromTop (rowH)); r.removeFromTop (2);
            avSweepSlider.setBounds (r.removeFromTop (rowH)); r.removeFromTop (2);
            avTypeCombo  .setBounds (r.removeFromTop (rowH));
        };

        interpCombo.addItem ("None",   1);
        interpCombo.addItem ("Linear", 2);
        interpCombo.addItem ("Cubic",  3);
        interpCombo.addItem ("Sinc16", 4);
        interpCombo.onChange = [this] {
            node.setInterpMode ((SamplerNode::InterpMode) (interpCombo.getSelectedId() - 1));
        };
        addAndMakeVisible (interpCombo);

        status.setJustificationType (Justification::centredLeft);
        status.setColour (Label::textColourId, Colour { 0xff'b0'b0'b0 });
        status.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 11.0f, Font::plain));
        addAndMakeVisible (status);

        setOpaque (true);
        setSize (1040, 600);
        rebuildInstCombo();
        rebuildEnvelopeViews();
        switchPage (Page::kBank);
        refresh();
        startTimerHz (8);
    }

    ~SamplerNodeEditor() override { stopTimer(); }

    void paint (Graphics& g) override
    {
        g.fillAll (Colour { 0xff'18'18'18 });
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);

        /* === Nav strip: Bank | Inst | Sample | FX + instrument combo. === */
        auto navRow = r.removeFromTop (26);
        const int navBtnW = 56;
        bankNavBtn  .setBounds (navRow.removeFromLeft (navBtnW)); navRow.removeFromLeft (2);
        instNavBtn  .setBounds (navRow.removeFromLeft (navBtnW)); navRow.removeFromLeft (2);
        sampleNavBtn.setBounds (navRow.removeFromLeft (navBtnW)); navRow.removeFromLeft (2);
        fxNavBtn    .setBounds (navRow.removeFromLeft (navBtnW)); navRow.removeFromLeft (12);
        instAddBtn.setBounds (navRow.removeFromRight (24));
        navRow.removeFromRight (2);
        instDelBtn.setBounds (navRow.removeFromRight (24));
        navRow.removeFromRight (4);
        instCombo .setBounds (navRow);
        r.removeFromTop (6);

        /* === Body area for the active page. === */
        const auto body = r;

        /* Inst / Sample / FX pages fill the body when active. */
        instPage_  .setBounds (body);
        samplePage_.setBounds (body);
        fxPage_    .setBounds (body);

        if (currentPage_ != Page::kBank)
            return;

        /* === Bank page: slot list fills full height on the left, right
         * pane gets per-slot params, large waveform, tabbed envelopes,
         * interp + status pinned at the bottom. === */
        auto bankR = body;

        /* Left column: load/clear pinned at bottom, slot list fills above. */
        auto leftCol = bankR.removeFromLeft (270);
        auto leftFooter = leftCol.removeFromBottom (24);
        clearBtn.setBounds (leftFooter.removeFromRight (50)); leftFooter.removeFromRight (4);
        loadBtn .setBounds (leftFooter);
        leftCol.removeFromBottom (6);
        slotList.setBounds (leftCol);

        bankR.removeFromLeft (12);

        /* Right column: footer (interp + status) pinned at bottom. */
        auto rightFooter = bankR.removeFromBottom (32);
        rightFooter.removeFromBottom (4);
        auto interpRow = rightFooter.removeFromTop (26);
        interpCombo.setBounds (interpRow.removeFromLeft (140));
        interpRow.removeFromLeft (8);
        status.setBounds (interpRow);

        /* Right column: per-slot param sliders at top. */
        const int rowH = 22;
        rootSlider .setBounds (bankR.removeFromTop (rowH)); bankR.removeFromTop (2);
        relSlider  .setBounds (bankR.removeFromTop (rowH)); bankR.removeFromTop (2);
        fineSlider .setBounds (bankR.removeFromTop (rowH)); bankR.removeFromTop (2);
        volSlider  .setBounds (bankR.removeFromTop (rowH)); bankR.removeFromTop (2);
        panSlider  .setBounds (bankR.removeFromTop (rowH)); bankR.removeFromTop (4);
        loopCombo.setBounds (bankR.removeFromTop (rowH));   bankR.removeFromTop (6);

        /* Remaining vertical splits 55% waveform / 45% tabbed envelope. */
        const int splitH = bankR.getHeight();
        const int waveH = juce::jmax (140, (splitH - 8) * 11 / 20);
        waveformView.setBounds (bankR.removeFromTop (waveH));
        bankR.removeFromTop (8);
        tabBar.setBounds (bankR);
    }

    /** Switch the visible body page and update tab toggle state. */
    void switchPage (Page p)
    {
        currentPage_ = p;
        const bool bank   = p == Page::kBank;
        const bool inst   = p == Page::kInst;
        const bool sample = p == Page::kSample;
        const bool fx     = p == Page::kFX;

        bankNavBtn  .setToggleState (bank,   dontSendNotification);
        instNavBtn  .setToggleState (inst,   dontSendNotification);
        sampleNavBtn.setToggleState (sample, dontSendNotification);
        fxNavBtn    .setToggleState (fx,     dontSendNotification);

        slotList     .setVisible (bank);
        loadBtn      .setVisible (bank);
        clearBtn     .setVisible (bank);
        rootSlider   .setVisible (bank);
        relSlider    .setVisible (bank);
        fineSlider   .setVisible (bank);
        volSlider    .setVisible (bank);
        panSlider    .setVisible (bank);
        loopCombo    .setVisible (bank);
        waveformView .setVisible (bank);
        tabBar       .setVisible (bank);
        interpCombo  .setVisible (bank);
        status       .setVisible (bank);

        instPage_   .setVisible (inst);
        samplePage_ .setVisible (sample);
        fxPage_     .setVisible (fx);

        if (inst)   instPage_.refresh();
        if (sample) samplePage_.refresh();
        resized();
    }

    /* === ListBoxModel ================================================= */

    int getNumRows() override { return SamplerInstrument::kNumSlots; }

    void paintListBoxItem (int row, Graphics& g, int width, int height,
                           bool rowIsSelected) override
    {
        if (rowIsSelected)
        {
            g.setColour (Colour { 0xff'40'80'b0 }.withAlpha (0.4f));
            g.fillRect (0, 0, width, height);
        }

        auto inst = node.getInstrument (activeInstrument);
        const auto* slot = inst ? inst->getSlot (row) : nullptr;

        g.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 12.0f, Font::plain));

        g.setColour (slot != nullptr ? Colour { 0xff'd4'd4'd4 } : Colour { 0xff'5a'5a'5a });
        g.drawText (String::formatted ("%02d", row + 1), 6, 0, 28, height,
                    Justification::centredLeft);

        int lo = -1, hi = -1;
        if (inst && slot)
        {
            for (int n = 0; n < 128; ++n)
            {
                if (inst->slotForNote (n) == row)
                {
                    if (lo < 0) lo = n;
                    hi = n;
                }
            }
        }
        const String rng = (lo >= 0 && hi >= 0)
                              ? (midiNoteName (lo) + "  " + midiNoteName (hi))
                              : String ("        ");
        g.setColour (Colour { 0xff'd0'80'40 });
        g.drawText (rng, 38, 0, 80, height, Justification::centredLeft);

        g.setColour (slot != nullptr ? Colour { 0xff'b0'b0'b0 } : Colour { 0xff'40'40'40 });
        g.drawText (slot != nullptr ? slot->name : String (juce::CharPointer_UTF8 ("\xe2\x80\x94")),
                    124, 0, width - 130, height, Justification::centredLeft);
    }

    void selectedRowsChanged (int lastRowSelected) override
    {
        if (lastRowSelected >= 0)
        {
            activeSlot = lastRowSelected;
            refresh();
        }
    }

    void listBoxItemDoubleClicked (int row, const MouseEvent&) override
    {
        activeSlot = row;
        slotList.selectRow (row);
        onLoad();    /* pulls from DiskOpService selection if any */
    }

private:
    /** A simple Component that calls a std::function on resize.  Used as
     *  the tabbed-content container so we can lay out child controls. */
    struct ResizableWrap : public Component
    {
        std::function<void()> onResized;
        void resized() override { if (onResized) onResized(); }
    };

    void timerCallback() override { refresh(); }

    void configureParamSlider (Slider& s, const String& label, double lo, double hi,
                               double step, std::function<void (double)> on)
    {
        s.setSliderStyle (Slider::LinearBar);
        s.setRange (lo, hi, step);
        s.setTextValueSuffix ("  " + label);
        s.onValueChange = [&s, on] { on (s.getValue()); };
        s.setColour (Slider::backgroundColourId, Colour { 0xff'24'24'24 });
        s.setColour (Slider::trackColourId,      Colour { 0xff'4a'7a'b5 });
        addAndMakeVisible (s);
    }

    SamplerSampleSlot* currentSlot()
    {
        if (auto inst = node.getInstrument (activeInstrument))
            return inst->getSlot (activeSlot);
        return nullptr;
    }

    void rebuildInstCombo()
    {
        instCombo.clear (dontSendNotification);
        const int n = juce::jmax (1, node.getNumInstruments());
        for (int i = 0; i < n; ++i)
        {
            auto inst = node.getInstrument (i);
            String label = String::formatted ("%02d ", i + 1);
            if (inst != nullptr && inst->name.isNotEmpty()) label += inst->name;
            else                                            label += "(empty)";
            instCombo.addItem (label, i + 1);
        }
        instCombo.setSelectedId (activeInstrument + 1, dontSendNotification);
    }

    void rebuildEnvelopeViews()
    {
        instPage_.rebuildEnvViews();
        volEnvView.reset();
        panEnvView.reset();
        if (auto inst = node.getInstrument (activeInstrument))
        {
            volEnvView.reset (new FT2EnvelopeView (
                inst->volumeEnv, "Volume Env",
                [this] { /* changes are direct on the struct; just repaint */ }));
            panEnvView.reset (new FT2EnvelopeView (
                inst->panEnv,    "Pan Env",
                [this] { }));
            volEnvWrap.removeAllChildren();
            panEnvWrap.removeAllChildren();
            volEnvWrap.addAndMakeVisible (volEnvView.get());
            panEnvWrap.addAndMakeVisible (panEnvView.get());
            volEnvWrap.addAndMakeVisible (volTabAddBtn);
            volEnvWrap.addAndMakeVisible (volTabDelBtn);
            panEnvWrap.addAndMakeVisible (panTabAddBtn);
            panEnvWrap.addAndMakeVisible (panTabDelBtn);
            volEnvWrap.onResized = [this] {
                auto r = volEnvWrap.getLocalBounds().reduced (2);
                auto btnRow = r.removeFromBottom (22);
                volTabAddBtn.setBounds (btnRow.removeFromLeft (60));
                btnRow.removeFromLeft (4);
                volTabDelBtn.setBounds (btnRow.removeFromLeft (60));
                r.removeFromBottom (3);
                if (volEnvView) volEnvView->setBounds (r);
            };
            panEnvWrap.onResized = [this] {
                auto r = panEnvWrap.getLocalBounds().reduced (2);
                auto btnRow = r.removeFromBottom (22);
                panTabAddBtn.setBounds (btnRow.removeFromLeft (60));
                btnRow.removeFromLeft (4);
                panTabDelBtn.setBounds (btnRow.removeFromLeft (60));
                r.removeFromBottom (3);
                if (panEnvView) panEnvView->setBounds (r);
            };
            volEnvWrap.resized();
            panEnvWrap.resized();

            fadeoutSlider.setValue ((double) inst->fadeoutRate, dontSendNotification);
            avDepthSlider.setValue ((double) inst->autoVib.depth, dontSendNotification);
            avRateSlider .setValue ((double) inst->autoVib.rate,  dontSendNotification);
            avSweepSlider.setValue ((double) inst->autoVib.sweep, dontSendNotification);
            avTypeCombo  .setSelectedId ((int) inst->autoVib.type + 1, dontSendNotification);
        }
    }

    /* Bank-page envelope point add/del — mirrors SamplerInstPage's
     * Add/Del so envelope editing works from the Vol/Pan tab too. */
    void addEnvPointBank (bool isVol)
    {
        auto inst = node.getInstrument (activeInstrument);
        if (inst == nullptr) return;
        FT2Envelope& e = isVol ? inst->volumeEnv : inst->panEnv;
        if (e.length >= 12) return;
        if (e.length == 0) { e.points[0] = { 0, 64 }; e.length = 1; }
        const int prevX = e.points[e.length - 1].x;
        const int prevY = e.points[e.length - 1].y;
        const int newX  = juce::jmin (324, prevX + 20);
        e.points[e.length] = { (int16_t) newX, (int16_t) prevY };
        e.length = (uint8_t) (e.length + 1);
        if (volEnvView) volEnvView->repaint();
        if (panEnvView) panEnvView->repaint();
        instPage_.refresh();
    }
    void delEnvPointBank (bool isVol)
    {
        auto inst = node.getInstrument (activeInstrument);
        if (inst == nullptr) return;
        FT2Envelope& e = isVol ? inst->volumeEnv : inst->panEnv;
        if (e.length <= 2) return;
        e.length = (uint8_t) (e.length - 1);
        if (e.sustainPoint >= e.length) e.sustainPoint = (uint8_t) (e.length - 1);
        if (e.loopStart   >= e.length) e.loopStart   = (uint8_t) (e.length - 1);
        if (e.loopEnd     >= e.length) e.loopEnd     = (uint8_t) (e.length - 1);
        if (volEnvView) volEnvView->repaint();
        if (panEnvView) panEnvView->repaint();
        instPage_.refresh();
    }

    void onLoad()
    {
        /* Pull the currently-selected sample from the app-wide Disk Op
         * service.  The user picks a file in the Disk Op nav page
         * (F5 / View → Disk Op), then comes back here and clicks Load.
         * Mode is forced to Sample so the Disk Op shows the audio-files
         * filter on the next visit. */
        auto& svc = DiskOpService::get();
        svc.setMode (DiskOpService::Mode::kSample);

        const auto f = svc.getSelectedFile();
        if (! f.existsAsFile())
        {
            status.setText ("No sample selected — pick one in Disk Op",
                            dontSendNotification);
            return;
        }
        if (node.loadSampleToSlot (activeInstrument, activeSlot, f))
            status.setText ("Loaded " + f.getFileName(), dontSendNotification);
        else
            status.setText ("Failed: " + f.getFileName(), dontSendNotification);
        refresh();
    }

    void refresh()
    {
        auto inst = node.getInstrument (activeInstrument);
        const int slotListSel = slotList.getSelectedRow();
        if (slotListSel != activeSlot)
            slotList.selectRow (activeSlot, true, false);

        auto* slot = currentSlot();
        if (slot != nullptr)
        {
            rootSlider.setValue ((double) slot->rootNote,     dontSendNotification);
            relSlider .setValue ((double) slot->relativeNote, dontSendNotification);
            fineSlider.setValue ((double) slot->finetune,     dontSendNotification);
            volSlider .setValue ((double) slot->volume,       dontSendNotification);
            panSlider .setValue ((double) slot->panning,      dontSendNotification);
            loopCombo .setSelectedId ((int) slot->loopMode + 1, dontSendNotification);
        }
        interpCombo.setSelectedId ((int) node.getInterpMode() + 1, dontSendNotification);

        const int numLoaded = inst ? inst->numLoaded() : 0;
        const int numInst   = node.getNumInstruments();
        const SamplerSampleSlot* slotPtr = slot;
        const bool listDirty = (numLoaded != lastNumLoaded_)
                            || (slotPtr   != lastSlotPtr_)
                            || (activeSlot != lastActiveSlot_)
                            || (activeInstrument != lastActiveInstrument_);
        if (listDirty)
        {
            slotList.updateContent();
            slotList.repaint();
        }

        if (slotPtr != lastSlotPtr_)
            waveformView.repaint();

        if (numInst != lastNumInstruments_)
            rebuildInstCombo();

        const int voices = node.getNumVoices();
        const String slotName = (slot != nullptr) ? slot->name : String ("(empty)");
        if (numLoaded != lastNumLoaded_
         || voices    != lastVoices_
         || activeSlot != lastActiveSlot_
         || slotName  != lastSlotName_
         || activeInstrument != lastActiveInstrument_)
        {
            status.setText (String::formatted ("Ins %02d / Slot %02d: %s | %d/%d slots | %d ins | %d voices",
                                                activeInstrument + 1, activeSlot + 1,
                                                slotName.toRawUTF8(),
                                                numLoaded, SamplerInstrument::kNumSlots,
                                                numInst, voices),
                            dontSendNotification);
        }

        lastNumLoaded_       = numLoaded;
        lastSlotPtr_         = slotPtr;
        lastActiveSlot_      = activeSlot;
        lastActiveInstrument_ = activeInstrument;
        lastVoices_          = voices;
        lastNumInstruments_  = numInst;
        lastSlotName_        = slotName;
    }

    int                  lastNumLoaded_       { -1 };
    int                  lastVoices_          { -1 };
    int                  lastActiveSlot_      { -1 };
    int                  lastActiveInstrument_{ -1 };
    int                  lastNumInstruments_  { -1 };
    const SamplerSampleSlot* lastSlotPtr_     { nullptr };
    String               lastSlotName_;

    SamplerNode& node;

    ComboBox     instCombo;
    TextButton   instAddBtn, instDelBtn;

    ListBox      slotList;
    TextButton   loadBtn, clearBtn;

    Slider       rootSlider, relSlider, fineSlider, volSlider, panSlider;
    ComboBox     loopCombo;
    SampleWaveformView waveformView;

    TabbedComponent tabBar { TabbedButtonBar::TabsAtTop };
    ResizableWrap   volEnvWrap, panEnvWrap, autoVibWrap;
    TextButton      volTabAddBtn, volTabDelBtn, panTabAddBtn, panTabDelBtn;
    std::unique_ptr<FT2EnvelopeView> volEnvView, panEnvView;

    Slider       fadeoutSlider, avDepthSlider, avRateSlider, avSweepSlider;
    ComboBox     avTypeCombo;

    ComboBox     interpCombo;
    Label        status;

    int activeSlot = 0;
    int activeInstrument = 0;

    /* Paged container — nav state + sub-pages. */
    Page currentPage_ = Page::kBank;
    TextButton bankNavBtn, instNavBtn, sampleNavBtn, fxNavBtn;
    SamplerInstPage   instPage_;
    SamplerSamplePage samplePage_;
    SamplerFXPage     fxPage_;
};

AudioProcessorEditor* SamplerNode::createEditor()
{
    return new SamplerNodeEditor (*this);
}


/* =========================================================================== */

SamplerNode::SamplerNode()
    : BaseProcessor (BusesProperties()
                       .withOutput ("Output", AudioChannelSet::stereo(), true))
{
    ensureMixerInterpolationTablesReady();
    formatManager.registerBasicFormats();
    channelBinding.fill (-1);

    synth.reset (new ChannelTrackingSynth (*this));

    instruments.reserve (16);
    instruments.push_back (new SamplerInstrument());
    rebuildVoicePool();

    synth->addSound (new Ft2SamplerSound (*this));
}

SamplerNode::~SamplerNode()
{
    synth->clearVoices();
    synth->clearSounds();
}

void SamplerNode::fillInPluginDescription (PluginDescription& desc) const
{
    desc.fileOrIdentifier   = EL_NODE_ID_SAMPLER;
    desc.name               = "Sampler";
    desc.descriptiveName    = "Multi-sample instrument (MIDI-in / stereo audio-out)";
    desc.numInputChannels   = 0;
    desc.numOutputChannels  = 2;
    desc.hasSharedContainer = false;
    desc.isInstrument       = true;
    desc.manufacturerName   = EL_NODE_FORMAT_AUTHOR;
    desc.pluginFormatName   = EL_NODE_FORMAT_NAME;
    desc.version            = "0.3.0";
    desc.uniqueId           = EL_NODE_UID_SAMPLER;
}

void SamplerNode::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    synth->setCurrentPlaybackSampleRate (sampleRate);
}

void SamplerNode::releaseResources()
{
    synth->allNotesOff (0, true);
}

void SamplerNode::processBlock (AudioBuffer<float>& audio_, MidiBuffer& midi)
{
    audio_.clear();

    /* Scan for MIDI program-change events and update channel→instrument
     * binding before renderNextBlock dispatches notes.  PC value 0..127
     * maps directly to instrument index.  If the index exceeds the
     * loaded count, fall back to the channel default. */
    for (const auto& meta : midi)
    {
        const auto msg = meta.getMessage();
        if (! msg.isProgramChange()) continue;
        const int ch = msg.getChannel();         /* 1..16 */
        const int pc = msg.getProgramChangeNumber();
        if (pc >= 0 && pc < (int) instruments.size())
            bindChannelToInstrument (ch, pc);
        else
            bindChannelToInstrument (ch, -1);   /* default mapping */
    }

    synth->renderNextBlock (audio_, midi, 0, audio_.getNumSamples());
}

bool SamplerNode::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == AudioChannelSet::stereo();
}

void SamplerNode::rebuildVoicePool()
{
    synth->clearVoices();
    for (int i = 0; i < numVoices; ++i)
        synth->addVoice (new Ft2SamplerVoice (*this));
}

int SamplerNode::getNumInstruments() const
{
    return (int) instruments.size();
}

SamplerInstrument::Ptr SamplerNode::getInstrument (int index) const
{
    if (index < 0 || index >= (int) instruments.size()) return nullptr;
    return instruments[(size_t) index];
}

SamplerInstrument::Ptr SamplerNode::addInstrument()
{
    ScopedLock sl (sampleLock);
    if ((int) instruments.size() >= kMaxInstruments) return nullptr;
    auto inst = SamplerInstrument::Ptr (new SamplerInstrument());
    instruments.push_back (inst);
    return inst;
}

void SamplerNode::removeInstrument (int index)
{
    ScopedLock sl (sampleLock);
    if (index < 0 || index >= (int) instruments.size()) return;
    if (instruments.size() <= 1) return; /* always keep at least one */
    instruments.erase (instruments.begin() + index);
    /* Re-map channel bindings that pointed past the removed index. */
    for (auto& b : channelBinding)
    {
        if (b == (int8_t) index) b = -1;
        else if (b > (int8_t) index) --b;
    }
}

SamplerInstrument::Ptr SamplerNode::getInstrumentForChannel (int channel1to16) const
{
    const int ch = juce::jlimit (1, 16, channel1to16) - 1;
    int idx = channelBinding[(size_t) ch];
    if (idx < 0) idx = ch;   /* default mapping */
    if (idx >= (int) instruments.size()) idx = 0;
    if (idx < 0 || idx >= (int) instruments.size()) return nullptr;
    return instruments[(size_t) idx];
}

void SamplerNode::bindChannelToInstrument (int channel1to16, int instrumentIndex)
{
    const int ch = juce::jlimit (1, 16, channel1to16) - 1;
    if (instrumentIndex < 0 || instrumentIndex >= (int) instruments.size())
        channelBinding[(size_t) ch] = -1;
    else
        channelBinding[(size_t) ch] = (int8_t) instrumentIndex;
}

int SamplerNode::getChannelBinding (int channel1to16) const
{
    const int ch = juce::jlimit (1, 16, channel1to16) - 1;
    return (int) channelBinding[(size_t) ch];
}

bool SamplerNode::loadSampleToSlot (int instrumentIndex, int slot, const File& file)
{
    ScopedLock sl (sampleLock);
    if (instrumentIndex < 0 || instrumentIndex >= (int) instruments.size()) return false;
    return instruments[(size_t) instrumentIndex]->loadSampleToSlot (slot, file, formatManager);
}

void SamplerNode::rebuildInstrument()
{
    ScopedLock sl (sampleLock);
    instruments.clear();
    instruments.push_back (new SamplerInstrument());
    channelBinding.fill (-1);
    synth->clearSounds();
    synth->addSound (new Ft2SamplerSound (*this));
}

void SamplerNode::setNumVoices (int n)
{
    numVoices = jlimit (1, 64, n);
    rebuildVoicePool();
}

void SamplerNode::setInterpMode (InterpMode m) { interpMode = m; }
void SamplerNode::setAdsr (AdsrParams p)
{
    adsrParams = p;
    adsrParams.attack  = jlimit (0.f, 30.f, adsrParams.attack);
    adsrParams.decay   = jlimit (0.f, 30.f, adsrParams.decay);
    adsrParams.sustain = jlimit (0.f,  1.f, adsrParams.sustain);
    adsrParams.release = jlimit (0.f, 30.f, adsrParams.release);
}

int SamplerNode::getSamplesPerEnvTick() const noexcept
{
    return juce::jmax (1, (int) (currentSampleRate / (double) kEnvTickRateHz));
}

std::vector<int> SamplerNode::collectPlayheadsForSlot (const SamplerSampleSlot* slot) const
{
    std::vector<int> out;
    if (slot == nullptr) return out;
    auto& s = *synth;
    const int nv = s.getNumVoices();
    out.reserve ((size_t) nv);
    for (int i = 0; i < nv; ++i)
    {
        if (auto* v = dynamic_cast<Ft2SamplerVoice*> (s.getVoice (i)))
        {
            if (v->isPlayingActive() && v->getCurrentSlot() == slot)
                out.push_back (v->getCurrentSamplePos());
        }
    }
    return out;
}

int SamplerNode::getMixFuncIndexForCurrentMode (bool loop, bool pingpong) const
{
    const int base    = kBase16Ramped;
    const int interp  = interpOffset ((int) interpMode);
    const int loopOff = pingpong ? 2 : (loop ? 1 : 0);
    return base + interp + loopOff;
}

void SamplerNode::getStateInformation (MemoryBlock& dest)
{
    ValueTree tree ("sampler");
    tree.setProperty ("numVoices",  numVoices,         nullptr);
    tree.setProperty ("interpMode", (int) interpMode,  nullptr);
    tree.setProperty ("adsrAtt",  adsrParams.attack,  nullptr);
    tree.setProperty ("adsrDec",  adsrParams.decay,   nullptr);
    tree.setProperty ("adsrSus",  adsrParams.sustain, nullptr);
    tree.setProperty ("adsrRel",  adsrParams.release, nullptr);

    for (int b = 0; b < 16; ++b)
        tree.setProperty (Identifier ("bind" + String (b)), (int) channelBinding[(size_t) b], nullptr);

    for (size_t ii = 0; ii < instruments.size(); ++ii)
    {
        auto inst = instruments[ii];
        if (inst == nullptr) continue;
        ValueTree instTree ("instr");
        instTree.setProperty ("idx",  (int) ii,        nullptr);
        instTree.setProperty ("name", inst->name,      nullptr);
        instTree.setProperty ("fadeout", (int) inst->fadeoutRate, nullptr);
        instTree.setProperty ("avType",  (int) inst->autoVib.type,  nullptr);
        instTree.setProperty ("avSweep", (int) inst->autoVib.sweep, nullptr);
        instTree.setProperty ("avDepth", (int) inst->autoVib.depth, nullptr);
        instTree.setProperty ("avRate",  (int) inst->autoVib.rate,  nullptr);

        auto writeEnv = [&](const FT2Envelope& e, const String& prefix)
        {
            instTree.setProperty (Identifier (prefix + "Len"),     (int) e.length,       nullptr);
            instTree.setProperty (Identifier (prefix + "Flags"),   (int) e.flags,        nullptr);
            instTree.setProperty (Identifier (prefix + "Sus"),     (int) e.sustainPoint, nullptr);
            instTree.setProperty (Identifier (prefix + "LoopS"),   (int) e.loopStart,    nullptr);
            instTree.setProperty (Identifier (prefix + "LoopE"),   (int) e.loopEnd,      nullptr);
            String pts;
            for (int i = 0; i < (int) e.length; ++i)
                pts += String (e.points[i].x) + ":" + String (e.points[i].y) + ";";
            instTree.setProperty (Identifier (prefix + "Pts"), pts, nullptr);
        };
        writeEnv (inst->volumeEnv, "ve");
        writeEnv (inst->panEnv,    "pe");

        for (int i = 0; i < SamplerInstrument::kNumSlots; ++i)
        {
            const auto* slot = inst->getSlot (i);
            if (slot == nullptr) continue;
            ValueTree slotTree ("slot");
            slotTree.setProperty ("idx",          i,                    nullptr);
            slotTree.setProperty ("name",         slot->name,           nullptr);
            slotTree.setProperty ("rootNote",     slot->rootNote,       nullptr);
            slotTree.setProperty ("relativeNote", slot->relativeNote,   nullptr);
            slotTree.setProperty ("finetune",     slot->finetune,       nullptr);
            slotTree.setProperty ("volume",       slot->volume,         nullptr);
            slotTree.setProperty ("pan",          slot->panning,        nullptr);
            slotTree.setProperty ("loopMode",     (int) slot->loopMode, nullptr);
            slotTree.setProperty ("loopStart",    slot->loopStart,      nullptr);
            slotTree.setProperty ("loopLength",   slot->loopLength,     nullptr);
            instTree.appendChild (slotTree, nullptr);
        }
        tree.appendChild (instTree, nullptr);
    }

    MemoryOutputStream out (dest, false);
    { GZIPCompressorOutputStream gzip (out); tree.writeToStream (gzip); }
}

void SamplerNode::setStateInformation (const void* data, int size)
{
    if (data == nullptr || size <= 0) return;
    const auto tree = ValueTree::readFromGZIPData (data, (size_t) size);
    if (! tree.isValid() || tree.getType() != Identifier ("sampler")) return;

    numVoices  = (int) tree.getProperty ("numVoices", 16);
    interpMode = (InterpMode) (int) tree.getProperty ("interpMode", (int) kInterpLinear);
    adsrParams.attack  = (float) (double) tree.getProperty ("adsrAtt", 0.005);
    adsrParams.decay   = (float) (double) tree.getProperty ("adsrDec", 0.05);
    adsrParams.sustain = (float) (double) tree.getProperty ("adsrSus", 1.0);
    adsrParams.release = (float) (double) tree.getProperty ("adsrRel", 0.10);
    rebuildVoicePool();

    for (int b = 0; b < 16; ++b)
        channelBinding[(size_t) b] = (int8_t) (int) tree.getProperty (Identifier ("bind" + String (b)), -1);

    /* Reset instrument table from saved children. */
    instruments.clear();

    for (int i = 0; i < tree.getNumChildren(); ++i)
    {
        const auto instTree = tree.getChild (i);
        if (instTree.getType() != Identifier ("instr")) continue;
        SamplerInstrument::Ptr inst (new SamplerInstrument());
        inst->name        = instTree.getProperty ("name", "").toString();
        inst->fadeoutRate = (uint16_t) (int) instTree.getProperty ("fadeout", 0);
        inst->autoVib.type  = (uint8_t) (int) instTree.getProperty ("avType",  0);
        inst->autoVib.sweep = (uint8_t) (int) instTree.getProperty ("avSweep", 0);
        inst->autoVib.depth = (uint8_t) (int) instTree.getProperty ("avDepth", 0);
        inst->autoVib.rate  = (uint8_t) (int) instTree.getProperty ("avRate",  0);

        auto readEnv = [&](FT2Envelope& e, const String& prefix)
        {
            e.length       = (uint8_t) (int) instTree.getProperty (Identifier (prefix + "Len"),     0);
            e.flags        = (uint8_t) (int) instTree.getProperty (Identifier (prefix + "Flags"),   0);
            e.sustainPoint = (uint8_t) (int) instTree.getProperty (Identifier (prefix + "Sus"),     0);
            e.loopStart    = (uint8_t) (int) instTree.getProperty (Identifier (prefix + "LoopS"),   0);
            e.loopEnd      = (uint8_t) (int) instTree.getProperty (Identifier (prefix + "LoopE"),   0);
            const String pts = instTree.getProperty (Identifier (prefix + "Pts"), "").toString();
            const auto parts = StringArray::fromTokens (pts, ";", "");
            int n = 0;
            for (const auto& s : parts)
            {
                if (n >= 12) break;
                const auto xy = StringArray::fromTokens (s, ":", "");
                if (xy.size() != 2) continue;
                e.points[n].x = (int16_t) xy[0].getIntValue();
                e.points[n].y = (int16_t) xy[1].getIntValue();
                ++n;
            }
            if (e.length == 0 && n > 0) e.length = (uint8_t) n;
        };
        readEnv (inst->volumeEnv, "ve");
        readEnv (inst->panEnv,    "pe");

        for (int c = 0; c < instTree.getNumChildren(); ++c)
        {
            const auto slotTree = instTree.getChild (c);
            if (slotTree.getType() != Identifier ("slot")) continue;
            const int idx = (int) slotTree.getProperty ("idx", 0);
            if (auto* slot = inst->getSlot (idx))
            {
                slot->name         = slotTree.getProperty ("name", "").toString();
                slot->rootNote     = (int) slotTree.getProperty ("rootNote", 60);
                slot->relativeNote = (int) slotTree.getProperty ("relativeNote", 0);
                slot->finetune     = (int) slotTree.getProperty ("finetune", 0);
                slot->volume       = (float) (double) slotTree.getProperty ("volume", 1.0);
                slot->panning      = (float) (double) slotTree.getProperty ("pan", 0.5);
                slot->loopMode     = (SamplerLoopMode) (int) slotTree.getProperty ("loopMode", 0);
                slot->loopStart    = (int) slotTree.getProperty ("loopStart",  0);
                slot->loopLength   = (int) slotTree.getProperty ("loopLength", 0);
            }
        }
        instruments.push_back (inst);
    }

    if (instruments.empty())
        instruments.push_back (new SamplerInstrument());

    synth->clearSounds();
    synth->addSound (new Ft2SamplerSound (*this));
}

} // namespace element
