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

/** Convert float[] (mono) → int16[]. */
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

    AudioBuffer<float> tmp ((int) reader->numChannels, n);
    reader->read (&tmp, 0, n, 0, true, true);
    if (tmp.getNumChannels() > 1)
        for (int i = 0; i < n; ++i)
            tmp.setSample (0, i, 0.5f * (tmp.getSample (0, i) + tmp.getSample (1, i)));

    auto s = std::make_unique<SamplerSampleSlot>();
    s->name             = file.getFileNameWithoutExtension();
    s->data16           = convertFloatToInt16 (tmp.getReadPointer (0), n);
    s->numSamples       = n;
    s->sourceSampleRate = reader->sampleRate;
    /* Heuristic: assume root note follows slot's default position in the
     * spread, but leave at C4 for slot 0 by default. */
    s->rootNote = 60;

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

        voice = {};
        voice.base16     = slot->data16.get();
        voice.sampleEnd  = slot->numSamples;
        voice.position   = 0;
        voice.positionFrac = 0;
        voice.loopType   = 0;
        voice.active     = true;
        voice.mixFuncOffset = (uint8_t) owner.getMixFuncIndexForCurrentMode (false, false);

        /* Pitch: 12tet semitones + fine-tune (128 = half-semitone). */
        const double semis    = (double)(midiNote - slot->rootNote) + (double) slot->finetune / 128.0;
        const double pitchMul = std::pow (2.0, semis / 12.0);
        const double playRate = pitchMul * slot->sourceSampleRate / getSampleRate();
        voice.delta = (uint64_t) jlimit (0.0, 1e18, playRate * 4294967296.0);

        const float vel    = juce::jlimit (0.0f, 1.0f, velocity);
        const float gain   = vel * slot->volume;
        const float pan    = jlimit (0.0f, 1.0f, slot->panning);
        voice.fVolume        = gain;
        voice.fCurrVolumeL   = gain * (1.0f - pan);
        voice.fCurrVolumeR   = gain *         pan;
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

        /* Render the ft2 mixer into a scratch stereo accumulator first
         * so we can multiply by the ADSR per-sample without disturbing
         * other voices' contributions. */
        AudioBuffer<float> scratch (2, numSamples);
        scratch.clear();

        audio.fMixBufferL = scratch.getWritePointer (0);
        audio.fMixBufferR = scratch.getWritePointer (1);

        if (voice.active)
            mixFuncTab[voice.mixFuncOffset] (&voice, 0u, (uint32_t) numSamples);

        /* Multiply by per-sample ADSR envelope, then accumulate into the
         * synth's shared output buffer at (startSample, numSamples). */
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

        /* HANDLE_SAMPLE_END flips voice.active to false on sample end.
         * Also kill the voice when ADSR fully releases. */
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
        adsrView (s.getAdsr(), [this] (SamplerNode::AdsrParams p) { node.setAdsr (p); })
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
        panSlider .setBounds (r.removeFromTop (rowH)); r.removeFromTop (8);

        adsrView.setBounds (r.removeFromTop (120)); r.removeFromTop (8);

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
        if (slotList.getSelectedRow() != activeSlot)
            slotList.selectRow (activeSlot, true, false);

        if (auto* slot = currentSlot())
        {
            rootSlider.setValue ((double) slot->rootNote, dontSendNotification);
            fineSlider.setValue ((double) slot->finetune, dontSendNotification);
            volSlider .setValue ((double) slot->volume,   dontSendNotification);
            panSlider .setValue ((double) slot->panning,  dontSendNotification);
        }

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

int SamplerNode::getMixFuncIndexForCurrentMode (bool loop, bool pingpong) const
{
    const int base   = kBase16NoRamp; /* always ramped versions used? for v1 no-ramp */
    const int interp = interpOffset ((int) interpMode);
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
            }
        }
    }
}

} // namespace element
