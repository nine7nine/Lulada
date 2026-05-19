// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/sampler.hpp"

/* Vendored ft2-clone mixer (BSD-3-Clause). Sinc / cubic / linear /
 * no-interp variants; per-voice volume ramp; ping-pong + forward loops.
 * Activated as the per-voice DSP via Ft2SamplerVoice below.
 *
 * Stage 2 swap: juce::SamplerVoice → Ft2SamplerVoice.  The
 * juce::Synthesiser voice-pool + MIDI dispatch stay in place. */
extern "C" {
#include "engine/sampler/ft2_audio.h"
#include "engine/sampler/ft2_mix.h"
#include "engine/sampler/ft2_mix_interpolation.h"
}

namespace element {

namespace {

/* mixFunc indices into mixFuncTab (see ft2_mix.c bottom).
 * Layout: [no-ramp][8bit then 16bit][no-loop/loop/pingpong]×[no-intrp/s8/lin/s16/cubic]
 * Then [ramp][...same...]. Total 60 entries.
 *
 * For Stage 2 first cut we use 16-bit no-loop linear no-ramp = 21. */
constexpr int kMixFnIdx_16b_NoLoop_Linear_NoRamp = 21;

/** Convert float[] (mono) → int16[] for ft2's int16 mixer paths. */
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

/** One-time init of the interpolation LUTs.  Safe to call multiple
 *  times (it's idempotent on the C side but we gate just in case). */
void ensureMixerInterpolationTablesReady()
{
    static std::once_flag flag;
    std::call_once (flag, [] { setupMixerInterpolationTables(); });
}

} // anonymous namespace


/* ===========================================================================
 * Ft2SamplerSound — owns int16 sample data + original sample rate.
 * Visible to all Ft2SamplerVoices via the juce::Synthesiser sound pool.
 * ========================================================================*/
class Ft2SamplerSound : public SynthesiserSound
{
public:
    Ft2SamplerSound (const String& name_, AudioFormatReader& reader,
                     int rootMidiNote)
        : name (name_),
          rootNote (rootMidiNote),
          sourceSampleRate (reader.sampleRate)
    {
        const int64_t maxLen = (int64_t) 10 * 60 * (int64_t) reader.sampleRate;
        const int n = (int) std::min ((int64_t) reader.lengthInSamples, maxLen);
        AudioBuffer<float> tmp ((int) reader.numChannels, n);
        reader.read (&tmp, 0, n, 0, true, true);

        /* Mono only for v1 — sum stereo down. */
        if (tmp.getNumChannels() > 1)
        {
            for (int i = 0; i < n; ++i)
                tmp.setSample (0, i, 0.5f * (tmp.getSample (0, i) + tmp.getSample (1, i)));
        }

        numSamples = n;
        data16 = convertFloatToInt16 (tmp.getReadPointer (0), n);
    }

    bool appliesToNote    (int) override { return true; }
    bool appliesToChannel (int) override { return true; }

    const int16_t* getData()         const noexcept { return data16.get(); }
    int            getNumSamples()   const noexcept { return numSamples; }
    int            getRootNote()     const noexcept { return rootNote; }
    double         getSourceRate()   const noexcept { return sourceSampleRate; }
    const String&  getDisplayName()  const noexcept { return name; }

private:
    String name;
    int rootNote = 60;
    double sourceSampleRate = 48000.0;
    int numSamples = 0;
    std::unique_ptr<int16_t[]> data16;
};


/* ===========================================================================
 * Ft2SamplerVoice — per-voice DSP via the vendored ft2 mixer.
 * One voice_t per JUCE voice; startNote configures it, renderNextBlock
 * dispatches via mixFuncTab.
 * ========================================================================*/
class Ft2SamplerVoice : public SynthesiserVoice
{
public:
    Ft2SamplerVoice() = default;

    bool canPlaySound (SynthesiserSound* s) override
    {
        return dynamic_cast<Ft2SamplerSound*> (s) != nullptr;
    }

    void startNote (int midiNote, float velocity,
                    SynthesiserSound* sound, int /*pitchWheel*/) override
    {
        auto* s = dynamic_cast<Ft2SamplerSound*> (sound);
        if (s == nullptr) return;

        voice = {};
        voice.base16     = s->getData();
        voice.sampleEnd  = s->getNumSamples();
        voice.position   = 0;
        voice.positionFrac = 0;
        voice.loopType   = 0;
        voice.active     = true;
        voice.mixFuncOffset = (uint8_t) kMixFnIdx_16b_NoLoop_Linear_NoRamp;

        const double semis     = (double) midiNote - (double) s->getRootNote();
        const double pitchMul  = std::pow (2.0, semis / 12.0);
        const double playRate  = pitchMul * s->getSourceRate() / getSampleRate();
        /* delta is sample-step per output-sample, 32.32 fixed-point
         * (MIXER_FRAC_BITS=32). */
        const double scaled = playRate * 4294967296.0; // 2^32
        voice.delta = (uint64_t) jlimit (0.0, 1e18, scaled);

        const float v = juce::jlimit (0.0f, 1.0f, velocity);
        voice.fVolume        = v;
        voice.fCurrVolumeL   = v * 0.5f; // pan-centre, full-rms
        voice.fCurrVolumeR   = v * 0.5f;
        voice.fTargetVolumeL = voice.fCurrVolumeL;
        voice.fTargetVolumeR = voice.fCurrVolumeR;
        voice.fVolumeLDelta  = 0.0f;
        voice.fVolumeRDelta  = 0.0f;
        voice.volumeRampLength = 0;
    }

    void stopNote (float /*velocity*/, bool /*allowTailOff*/) override
    {
        /* v1: instant note-off.  Tail-off / fadeout via ramped mix
         * functions is Stage-2.5 polish. */
        voice.active = false;
        clearCurrentNote();
    }

    void pitchWheelMoved (int)            override {}
    void controllerMoved (int, int)       override {}

    void renderNextBlock (AudioBuffer<float>& out,
                          int startSample, int numSamples) override
    {
        if (! voice.active) return;
        if (out.getNumChannels() < 2) return;

        /* Point ft2's globals at the JUCE output buffer's L/R channels.
         * Mixer accumulates (+=) at audio.fMixBufferL[bufferPos + i] so
         * the base ptr is the buffer start, bufferPos = startSample. */
        audio.fMixBufferL = out.getWritePointer (0);
        audio.fMixBufferR = out.getWritePointer (1);

        mixFuncTab[voice.mixFuncOffset] (&voice,
                                         (uint32_t) startSample,
                                         (uint32_t) numSamples);

        /* HANDLE_SAMPLE_END in the mixer flips active=false when the
         * playhead crosses sampleEnd; reflect that to JUCE. */
        if (! voice.active)
            clearCurrentNote();
    }

private:
    voice_t voice {};
};

} // namespace element

namespace element {

/* ===========================================================================
 * SamplerNodeEditor — minimal inline editor.
 *   - Path text field (paste a .wav path; press Enter or click Load).
 *   - Status label showing current loaded sample.
 *   - Root-note slider (0..127, default 60 = C4).
 *
 * File dialog deferred until the user-led save/dialog session lands.
 * ========================================================================*/
class SamplerNodeEditor : public AudioProcessorEditor,
                          private Timer
{
public:
    explicit SamplerNodeEditor (SamplerNode& s) : AudioProcessorEditor (s), node (s)
    {
        pathEdit.setTextToShowWhenEmpty ("/path/to/sample.wav", Colours::grey);
        pathEdit.setText (node.getCurrentSamplePath(), dontSendNotification);
        pathEdit.setMultiLine (false);
        pathEdit.setReturnKeyStartsNewLine (false);
        pathEdit.onReturnKey = [this] { onLoad(); };
        addAndMakeVisible (pathEdit);

        loadBtn.setButtonText ("Load");
        loadBtn.onClick = [this] { onLoad(); };
        addAndMakeVisible (loadBtn);

        rootSlider.setRange (0.0, 127.0, 1.0);
        rootSlider.setValue ((double) node.getRootNote(), dontSendNotification);
        rootSlider.setSliderStyle (Slider::LinearBar);
        rootSlider.setTextValueSuffix ("  (root note)");
        rootSlider.onValueChange = [this] { node.setRootNote ((int) rootSlider.getValue()); };
        addAndMakeVisible (rootSlider);

        status.setJustificationType (Justification::centredLeft);
        status.setColour (Label::textColourId, Colour { 0xff'b0'b0'b0 });
        addAndMakeVisible (status);

        setOpaque (true);
        setSize (520, 140);
        refresh();
        startTimerHz (4);
    }

    ~SamplerNodeEditor() override { stopTimer(); }

    void paint (Graphics& g) override { g.fillAll (Colour { 0xff'18'18'18 }); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8);
        auto top = r.removeFromTop (28);
        loadBtn.setBounds (top.removeFromRight (72));
        top.removeFromRight (6);
        pathEdit.setBounds (top);
        r.removeFromTop (6);
        rootSlider.setBounds (r.removeFromTop (28));
        r.removeFromTop (6);
        status.setBounds (r.removeFromTop (40));
    }

private:
    void timerCallback() override { refresh(); }

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
        if (node.loadSample (f))
            status.setText ("Loaded: " + f.getFileName(), dontSendNotification);
        else
            status.setText ("Failed to read: " + f.getFileName(), dontSendNotification);
    }

    void refresh()
    {
        const auto p = node.getCurrentSamplePath();
        if (p.isNotEmpty())
            status.setText ("Sample: " + File (p).getFileName()
                            + "   |   voices " + String (node.getNumVoices())
                            + "   |   root " + String (node.getRootNote()),
                            dontSendNotification);
        else
            status.setText ("(no sample loaded)", dontSendNotification);
    }

    SamplerNode& node;
    TextEditor pathEdit;
    TextButton loadBtn;
    Slider rootSlider;
    Label status;
};

AudioProcessorEditor* SamplerNode::createEditor()
{
    return new SamplerNodeEditor (*this);
}

SamplerNode::SamplerNode()
    : BaseProcessor (BusesProperties()
                       .withOutput ("Output", AudioChannelSet::stereo(), true))
{
    ensureMixerInterpolationTablesReady();
    formatManager.registerBasicFormats();
    rebuildVoicePool();
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
    desc.descriptiveName    = "Sample-based instrument (MIDI-in / stereo audio-out)";
    desc.numInputChannels   = 0;
    desc.numOutputChannels  = 2;
    desc.hasSharedContainer = false;
    desc.isInstrument       = true;
    desc.manufacturerName   = EL_NODE_FORMAT_AUTHOR;
    desc.pluginFormatName   = EL_NODE_FORMAT_NAME;
    desc.version            = "0.1.0";
    desc.uniqueId           = EL_NODE_UID_SAMPLER;
}

void SamplerNode::prepareToPlay (double sampleRate, int maxBlock)
{
    ignoreUnused (maxBlock);
    currentSampleRate = sampleRate;
    synth.setCurrentPlaybackSampleRate (sampleRate);
}

void SamplerNode::releaseResources()
{
    synth.allNotesOff (0, true);
}

void SamplerNode::processBlock (AudioBuffer<float>& audio, MidiBuffer& midi)
{
    audio.clear();
    synth.renderNextBlock (audio, midi, 0, audio.getNumSamples());
}

bool SamplerNode::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    /* Stereo out only.  No inputs (MIDI flows through the midi-pipe,
     * not the audio bus). */
    return layouts.getMainOutputChannelSet() == AudioChannelSet::stereo();
}

void SamplerNode::rebuildVoicePool()
{
    synth.clearVoices();
    for (int i = 0; i < numVoices; ++i)
        synth.addVoice (new Ft2SamplerVoice());
}

bool SamplerNode::loadSample (const File& file)
{
    if (! file.existsAsFile()) return false;

    std::unique_ptr<AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr) return false;

    auto* sound = new Ft2SamplerSound (
        file.getFileNameWithoutExtension(),
        *reader,
        rootNote);

    {
        ScopedLock sl (sampleLock);
        synth.clearSounds();
        synth.addSound (sound);
        currentPath = file.getFullPathName();
    }
    return true;
}

String SamplerNode::getCurrentSamplePath() const
{
    ScopedLock sl (sampleLock);
    return currentPath;
}

void SamplerNode::setRootNote (int n)
{
    rootNote = jlimit (0, 127, n);
    /* SamplerSound captures root note at construction; reload to apply. */
    const auto path = getCurrentSamplePath();
    if (path.isNotEmpty())
        loadSample (File (path));
}

void SamplerNode::setNumVoices (int n)
{
    numVoices = jlimit (1, 64, n);
    rebuildVoicePool();
}

void SamplerNode::getStateInformation (MemoryBlock& dest)
{
    ValueTree tree ("sampler");
    tree.setProperty ("samplePath", getCurrentSamplePath(), nullptr);
    tree.setProperty ("rootNote",   rootNote,               nullptr);
    tree.setProperty ("numVoices",  numVoices,              nullptr);

    MemoryOutputStream out (dest, false);
    {
        GZIPCompressorOutputStream gzip (out);
        tree.writeToStream (gzip);
    }
}

void SamplerNode::setStateInformation (const void* data, int size)
{
    if (data == nullptr || size <= 0) return;
    const auto tree = ValueTree::readFromGZIPData (data, (size_t) size);
    if (! tree.isValid() || tree.getType() != Identifier ("sampler")) return;

    numVoices = (int) tree.getProperty ("numVoices", 16);
    rootNote  = (int) tree.getProperty ("rootNote",  60);
    rebuildVoicePool();

    const auto p = tree.getProperty ("samplePath").toString();
    if (p.isNotEmpty())
        loadSample (File (p));
}

} // namespace element
