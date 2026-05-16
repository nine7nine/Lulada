// Copyright 2023 Kushview, LLC <info@kushview.net>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce/audio_devices.hpp>
#include <element/audioengine.hpp>

namespace element {

class Settings;

class DeviceManager : public juce::AudioDeviceManager {
public:
    typedef juce::AudioDeviceManager::AudioDeviceSetup AudioSettings;
    static const int maxAudioChannels;

    DeviceManager();
    ~DeviceManager();

    void getAudioDrivers (juce::StringArray& drivers);
    void selectAudioDriver (const juce::String& name);
    void attach (AudioEnginePtr engine);

    /** Element-NSPA: apply Settings-backed JACK port counts to the
     *  embedded JackClient.  Called by Context after both DeviceManager
     *  and Settings have been constructed.  Safe to call again when
     *  the user changes the audio preferences — caller is responsible
     *  for triggering a device restart (closeAudioDevice +
     *  restartLastAudioDevice) so the new counts take effect. */
    void applyJackPortCountsFromSettings (Settings& settings);

#if KV_JACK_AUDIO
    kv::JackClient& getJackClient();
#endif

    void createAudioDeviceTypes (juce::OwnedArray<juce::AudioIODeviceType>& list) override;

private:
    friend class World;
    class Private;
    std::unique_ptr<Private> impl;
};

} // namespace element
