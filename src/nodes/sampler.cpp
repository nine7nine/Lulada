// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "nodes/sampler.hpp"

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
        synth.addVoice (new SamplerVoice());
}

bool SamplerNode::loadSample (const File& file)
{
    if (! file.existsAsFile()) return false;

    std::unique_ptr<AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr) return false;

    BigInteger allNotes;
    allNotes.setRange (0, 128, true);

    auto* sound = new SamplerSound (
        file.getFileNameWithoutExtension(),   // name
        *reader,                              // reader
        allNotes,                             // midi notes covered
        rootNote,                             // root note
        0.01,                                 // attack (seconds)
        0.5,                                  // release (seconds)
        30.0);                                // max sample length (seconds)

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
