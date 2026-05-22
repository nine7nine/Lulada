// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "nodes/baseprocessor.hpp"
#include "ElementApp.h"

namespace element {

/** Loop mode for sample playback. */
enum class SamplerLoopMode { kNone = 0, kForward = 1, kPingpong = 2 };

/** One sample slot within an Instrument.  Holds int16 sample data
 *  (mono OR stereo — data16R is null for mono), source sample rate,
 *  per-sample play params (root note, fine tune, volume, pan, relativeNote
 *  XM-style semitone offset), and loop state. */
struct SamplerSampleSlot
{
    String  name;

    /** Absolute POSIX path of the originally-loaded sample file.
     *  Stored as std::string (not juce::File or juce::String) so the
     *  persistence layer matches the native-path convention Disk Op
     *  uses (::opendir / ::readdir / ::access).  Empty for slots
     *  created from non-file sources (paste, future synth bake).
     *  Used to reload audio across session save/restart -- the
     *  decoded int16 buffers below are NEVER embedded in the
     *  session XML, only this path is. */
    std::string sourceFile;

    std::unique_ptr<int16_t[]> data16L;
    std::unique_ptr<int16_t[]> data16R;
    bool    isStereo = false;
    int     numSamples = 0;
    double  sourceSampleRate = 48000.0;
    int     rootNote = 60;
    int     finetune = 0;          // -128..127 (cents/2 fine offset)
    int     relativeNote = 0;      // -96..96 (XM semitone offset)
    float   volume   = 1.0f;
    float   panning  = 0.5f;

    SamplerLoopMode loopMode = SamplerLoopMode::kNone;
    int     loopStart  = 0;
    int     loopLength = 0;

    /** Which bus this slot routes to (0..SamplerNode::kNumBuses-1 =
     *  Bus 1..N).  Default Bus 1.  The per-bus master gain lives on
     *  SamplerNode (busGain[]). */
    int     busIndex = 0;

    bool isLoaded() const noexcept { return data16L != nullptr && numSamples > 0; }
};

/** FT2-style 12-point envelope.  point.x = tick offset 0..324, point.y =
 *  0..64.  Shape matches instr_t::volEnvPoints in ft2-clone.  Flags use
 *  ENV_ENABLED / ENV_SUSTAIN / ENV_LOOP from ft2_replayer.h. */
struct FT2Envelope
{
    enum Flag { kEnabled = 1, kSustain = 2, kLoop = 4 };
    struct Point { int16_t x; int16_t y; };

    Point   points[12] {};
    uint8_t length = 0;
    uint8_t flags = 0;           // bitmask of Flag
    uint8_t sustainPoint = 0;
    uint8_t loopStart = 0;
    uint8_t loopEnd = 0;
};

/** FT2 auto-vibrato instrument parameters.  Matches instr_t::autoVib*. */
struct AutoVibParams
{
    uint8_t type  = 0;  // 0=sine, 1=square, 2=ramp-up, 3=ramp-down
    uint8_t sweep = 0;  // ramp-in time
    uint8_t depth = 0;  // 0..15
    uint8_t rate  = 0;  // 0..63
};

/** Instrument = up to 16 sample slots + a 128-entry MIDI keymap +
 *  per-instrument FT2 envelopes / fadeout / auto-vibrato.  FT2/Renoise
 *  model. */
class SamplerInstrument : public ReferenceCountedObject
{
public:
    using Ptr = ReferenceCountedObjectPtr<SamplerInstrument>;
    static constexpr int kNumSlots = 32;

    SamplerInstrument();

    bool loadSampleToSlot (int slot, const File& file, AudioFormatManager& fmt);

    /** Two-phase load — UI thread can do the file I/O (prepareSlot)
     *  without holding any sampler-side lock, then call commitSlot
     *  briefly under SamplerNode::sampleLock to publish the result.
     *  prepareSlot returns null on read failure. */
    std::unique_ptr<SamplerSampleSlot> prepareSlot (const File& file, AudioFormatManager& fmt);
    bool commitSlot (int slot, std::unique_ptr<SamplerSampleSlot> data);

    void clearSlot (int slot);

    /** Wipe every slot + reset the name, in place.  Used by the Disk Op
        Sample Bank pane's delete-key handler — the pane displays a fixed
        128-row table and needs index-stable "empty this bank" semantics
        without the shift that `SamplerNode::removeInstrument` performs. */
    void clear();

    int  slotForNote (int midiNote) const noexcept;
    void setSlotForNote (int midiNote, int slot);
    void autoSpreadKeymap();

    SamplerSampleSlot*       getSlot (int slot);
    const SamplerSampleSlot* getSlot (int slot) const;
    int firstLoadedSlot() const noexcept;
    int numLoaded() const noexcept;

    String        name;
    FT2Envelope   volumeEnv;
    FT2Envelope   panEnv;
    uint16_t      fadeoutRate = 0;   // 0 = no fadeout (sample plays out)
    AutoVibParams autoVib;

    /** Mono mode: only one voice plays per channel binding at any time;
     *  re-triggering on a held key glides pitch (last-note priority with
     *  legato release).  Portamento time is in milliseconds — 0 means
     *  instantaneous (which is a glissando but mono-stealing only). */
    bool          mono = false;
    float         portamentoTimeMs = 80.0f;

    /** When true, FT2 envelope ticks are scaled at note-on so the
     *  envelope's last point lines up with the end of the playing
     *  slot's sample — instead of running in absolute 50 Hz ticks
     *  (max ~6.5 s).  Reflects the actual musical "shape this sample"
     *  use case; default ON since envelopes are unusable as-shipped
     *  for short one-shots without this scaling. */
    bool          envSampleRelative = true;

private:
    std::array<std::unique_ptr<SamplerSampleSlot>, kNumSlots> slots;
    uint8_t noteToSlot[128] {};
    bool keymapUserModified = false;
};

/** Sample-based instrument node.  MIDI-in / stereo audio-out.
 *
 *  Up to 128 instruments, FT2-style.  MIDI channel → instrument mapping
 *  (default: channel N → instrument N; updated by MIDI program-change).
 *  Per-voice DSP via vendored ft2-clone mixer.  Each instrument carries
 *  its own FT2 vol+pan envelopes, fadeout, and auto-vibrato.  */
class SamplerNode : public BaseProcessor,
                    public juce::ChangeBroadcaster
{
public:
    /** Notify listeners (editor + Disk Op pane) that instruments[] or
     *  any slot contents changed.  Cheap, deferred to message thread
     *  internally by juce::ChangeBroadcaster.  Called from every
     *  mutation entry point (addInstrument / removeInstrument /
     *  loadSampleToSlot / setStateInformation / clearSlot wrappers
     *  / external clears in DiskOpView etc.). */
    void notifyBanksChanged() { sendChangeMessage(); }

    static constexpr int kMaxInstruments = 128;
    static constexpr int kEnvTickRateHz  = 50;   // FT2 nominal tick rate
    static constexpr int kNumBuses       = 4;    // stereo aux outputs

    /** Master gain for each of the 4 buses (0..2.0; 1.0 = unity).
     *  Slot's audio is multiplied by busGain[slot->busIndex] before
     *  being written to that bus's output channels.  UI: 4 sliders
     *  below the sample preview on the Bank page. */
    float busGain[kNumBuses] = { 1.0f, 1.0f, 1.0f, 1.0f };

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

    /* --- multi-instrument access --------------------------------------- */
    int  getNumInstruments() const;
    SamplerInstrument::Ptr getInstrument (int index) const;
    SamplerInstrument::Ptr addInstrument();
    void removeInstrument (int index);

    /** Look up the currently-routed instrument for a MIDI channel (1..16).
     *  Returns null if no instrument is bound. */
    SamplerInstrument::Ptr getInstrumentForChannel (int channel1to16) const;

    /** Bind a channel (1..16) to an instrument index.  -1 unbinds. */
    void bindChannelToInstrument (int channel1to16, int instrumentIndex);
    int  getChannelBinding (int channel1to16) const;

    /** Convenience: load a WAV into a slot of the active editor instrument. */
    bool loadSampleToSlot (int instrumentIndex, int slot, const File& file);

    /** Clear all instruments + (re-)create one default empty instrument. */
    void rebuildInstrument();

    /* --- voice / quality params --------------------------------------- */
    int  getNumVoices() const noexcept { return numVoices; }
    void setNumVoices (int n);

    enum InterpMode { kInterpNone = 0, kInterpLinear = 1, kInterpCubic = 2, kInterpSinc16 = 3 };
    InterpMode getInterpMode() const noexcept { return interpMode; }
    void       setInterpMode (InterpMode m);

    /** Global ADSR — applied as a fallback shape when an instrument has no
     *  volume envelope enabled.  When the instrument's volEnv.kEnabled
     *  flag is set, the ADSR is bypassed in favour of the FT2 envelope. */
    struct AdsrParams { float attack = 0.005f; float decay = 0.05f;
                        float sustain = 1.0f;  float release = 0.10f; };
    AdsrParams getAdsr() const { return adsrParams; }
    void setAdsr (AdsrParams p);

    int getMixFuncIndexForCurrentMode (bool loop, bool pingpong) const;

    std::vector<int> collectPlayheadsForSlot (const SamplerSampleSlot* slot) const;

    AudioFormatManager& getFormatManager() { return formatManager; }

    /** Samples per FT2 envelope tick at the current sample rate. */
    int getSamplesPerEnvTick() const noexcept;

    /** Last MIDI channel observed in noteOn — used by voices during
     *  startNote() to pick the right instrument. */
    int getLastNoteChannel() const noexcept { return lastNoteChannel; }

    /** Called by the channel-tracking synth shim on every noteOn. */
    void setLastNoteChannel (int ch) noexcept { lastNoteChannel = ch; }

protected:
    bool isBusesLayoutSupported (const BusesLayout&) const override;

private:
    void rebuildVoicePool();

    std::unique_ptr<Synthesiser> synth;
    AudioFormatManager formatManager;

    CriticalSection sampleLock;
    std::vector<SamplerInstrument::Ptr> instruments;
    std::array<int8_t, 16> channelBinding {};   // -1 = default (channel N → instrument N)

    int numVoices = 16;
    double currentSampleRate = 48000.0;
    InterpMode interpMode = kInterpLinear;
    AdsrParams adsrParams;
    int lastNoteChannel = 1;
};

} // namespace element
