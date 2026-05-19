// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "nodes/baseprocessor.hpp"
#include "ElementApp.h"

namespace element {

/** One sample slot within an Instrument.  Holds the loaded WAV's int16
 *  data, source sample rate, and per-sample play parameters (root note,
 *  fine tune, volume, pan).  Mono only for v1; stereo is summed at load. */
struct SamplerSampleSlot
{
    String  name;
    std::unique_ptr<int16_t[]> data16;
    int     numSamples = 0;
    double  sourceSampleRate = 48000.0;
    int     rootNote = 60;       // MIDI note at which sample plays at native pitch
    int     finetune = 0;        // -128..127 (cents/2 fine offset)
    float   volume   = 1.0f;     // 0..1
    float   panning  = 0.5f;     // 0=L  0.5=centre  1=R

    bool isLoaded() const noexcept { return data16 != nullptr && numSamples > 0; }
};

/** Instrument = up to 16 sample slots + a 128-entry MIDI keymap that
 *  maps each MIDI note to a slot index.  Default keymap spreads loaded
 *  slots evenly across the keyboard.  Renoise / FT2 model. */
class SamplerInstrument : public ReferenceCountedObject
{
public:
    using Ptr = ReferenceCountedObjectPtr<SamplerInstrument>;
    static constexpr int kNumSlots = 16;

    SamplerInstrument();

    /** Load a WAV (or AudioFormatManager-supported file) into slot. */
    bool loadSampleToSlot (int slot, const File& file, AudioFormatManager& fmt);
    void clearSlot (int slot);

    int  slotForNote (int midiNote) const noexcept;
    void setSlotForNote (int midiNote, int slot);

    /** Spread currently-loaded slots evenly across the keyboard.
     *  Called automatically on load when keymap is in default state. */
    void autoSpreadKeymap();

    SamplerSampleSlot*       getSlot (int slot);
    const SamplerSampleSlot* getSlot (int slot) const;
    int firstLoadedSlot() const noexcept;
    int numLoaded() const noexcept;

    String name;

private:
    std::array<std::unique_ptr<SamplerSampleSlot>, kNumSlots> slots;
    uint8_t noteToSlot[128] {};  // default all 0
    bool keymapUserModified = false;
};

/** Sample-based instrument node.  MIDI-in / stereo audio-out.
 *
 *  Multi-sample instrument model (up to 16 sample slots + keymap).
 *  Per-voice DSP via vendored ft2-clone mixer.  ADSR envelope + several
 *  interpolation modes.  */
class SamplerNode : public BaseProcessor
{
public:
    SamplerNode();
    ~SamplerNode() override;

    const String getName() const override { return "Sampler"; }
    void fillInPluginDescription (PluginDescription& desc) const override;

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void processBlock (AudioBuffer<float>&, MidiBuffer&) override;

    bool canAddBus    (bool) const override { return false; }
    bool canRemoveBus (bool) const override { return false; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool supportsMPE() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const String getProgramName (int) override { return getName(); }
    void changeProgramName (int, const String&) override {}

    void getStateInformation (MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    /** Load a WAV into a specific slot of the active instrument. */
    bool loadSampleToSlot (int slot, const File& file);

    SamplerInstrument::Ptr getInstrument() const { return instrument; }
    void rebuildInstrument(); /* clear all slots */

    int  getNumVoices() const noexcept { return numVoices; }
    void setNumVoices (int n);

    /** Interpolation quality. */
    enum InterpMode { kInterpNone = 0, kInterpLinear = 1, kInterpCubic = 2, kInterpSinc16 = 3 };
    InterpMode getInterpMode() const noexcept { return interpMode; }
    void       setInterpMode (InterpMode m);

    /** Instrument-global ADSR.  Seconds for A/D/R; 0..1 for sustain. */
    struct AdsrParams { float attack = 0.005f; float decay = 0.05f;
                        float sustain = 1.0f;  float release = 0.10f; };
    AdsrParams getAdsr() const { return adsrParams; }
    void setAdsr (AdsrParams p);

    /** Mix-func index for current interpolation mode.  Used by voices. */
    int getMixFuncIndexForCurrentMode (bool loop, bool pingpong) const;

    AudioFormatManager& getFormatManager() { return formatManager; }

protected:
    bool isBusesLayoutSupported (const BusesLayout&) const override;

private:
    void rebuildVoicePool();

    Synthesiser synth;
    AudioFormatManager formatManager;

    CriticalSection sampleLock;
    SamplerInstrument::Ptr instrument;
    int numVoices = 16;
    double currentSampleRate = 48000.0;
    InterpMode interpMode = kInterpLinear;
    AdsrParams adsrParams;
};

} // namespace element
