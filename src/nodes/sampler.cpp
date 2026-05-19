// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/sampler.hpp"

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

/* Per-interp offset within a 15-entry row. */
constexpr int interpOffset (int interpMode)
{
    switch (interpMode)
    {
        case 0: return  0;  /* None      */
        case 1: return  6;  /* Linear    (L) — entries 6..8 */
        case 2: return 12;  /* Cubic     (C) — entries 12..14 */
        case 3: return  9;  /* Sinc16    (S16) — entries 9..11 */
        default: return 0;
    }
}

/** Convert one channel of float audio → int16[]. */
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

} // anonymous namespace


/* ===========================================================================
 * SamplerInstrument
 * ========================================================================*/

SamplerInstrument::SamplerInstrument()
{
    for (auto& s : slots) s.reset();
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

    /* Spread loaded slots evenly across MIDI 0..127. */
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
 * Ft2SamplerSound — JUCE shim that holds the SamplerInstrument pointer
 * so voices can look up sample data on note-on.
 * ========================================================================*/
class Ft2SamplerSound : public SynthesiserSound
{
public:
    explicit Ft2SamplerSound (SamplerInstrument::Ptr inst) : instrument (inst) {}
    bool appliesToNote    (int) override { return true; }
    bool appliesToChannel (int) override { return true; }
    SamplerInstrument::Ptr getInstrument() const { return instrument; }
private:
    SamplerInstrument::Ptr instrument;
};


/* ===========================================================================
 * Ft2SamplerVoice — ft2 mixer + juce::ADSR envelope.
 * ========================================================================*/
class Ft2SamplerVoice : public SynthesiserVoice
{
public:
    explicit Ft2SamplerVoice (SamplerNode& owner_) : owner (owner_) {}

    bool canPlaySound (SynthesiserSound* s) override
    {
        return dynamic_cast<Ft2SamplerSound*> (s) != nullptr;
    }

    /** Read-only state queries for the waveform-view playhead overlay.
     *  Audio thread writes voice.position; GUI thread reads.  int32 is
     *  atomic on every platform we care about; an occasional torn read
     *  is fine for a visual indicator. */
    int  getCurrentSamplePos() const noexcept { return voice.position; }
    bool isPlayingActive() const noexcept     { return voice.active; }
    const SamplerSampleSlot* getCurrentSlot() const noexcept { return slotPtr; }

    void startNote (int midiNote, float velocity,
                    SynthesiserSound* sound, int /*pitchWheel*/) override
    {
        auto* s = dynamic_cast<Ft2SamplerSound*> (sound);
        if (s == nullptr) return;

        auto inst = s->getInstrument();
        if (inst == nullptr) { clearCurrentNote(); return; }

        const int slotIdx = inst->slotForNote (midiNote);
        const auto* slot = inst->getSlot (slotIdx);
        if (slot == nullptr || ! slot->isLoaded()) { clearCurrentNote(); return; }

        slotPtr      = slot;
        slotIsStereo = slot->isStereo && slot->data16R != nullptr;

        const bool loopRequested = (slot->loopMode == SamplerLoopMode::kForward)
                                || (slot->loopMode == SamplerLoopMode::kPingpong);
        /* Loop is only active if there's a positive loop length AND
         * loopStart is in-range. Otherwise fall back to no-loop. */
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

        /* Pitch: 12tet semitones + fine-tune (128 = half-semitone). */
        const double semis    = (double)(midiNote - slot->rootNote) + (double) slot->finetune / 128.0;
        const double pitchMul = std::pow (2.0, semis / 12.0);
        const double playRate = pitchMul * slot->sourceSampleRate / getSampleRate();
        voice.delta = (uint64_t) jlimit (0.0, 1e18, playRate * 4294967296.0);

        const float vel  = juce::jlimit (0.0f, 1.0f, velocity);
        const float gain = vel * slot->volume;
        const float pan  = jlimit (0.0f, 1.0f, slot->panning);
        voice.fVolume = gain;
        if (slotIsStereo)
        {
            /* Stereo pan: at centre (0.5), L→L, R→R unchanged. At hard
             * left, both L+R sum into L. At hard right, both → R. */
            const float r = jmax (0.0f, 2.0f * (pan - 0.5f));      // ≥0 only when pan > 0.5
            const float l = jmax (0.0f, 2.0f * (0.5f - pan));      // ≥0 only when pan < 0.5
            gLL = gain * (1.0f - r);   // L data → L out
            gRR = gain * (1.0f - l);   // R data → R out
            gLR = gain * r;            // L data → R out (when panned right)
            gRL = gain * l;            // R data → L out (when panned left)
            voice.fCurrVolumeL = gLL;
            voice.fCurrVolumeR = gLR;
        }
        else
        {
            /* Mono: linear pan. */
            voice.fCurrVolumeL = gain * (1.0f - pan);
            voice.fCurrVolumeR = gain *          pan;
        }
        voice.fTargetVolumeL = voice.fCurrVolumeL;
        voice.fTargetVolumeR = voice.fCurrVolumeR;
        voice.fVolumeLDelta  = 0.0f;
        voice.fVolumeRDelta  = 0.0f;
        voice.volumeRampLength = 0;

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
            adsr.noteOff();
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

        AudioBuffer<float> scratch (2, numSamples);
        scratch.clear();

        audio.fMixBufferL = scratch.getWritePointer (0);
        audio.fMixBufferR = scratch.getWritePointer (1);

        if (voice.active)
        {
            if (slotIsStereo && slotPtr != nullptr)
            {
                /* Stereo: two mixer passes, each with one channel of
                 * the sample. Both passes use the same delta + start
                 * position, so they advance identically; we capture
                 * state after the first pass and restore for the
                 * second so the second runs with the same input
                 * position. After both, voice state matches one
                 * advance step. */
                voice_t snap = voice;
                voice.fCurrVolumeL   = gLL;
                voice.fCurrVolumeR   = gLR;
                voice.fTargetVolumeL = gLL;
                voice.fTargetVolumeR = gLR;
                voice.base16 = slotPtr->data16L.get();
                mixFuncTab[voice.mixFuncOffset] (&voice, 0u, (uint32_t) numSamples);

                voice_t afterFirst = voice;

                voice = snap;
                voice.fCurrVolumeL   = gRL;
                voice.fCurrVolumeR   = gRR;
                voice.fTargetVolumeL = gRL;
                voice.fTargetVolumeR = gRR;
                voice.base16 = slotPtr->data16R.get();
                mixFuncTab[voice.mixFuncOffset] (&voice, 0u, (uint32_t) numSamples);

                /* If either pass ran out of sample, mark voice inactive. */
                voice.active = voice.active && afterFirst.active;
            }
            else
            {
                mixFuncTab[voice.mixFuncOffset] (&voice, 0u, (uint32_t) numSamples);
            }
        }

        auto* outL = out.getWritePointer (0) + startSample;
        auto* outR = out.getWritePointer (1) + startSample;
        const auto* sL = scratch.getReadPointer (0);
        const auto* sR = scratch.getReadPointer (1);
        for (int i = 0; i < numSamples; ++i)
        {
            const float env = adsr.getNextSample();
            outL[i] += sL[i] * env;
            outR[i] += sR[i] * env;
        }

        if (! voice.active || ! adsr.isActive())
        {
            voice.active = false;
            clearCurrentNote();
        }
    }

private:
    SamplerNode& owner;
    voice_t voice {};
    ADSR adsr;

    /* Stereo state: per-voice cached gains so the two render passes can
     * use the right per-channel volumes without recomputing.
     *   gLL  L data → L out
     *   gLR  L data → R out   (when panned right of centre)
     *   gRL  R data → L out   (when panned left  of centre)
     *   gRR  R data → R out   */
    const SamplerSampleSlot* slotPtr = nullptr;
    bool slotIsStereo = false;
    float gLL = 1.0f, gLR = 0.0f, gRL = 0.0f, gRR = 1.0f;
};


/* ===========================================================================
 * Helpers — note names + visual ADSR.
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
 * SampleWaveformView — renders the active slot's waveform with draggable
 * loop markers (FT2-style sample editor mini).  Click+drag the markers
 * to set loop start / end.
 * ========================================================================*/
class SamplerNode;
class SamplerInstrument;
struct SamplerSampleSlot;

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
        startTimerHz (30); // smooth playhead chase
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

        /* Min/max envelope per pixel column. */
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

        /* Loop region + draggable markers. */
        if (slot->loopMode != SamplerLoopMode::kNone && slot->loopLength > 0)
        {
            const float x0 = bounds.getX() + (float) bounds.getWidth() * slot->loopStart / (float) n;
            const float x1 = bounds.getX() + (float) bounds.getWidth() * (slot->loopStart + slot->loopLength) / (float) n;
            g.setColour (Colour { 0x33'ff'a0'40 });
            g.fillRect (x0, bounds.getY(), x1 - x0, bounds.getHeight());
            g.setColour (Colour { 0xff'ff'a0'40 });
            g.fillRect (x0 - 1.0f, bounds.getY(), 2.0f, bounds.getHeight());
            g.fillRect (x1 - 1.0f, bounds.getY(), 2.0f, bounds.getHeight());
            /* Small handles top + bottom for grab affordance. */
            g.fillEllipse (x0 - 4.0f, bounds.getY() - 0.0f, 8.0f, 8.0f);
            g.fillEllipse (x1 - 4.0f, bounds.getBottom() - 8.0f, 8.0f, 8.0f);
        }

        /* Live playheads — one vertical line per active voice playing
         * this slot.  Drawn over the waveform + loop overlay. */
        if (playheadsFor)
        {
            const auto positions = playheadsFor (slot);
            g.setColour (Colour { 0xff'40'ff'80 }); // green
            for (int pos : positions)
            {
                if (pos < 0 || pos >= n) continue;
                const float x = bounds.getX() + (float) bounds.getWidth() * pos / (float) n;
                g.fillRect (x - 0.5f, bounds.getY(), 1.5f, bounds.getHeight());
            }
        }
    }

private:
    void timerCallback() override { repaint(); }

public:

    void mouseDown (const MouseEvent& e) override
    {
        auto* slot = getSlot ? getSlot() : nullptr;
        if (slot == nullptr || ! slot->isLoaded()) return;
        if (slot->loopMode == SamplerLoopMode::kNone || slot->loopLength <= 0)
        {
            /* Auto-activate forward loop covering [start..end] on first
             * click+drag.  start anchored at the click, length = 1 (user
             * drags right to extend). */
            slot->loopMode   = SamplerLoopMode::kForward;
            slot->loopStart  = pixelToSample (e.x, slot->numSamples);
            slot->loopLength = juce::jmax (1, slot->numSamples - slot->loopStart);
            draggingMarker   = 1; /* end */
            repaint();
            return;
        }
        /* Pick the closer of the two markers. */
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
    int draggingMarker = -1; /* -1 none; 0 = loopStart; 1 = loopEnd */
};


class AdsrCurveComponent : public Component
{
public:
    using ChangeCallback = std::function<void (SamplerNode::AdsrParams)>;

    AdsrCurveComponent (SamplerNode::AdsrParams p, ChangeCallback cb)
        : params (p), onChange (std::move (cb)) {}

    void setParams (SamplerNode::AdsrParams p) { params = p; repaint(); }
    SamplerNode::AdsrParams getParams() const { return params; }

    void paint (Graphics& g) override
    {
        const Rectangle<float> area = getLocalBounds().toFloat().reduced (4.0f);
        g.setColour (Colour { 0xff'12'12'12 });
        g.fillRect (area);
        g.setColour (Colour { 0xff'30'30'30 });
        g.drawRect (area, 1.0f);

        /* Compute the curve. */
        const auto pts = controlPoints (area);

        /* Polyline through control points. */
        Path curve;
        curve.startNewSubPath (pts[0]);
        for (size_t i = 1; i < pts.size(); ++i) curve.lineTo (pts[i]);
        g.setColour (Colour { 0xff'5a'a5'd0 });
        g.strokePath (curve, PathStrokeType (2.0f));

        /* Shade under the curve. */
        Path shade = curve;
        shade.lineTo (pts.back().x, area.getBottom());
        shade.lineTo (pts.front().x, area.getBottom());
        shade.closeSubPath();
        g.setColour (Colour { 0x33'5a'a5'd0 });
        g.fillPath (shade);

        /* Draggable handles. */
        g.setColour (Colour { 0xff'ff'a0'40 });
        for (size_t i = 1; i < pts.size(); ++i)
            g.fillEllipse (pts[i].x - 4.0f, pts[i].y - 4.0f, 8.0f, 8.0f);

        /* Label */
        g.setColour (Colour { 0xff'9a'9a'9a });
        g.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 10.0f, Font::plain));
        const String lbl = String::formatted ("A %.0fms  D %.0fms  S %.2f  R %.0fms",
                                              params.attack * 1000.0f,
                                              params.decay * 1000.0f,
                                              params.sustain,
                                              params.release * 1000.0f);
        g.drawText (lbl, area.reduced (4.0f), Justification::topLeft);
    }

    void mouseDown (const MouseEvent& e) override
    {
        const auto area = getLocalBounds().toFloat().reduced (4.0f);
        const auto pts = controlPoints (area);
        draggingPoint = -1;
        for (size_t i = 1; i < pts.size(); ++i)
        {
            if (pts[i].getDistanceFrom (e.position) < 10.0f)
            {
                draggingPoint = (int) i;
                break;
            }
        }
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (draggingPoint < 1) return;
        const auto area = getLocalBounds().toFloat().reduced (4.0f);
        /* X-axis: 0..maxTime, Y-axis: 1..0 (top=1, bottom=0). */
        const float maxTime = 5.0f;
        const float xRel = jlimit (0.0f, 1.0f,
                                     (e.position.x - area.getX()) / area.getWidth());
        const float yRel = 1.0f - jlimit (0.0f, 1.0f,
                                     (e.position.y - area.getY()) / area.getHeight());
        const float t = xRel * maxTime;

        switch (draggingPoint)
        {
            case 1: /* Attack handle:  x = attack time, y locked at 1 */
                params.attack = jmax (0.0f, t);
                break;
            case 2: /* Decay end handle: x = attack+decay, y = sustain */
                params.decay   = jmax (0.0f, t - params.attack);
                params.sustain = yRel;
                break;
            case 3: /* Release end: x = sustain region end + release; y locked at 0 */
                params.release = jmax (0.0f, t - params.attack - params.decay - sustainHold());
                break;
            default: break;
        }

        repaint();
        if (onChange) onChange (params);
    }

    void mouseUp (const MouseEvent&) override { draggingPoint = -1; }

private:
    /* Visual hold time for the sustain region — purely for layout
     * (note-off in reality is dynamic). */
    float sustainHold() const { return 0.5f; }

    std::array<Point<float>, 5> controlPoints (Rectangle<float> area) const
    {
        const float maxTime = 5.0f;
        auto xAt = [&] (float t) { return area.getX() + (t / maxTime) * area.getWidth(); };
        auto yAt = [&] (float v) { return area.getY() + (1.0f - v) * area.getHeight(); };

        const float t0 = 0.0f;
        const float t1 = params.attack;
        const float t2 = t1 + params.decay;
        const float t3 = t2 + sustainHold();
        const float t4 = t3 + params.release;

        std::array<Point<float>, 5> pts {{
            { xAt (t0), yAt (0.0f) },
            { xAt (t1), yAt (1.0f) },
            { xAt (t2), yAt (params.sustain) },
            { xAt (t3), yAt (params.sustain) },
            { xAt (t4), yAt (0.0f) }
        }};
        return pts;
    }

    SamplerNode::AdsrParams params;
    ChangeCallback onChange;
    int draggingPoint = -1;
};


/* ===========================================================================
 * SamplerNodeEditor
 *   - Left: vertical 16-row slot list (slot # · key range · sample name)
 *   - Middle: path field + Load / Clear buttons
 *   - Right top: per-slot params (root note / fine-tune / volume / pan)
 *   - Right middle: visual ADSR curve (drag handles)
 *   - Right bottom: interpolation selector + status
 * ========================================================================*/
class SamplerNodeEditor : public AudioProcessorEditor,
                          private Timer,
                          private ListBoxModel
{
public:
    explicit SamplerNodeEditor (SamplerNode& s) : AudioProcessorEditor (s), node (s),
        adsrView (s.getAdsr(), [this] (SamplerNode::AdsrParams p) { node.setAdsr (p); }),
        waveformView ([this] { return currentSlot(); },
                      [this] (const SamplerSampleSlot* slot) {
                          return node.collectPlayheadsForSlot (slot);
                      })
    {
        slotList.setModel (this);
        slotList.setRowHeight (24);
        slotList.setColour (ListBox::backgroundColourId, Colour { 0xff'14'14'14 });
        slotList.setColour (ListBox::outlineColourId,    Colour { 0xff'2a'2a'2a });
        slotList.setOutlineThickness (1);
        addAndMakeVisible (slotList);

        pathEdit.setTextToShowWhenEmpty ("/path/to/sample.wav", Colours::grey);
        pathEdit.setMultiLine (false);
        pathEdit.setReturnKeyStartsNewLine (false);
        pathEdit.onReturnKey = [this] { onLoad(); };
        addAndMakeVisible (pathEdit);

        loadBtn.setButtonText ("Load");
        loadBtn.onClick = [this] { onLoad(); };
        addAndMakeVisible (loadBtn);

        clearBtn.setButtonText ("Clear");
        clearBtn.onClick = [this] {
            if (auto inst = node.getInstrument()) inst->clearSlot (activeSlot);
            refresh();
        };
        addAndMakeVisible (clearBtn);

        configureParamSlider (rootSlider, "Root", 0.0, 127.0, 1.0,
            [this](double v) { if (auto* s = currentSlot()) s->rootNote = (int) v; });
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
        addAndMakeVisible (adsrView);

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
        status.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 12.0f, Font::plain));
        addAndMakeVisible (status);

        setOpaque (true);
        setSize (760, 380);
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

        /* Left column: slot list + path field. */
        auto leftCol = r.removeFromLeft (260);
        slotList.setBounds (leftCol.removeFromTop (260));
        leftCol.removeFromTop (8);
        auto pathRow = leftCol.removeFromTop (28);
        loadBtn .setBounds (pathRow.removeFromRight (52)); pathRow.removeFromRight (4);
        clearBtn.setBounds (pathRow.removeFromRight (52)); pathRow.removeFromRight (4);
        pathEdit.setBounds (pathRow);

        r.removeFromLeft (16);

        /* Right column: per-slot params + ADSR curve + interp + status. */
        const int rowH = 28;
        rootSlider.setBounds (r.removeFromTop (rowH)); r.removeFromTop (2);
        fineSlider.setBounds (r.removeFromTop (rowH)); r.removeFromTop (2);
        volSlider .setBounds (r.removeFromTop (rowH)); r.removeFromTop (2);
        panSlider .setBounds (r.removeFromTop (rowH)); r.removeFromTop (4);

        loopCombo.setBounds (r.removeFromTop (rowH));  r.removeFromTop (4);
        waveformView.setBounds (r.removeFromTop (90)); r.removeFromTop (8);

        adsrView.setBounds (r.removeFromTop (90)); r.removeFromTop (6);

        auto interpRow = r.removeFromTop (28);
        interpCombo.setBounds (interpRow.removeFromLeft (160));
        r.removeFromTop (4);

        status.setBounds (r.removeFromTop (40));
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

        auto inst = node.getInstrument();
        const auto* slot = inst ? inst->getSlot (row) : nullptr;

        g.setFont (FontOptions (Font::getDefaultMonospacedFontName(), 12.0f, Font::plain));

        /* col 1: slot # (always shown) */
        g.setColour (slot != nullptr ? Colour { 0xff'd4'd4'd4 } : Colour { 0xff'5a'5a'5a });
        g.drawText (String::formatted ("%02d", row + 1), 6, 0, 28, height,
                    Justification::centredLeft);

        /* col 2: key range covered by this slot (auto-spread). */
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

        /* col 3: sample name (or "—" placeholder). */
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
        if (pathEdit.getText().trim().isNotEmpty()) onLoad();
    }

private:
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
        if (auto inst = node.getInstrument()) return inst->getSlot (activeSlot);
        return nullptr;
    }

    void onLoad()
    {
        const auto text = pathEdit.getText().trim();
        if (text.isEmpty()) return;
        const File f (text);
        if (! f.existsAsFile())
        {
            status.setText ("Not a file: " + text, dontSendNotification);
            return;
        }
        if (node.loadSampleToSlot (activeSlot, f))
            status.setText ("Loaded slot " + String (activeSlot + 1) + ": " + f.getFileName(),
                            dontSendNotification);
        else
            status.setText ("Failed to read: " + f.getFileName(), dontSendNotification);
        refresh();
    }

    void refresh()
    {
        auto inst = node.getInstrument();
        slotList.updateContent();
        slotList.repaint();
        if (slotList.getSelectedRow() != activeSlot)
            slotList.selectRow (activeSlot, true, false);

        if (auto* slot = currentSlot())
        {
            rootSlider.setValue ((double) slot->rootNote, dontSendNotification);
            fineSlider.setValue ((double) slot->finetune, dontSendNotification);
            volSlider .setValue ((double) slot->volume,   dontSendNotification);
            panSlider .setValue ((double) slot->panning,  dontSendNotification);
            loopCombo.setSelectedId ((int) slot->loopMode + 1, dontSendNotification);
        }
        waveformView.repaint();

        adsrView.setParams (node.getAdsr());
        interpCombo.setSelectedId ((int) node.getInterpMode() + 1, dontSendNotification);

        const int n = inst ? inst->numLoaded() : 0;
        const String slotName = (inst && inst->getSlot (activeSlot))
                                  ? inst->getSlot (activeSlot)->name
                                  : String ("(empty)");
        status.setText (String::formatted ("Slot %02d: %s   |   %d/%d loaded   |   %d voices",
                                            activeSlot + 1, slotName.toRawUTF8(),
                                            n, SamplerInstrument::kNumSlots,
                                            node.getNumVoices()),
                        dontSendNotification);
    }

    SamplerNode& node;

    ListBox slotList;
    TextEditor pathEdit;
    TextButton loadBtn, clearBtn;

    Slider rootSlider, fineSlider, volSlider, panSlider;
    ComboBox loopCombo;
    SampleWaveformView waveformView;
    AdsrCurveComponent adsrView;
    ComboBox interpCombo;
    Label status;

    int activeSlot = 0;
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
    instrument = new SamplerInstrument();
    rebuildVoicePool();

    /* Wire the synth: one shared "sound" that points at our instrument
     * pointer.  Voices look up samples from it on note-on. */
    synth.addSound (new Ft2SamplerSound (instrument));
}

SamplerNode::~SamplerNode()
{
    synth.clearVoices();
    synth.clearSounds();
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
    desc.version            = "0.2.0";
    desc.uniqueId           = EL_NODE_UID_SAMPLER;
}

void SamplerNode::prepareToPlay (double sampleRate, int)
{
    currentSampleRate = sampleRate;
    synth.setCurrentPlaybackSampleRate (sampleRate);
}

void SamplerNode::releaseResources()
{
    synth.allNotesOff (0, true);
}

void SamplerNode::processBlock (AudioBuffer<float>& audio_, MidiBuffer& midi)
{
    audio_.clear();
    synth.renderNextBlock (audio_, midi, 0, audio_.getNumSamples());
}

bool SamplerNode::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == AudioChannelSet::stereo();
}

void SamplerNode::rebuildVoicePool()
{
    synth.clearVoices();
    for (int i = 0; i < numVoices; ++i)
        synth.addVoice (new Ft2SamplerVoice (*this));
}

bool SamplerNode::loadSampleToSlot (int slot, const File& file)
{
    ScopedLock sl (sampleLock);
    if (instrument == nullptr) instrument = new SamplerInstrument();
    return instrument->loadSampleToSlot (slot, file, formatManager);
}

void SamplerNode::rebuildInstrument()
{
    ScopedLock sl (sampleLock);
    instrument = new SamplerInstrument();
    synth.clearSounds();
    synth.addSound (new Ft2SamplerSound (instrument));
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

std::vector<int> SamplerNode::collectPlayheadsForSlot (const SamplerSampleSlot* slot) const
{
    std::vector<int> out;
    if (slot == nullptr) return out;
    auto& s = const_cast<Synthesiser&> (synth);
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
    /* Use ramped 16-bit mix funcs so the mixer's internal volume ramp
     * smooths any mid-block parameter changes (eliminates click
     * artefacts on velocity / pan tweaks even when ADSR is the macro
     * envelope). */
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

    if (instrument != nullptr)
    {
        for (int i = 0; i < SamplerInstrument::kNumSlots; ++i)
        {
            const auto* slot = instrument->getSlot (i);
            if (slot == nullptr) continue;
            ValueTree slotTree ("slot");
            slotTree.setProperty ("idx",      i,                 nullptr);
            slotTree.setProperty ("name",     slot->name,        nullptr);
            slotTree.setProperty ("rootNote", slot->rootNote,    nullptr);
            slotTree.setProperty ("finetune", slot->finetune,    nullptr);
            slotTree.setProperty ("volume",   slot->volume,      nullptr);
            slotTree.setProperty ("pan",      slot->panning,     nullptr);
            slotTree.setProperty ("loopMode",   (int) slot->loopMode, nullptr);
            slotTree.setProperty ("loopStart",  slot->loopStart,      nullptr);
            slotTree.setProperty ("loopLength", slot->loopLength,     nullptr);
            /* Note: raw sample data is not persisted to the session blob
             * (would bloat); user should keep sample files on disk and
             * rely on the per-slot path saved separately. v2 todo: save
             * the source file path so we can reload from disk. */
            tree.appendChild (slotTree, nullptr);
        }
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

    /* Sample data not persisted in v0.2.  Slot metadata-only restore. */
    if (instrument != nullptr)
    {
        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            const auto slotTree = tree.getChild (i);
            if (slotTree.getType() != Identifier ("slot")) continue;
            const int idx = (int) slotTree.getProperty ("idx", 0);
            if (auto* slot = instrument->getSlot (idx))
            {
                slot->rootNote = (int) slotTree.getProperty ("rootNote", 60);
                slot->finetune = (int) slotTree.getProperty ("finetune", 0);
                slot->volume   = (float) (double) slotTree.getProperty ("volume", 1.0);
                slot->panning  = (float) (double) slotTree.getProperty ("pan",    0.5);
                slot->loopMode   = (SamplerLoopMode) (int) slotTree.getProperty ("loopMode", 0);
                slot->loopStart  = (int) slotTree.getProperty ("loopStart",  0);
                slot->loopLength = (int) slotTree.getProperty ("loopLength", 0);
            }
        }
    }
}

} // namespace element
