// Copyright 2014-2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <element/devices.hpp>
#include <element/settings.hpp>
#include "engine/jack.hpp"

namespace element {

using namespace juce;

const int DeviceManager::maxAudioChannels = 128;

class DeviceManager::Private
{
public:
    Private (DeviceManager& o) : owner (o) {}
    ~Private() {}

    DeviceManager& owner;
    AudioEnginePtr engine;
#if ELEMENT_USE_JACK
    JackClient jack { "Element", 2, "main_in_", 2, "main_out_" };
#endif

    juce::ReferenceCountedArray<DeviceManager::LevelMeter> levelsIn, levelsOut;
};

DeviceManager::DeviceManager()
{
    impl = std::make_unique<Private> (*this);
}

DeviceManager::~DeviceManager()
{
    closeAudioDevice();
    attach (nullptr);
}

void DeviceManager::attach (AudioEnginePtr engine)
{
    if (impl->engine == engine)
        return;

    auto old = impl->engine;

    if (old != nullptr)
    {
        removeAudioCallback (&old->getAudioIODeviceCallback());
    }

    if (engine)
    {
        addAudioCallback (&engine->getAudioIODeviceCallback());
    }
    else
    {
        closeAudioDevice();
    }

    impl->engine = engine;
}

static void addIfNotNull (OwnedArray<AudioIODeviceType>& list, AudioIODeviceType* const device)
{
    if (device != nullptr)
        list.add (device);
}

void DeviceManager::createAudioDeviceTypes (OwnedArray<AudioIODeviceType>& list)
{
#if JUCE_ALSA
    addIfNotNull (list, AudioIODeviceType::createAudioIODeviceType_ALSA());
#endif
#if ELEMENT_USE_JACK
    addIfNotNull (list, Jack::createAudioIODeviceType (impl->jack));
#endif

    addIfNotNull (list, AudioIODeviceType::createAudioIODeviceType_ASIO());
    addIfNotNull (list, AudioIODeviceType::createAudioIODeviceType_WASAPI (WASAPIDeviceMode::exclusive));
    addIfNotNull (list, AudioIODeviceType::createAudioIODeviceType_WASAPI (WASAPIDeviceMode::sharedLowLatency));
    addIfNotNull (list, AudioIODeviceType::createAudioIODeviceType_DirectSound());

    addIfNotNull (list, AudioIODeviceType::createAudioIODeviceType_CoreAudio());

    addIfNotNull (list, AudioIODeviceType::createAudioIODeviceType_iOSAudio());

    addIfNotNull (list, AudioIODeviceType::createAudioIODeviceType_OpenSLES());
    addIfNotNull (list, AudioIODeviceType::createAudioIODeviceType_Android());
}

void DeviceManager::getAudioDrivers (StringArray& drivers)
{
    const OwnedArray<AudioIODeviceType>& types (getAvailableDeviceTypes());
    for (int i = 0; i < types.size(); ++i)
        drivers.add (types.getUnchecked (i)->getTypeName());
}

void DeviceManager::selectAudioDriver (const String& name)
{
    setCurrentAudioDeviceType (name, true);
}

#if ELEMENT_USE_JACK
JackClient& DeviceManager::getJackClient()
{
    return impl->jack;
}
#endif

void DeviceManager::applyJackPortCountsFromSettings (Settings& settings)
{
   #if ELEMENT_USE_JACK
    /* Element-NSPA: read the persisted forced port counts and push
     * them into the embedded JackClient.  0 = auto (mirror physical
     * hardware), > 0 = create exactly N JACK ports.  The change takes
     * effect on the next JackAudioIODevice open. */
    const int inCount  = settings.getInt (Settings::audioJackInputPortCountKey,  0);
    const int outCount = settings.getInt (Settings::audioJackOutputPortCountKey, 0);
    impl->jack.setNumMainInputs  (inCount);
    impl->jack.setNumMainOutputs (outCount);

    /* Same shape for MIDI: 0 means no Element-exposed JACK MIDI ports
     * (falls through to JUCE's ALSA-seq backend via MidiInput); > 0
     * registers N native JACK MIDI ports with sample-accurate
     * timestamps via the JackAudioIODevice MIDI drain/fill path. */
    const int midiInCount  = settings.getInt (Settings::audioJackInputMidiPortCountKey,  0);
    const int midiOutCount = settings.getInt (Settings::audioJackOutputMidiPortCountKey, 0);
    impl->jack.setNumMidiInputs  (midiInCount);
    impl->jack.setNumMidiOutputs (midiOutCount);

    /* Element-NSPA: per-port enable masks.  Default = ~0u (all enabled)
     * preserves "everything works" behaviour for users who haven't
     * touched the MIDI preferences toggles.  Setting::getInt returns a
     * signed int so the cast normalises -1 / ~0 to the same all-ones
     * bitmask. */
    const uint32_t midiInMask  = static_cast<uint32_t> (
        settings.getInt (Settings::audioJackInputMidiPortEnableMaskKey,  static_cast<int> (~0u)));
    const uint32_t midiOutMask = static_cast<uint32_t> (
        settings.getInt (Settings::audioJackOutputMidiPortEnableMaskKey, static_cast<int> (~0u)));
    impl->jack.setMidiInputEnableMask  (midiInMask);
    impl->jack.setMidiOutputEnableMask (midiOutMask);
   #else
    juce::ignoreUnused (settings);
   #endif
}

} // namespace element
