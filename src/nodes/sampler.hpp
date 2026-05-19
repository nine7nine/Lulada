// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "nodes/baseprocessor.hpp"
#include "ElementApp.h"

namespace element {

/** Sample-based instrument node.  MIDI-in / stereo audio-out.
 *
 *  Stage 1: built on top of juce::Synthesiser + juce::SamplerSound +
 *  juce::SamplerVoice (framework code from JUCE's juce_audio_basics
 *  module).  One sample is loaded from disk; pitch-shifts across the
 *  MIDI keyboard via linear interpolation.  Polyphony = numVoices.
 *
 *  Stage 2 (planned): swap juce::SamplerVoice for a custom
 *  juce::SynthesiserVoice subclass whose renderNextBlock() calls into
 *  ft2-clone's mixer (sinc/cubic interp, volume ramp, ping-pong loop).
 *  The juce::Synthesiser voice-pool + MIDI dispatch stay the same. */
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

    /** Load a WAV (or any AudioFormatManager-supported file).  Replaces
     *  any previously loaded sound.  Returns true on success. Thread-safe. */
    bool loadSample (const File& file);

    String getCurrentSamplePath() const;

    int  getRootNote() const noexcept { return rootNote; }
    void setRootNote (int n);

    int  getNumVoices() const noexcept { return numVoices; }
    void setNumVoices (int n);

protected:
    bool isBusesLayoutSupported (const BusesLayout&) const override;

private:
    void rebuildVoicePool();

    Synthesiser synth;
    AudioFormatManager formatManager;

    CriticalSection sampleLock;
    String currentPath;
    int rootNote     = 60;   // C4
    int numVoices    = 16;
    double currentSampleRate = 48000.0;
};

} // namespace element
