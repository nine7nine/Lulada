// SPDX-FileCopyrightText: 2014-2019 Kushview, LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#if ELEMENT_USE_JACK

#include <atomic>

#include <jack/jack.h>

#include <element/juce/audio_devices.hpp>

namespace element {

class JackCallback;
class JackClient;
class JackPort;

struct Jack
{
    static const char* audioPort;
    static const char* midiPort;
    static juce::AudioIODeviceType* createAudioIODeviceType (JackClient&);
    static int getClientNameSize();
    static int getPortNameSize();
};

/** Element-NSPA: sample-accurate JACK MIDI input handoff.
 *
 *  JUCE's AudioIODeviceCallback API has no MIDI parameter, so the JACK
 *  driver exposes per-period MIDI events through this side-channel:
 *
 *    1. The JACK process callback (RT) drains native JACK MIDI events
 *       directly into a per-device juce::MidiBuffer with each event's
 *       jack_midi_event_t::time preserved as the sample offset within
 *       the current period.
 *    2. AudioEngine's audio callback runs on the same RT thread,
 *       synchronously dispatched from the JACK process callback.
 *    3. AudioEngine queries getCurrentJackMidiInput() at the start of
 *       graph processing and merges those events into the MidiBuffer
 *       handed to the plugin graph — same period, same sample offsets.
 *
 *  Result: zero added latency, sample-accurate, no consumer-thread
 *  jitter.  The buffer reference is valid only for the duration of the
 *  audio callback that dispatched it; it is cleared at the start of
 *  each new JACK process callback. */
class JackMidiInputProvider
{
public:
    virtual ~JackMidiInputProvider() = default;

    /** Combined buffer carrying events from every configured JACK MIDI
        input port for the current period.  Consumed by AudioEngine to
        feed the graph's top-level MidiBuffer (the route used by the
        Graph I/O "MIDI Input" pseudo-node). */
    virtual const juce::MidiBuffer& getCurrentJackMidiInput() const noexcept = 0;

    /** Per-port buffer for selective routing.  Index N corresponds to
        the JACK port "midi_in_<N+1>".  Empty for out-of-range indices
        or disabled ports.  Used by JackMidiInputNode to feed events
        from a specific port into a downstream node chain (per-source
        routing — e.g. controller A → synth A, controller B → synth B). */
    virtual const juce::MidiBuffer& getCurrentJackMidiInputForPort (int portIndex) const noexcept = 0;

    /** Number of configured JACK MIDI input ports (matches the value
        last set on JackClient via setNumMidiInputs).  Bounds the valid
        range for getCurrentJackMidiInputForPort. */
    virtual int getNumJackMidiInputPorts() const noexcept = 0;
};

/** Element-NSPA: outbound JACK MIDI sink — symmetric counterpart of
 *  JackMidiInputProvider.  Implemented by JackAudioIODevice, queried
 *  via dynamic_cast from JackMidiOutputNode.
 *
 *  Calls land on the same RT thread as the JACK process callback
 *  (Element's audio callback IS the JACK process callback in the
 *  middle of its work).  pushMidiEvent writes to a lock-free
 *  jack_ringbuffer (mlocked) — the JACK process callback drains it
 *  into the actual midi_out_N port buffers at the start of the next
 *  period.  That's a one-period delay; acceptable for outbound MIDI
 *  (Wine drivers + most DAWs run the same shape). */
class JackMidiOutputSink
{
public:
    virtual ~JackMidiOutputSink() = default;

    /** Queue a MIDI event for delivery on element:midi_out_<portIndex+1>.
        Returns true if the event was queued (ringbuffer had room); false
        on full ring or out-of-range port index.  The data buffer is
        copied; caller does not need to keep it alive. */
    virtual bool pushJackMidiOutput (int portIndex, const juce::uint8* data, int size) noexcept = 0;

    /** Number of configured JACK MIDI output ports.  Bounds the valid
        range for pushJackMidiOutput. */
    virtual int getNumJackMidiOutputPorts() const noexcept = 0;
};

class JackPort final : public juce::ReferenceCountedObject
{
public:
    using Ptr = juce::ReferenceCountedObjectPtr<JackPort>;

    ~JackPort();

    void* getBuffer (uint32_t nframes);
    const char* getName() const;
    bool isInput() const;
    bool isOutput() const;
    bool isAudio() const;
    bool isMidi() const;
    int connect (const JackPort& other);
    int getFlags() const;

    operator jack_port_t*() const { return port; }

private:
    friend class JackClient;
    JackPort (JackClient& c, jack_port_t* p);
    JackClient& client;
    jack_port_t* port;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JackPort)
};

using JackStatus = jack_status_t;

class JackClient final
{
public:
    /** Make a new JACK client.
     *
     *  numMainInputs / numMainOutputs:
     *    > 0  — force this many JACK input/output ports regardless of
     *           hardware.  JackAudioIODevice creates the requested
     *           count and JUCE's audio engine sees N channels.  Useful
     *           for plugin-host workloads that want more channels than
     *           the connected interface exposes physically.
     *    = 0  — auto, mirror physical hardware port count (upstream
     *           behaviour).
     *
     *  Default is 0 / 0 so a freshly-constructed JackClient preserves
     *  the upstream mirror-hardware semantics unless explicitly
     *  overridden via the setters below or the Element Audio
     *  preferences panel.
     */
    explicit JackClient (const juce::String& name = juce::String(),
                         int numMainInputs = 0,
                         const juce::String& mainInputPrefix = "main_in_",
                         int numMainOutputs = 0,
                         const juce::String& mainOutputPrefix = "main_out_");

    ~JackClient();

    int getNumMainInputs() const { return numMainIns; }
    int getNumMainOutputs() const { return numMainOuts; }

    /** Set the forced JACK input port count.  > 0 forces N inputs;
     *  0 reverts to mirror-hardware behaviour.  Takes effect on the
     *  next JackAudioIODevice open — caller is responsible for
     *  triggering an audio-device restart for the change to apply. */
    void setNumMainInputs (int n) noexcept  { numMainIns  = n > 0 ? n : 0; }
    void setNumMainOutputs (int n) noexcept { numMainOuts = n > 0 ? n : 0; }

    /** Element-NSPA: forced JACK MIDI port counts.  Same semantics as
     *  the audio port count setters above.  N MIDI input ports are
     *  registered as "midi_in_1".."midi_in_N" and N MIDI output ports
     *  as "midi_out_1".."midi_out_M".  Takes effect on next audio
     *  device open. */
    int getNumMidiInputs()  const { return numMidiIns; }
    int getNumMidiOutputs() const { return numMidiOuts; }
    void setNumMidiInputs  (int n) noexcept { numMidiIns  = n > 0 ? n : 0; }
    void setNumMidiOutputs (int n) noexcept { numMidiOuts = n > 0 ? n : 0; }

    /** Element-NSPA: per-port enable bitmasks for native JACK MIDI.
        Bit N (0-indexed) controls whether events from / to midi_in_N+1
        / midi_out_N+1 are routed through the JACK driver's RT drain/
        fill paths.  Read with relaxed atomics from the RT JACK process
        callback once per period — single load, negligible cost.  UI
        toggles in the MIDI preferences panel write through these
        setters; changes take effect on the next JACK period without a
        device restart. */
    uint32_t getMidiInputEnableMask()  const noexcept { return midiInEnableMask.load (std::memory_order_relaxed); }
    uint32_t getMidiOutputEnableMask() const noexcept { return midiOutEnableMask.load (std::memory_order_relaxed); }
    void setMidiInputEnableMask  (uint32_t m) noexcept { midiInEnableMask.store  (m, std::memory_order_relaxed); }
    void setMidiOutputEnableMask (uint32_t m) noexcept { midiOutEnableMask.store (m, std::memory_order_relaxed); }

    /** Convenience: toggle a single port bit (0-indexed) preserving
        the other bits.  Loads and stores are relaxed — UI thread
        writes, RT thread reads; the only requirement is atomicity per
        access, which std::atomic<uint32_t> guarantees on all our
        target ISAs. */
    void setMidiInputPortEnabled  (int portIndex, bool enabled) noexcept
    {
        if (portIndex < 0 || portIndex >= 32) return;
        const uint32_t bit = 1u << static_cast<unsigned> (portIndex);
        uint32_t cur = midiInEnableMask.load (std::memory_order_relaxed);
        uint32_t next;
        do { next = enabled ? (cur | bit) : (cur & ~bit); }
        while (! midiInEnableMask.compare_exchange_weak (cur, next, std::memory_order_relaxed));
    }

    void setMidiOutputPortEnabled (int portIndex, bool enabled) noexcept
    {
        if (portIndex < 0 || portIndex >= 32) return;
        const uint32_t bit = 1u << static_cast<unsigned> (portIndex);
        uint32_t cur = midiOutEnableMask.load (std::memory_order_relaxed);
        uint32_t next;
        do { next = enabled ? (cur | bit) : (cur & ~bit); }
        while (! midiOutEnableMask.compare_exchange_weak (cur, next, std::memory_order_relaxed));
    }

    bool isMidiInputPortEnabled  (int portIndex) const noexcept
    {
        if (portIndex < 0 || portIndex >= 32) return false;
        return (getMidiInputEnableMask()  & (1u << static_cast<unsigned> (portIndex))) != 0u;
    }

    bool isMidiOutputPortEnabled (int portIndex) const noexcept
    {
        if (portIndex < 0 || portIndex >= 32) return false;
        return (getMidiOutputEnableMask() & (1u << static_cast<unsigned> (portIndex))) != 0u;
    }

    const juce::String& getMainOutputPrefix() const { return mainOutPrefix; }
    const juce::String& getMainInputPrefix() const { return mainInPrefix; }

    /** Open the client */
    JackStatus open (int options);

    /** Close the client */
    juce::String close();

    /** Returns true if client is open */
    bool isOpen() const;

    /** Activate the client */
    int activate();

    /** Deactivate the client */
    int deactivate();

    /** Returns true if the client is active */
    bool isActive();

    /** Register a new port */
    JackPort::Ptr registerPort (const juce::String& name, const juce::String& type, int flags, int bufsize = 0);

    /** Returns the client's name */
    juce::String getName();

    /** Returns the current sample rate */
    int getSampleRate();

    /** Returns the current buffer size */
    int getBufferSize();

    /** Query for ports */
    void getPorts (juce::StringArray& dest, juce::String nameRegex = {}, juce::String typeRegex = {}, uint64_t flags = 0);

    /** Element-NSPA: return the list of remote ports currently connected
        to one of Element's own JACK MIDI ports.  portIndex is 0-based
        and resolved against the configured port count + naming scheme
        ("<client>:midi_in_<N+1>" / "<client>:midi_out_<N+1>").  Used by
        the MIDI preferences panel to show a live "connected to: …"
        label per port.  Empty result if the client isn't open or the
        port hasn't been registered yet. */
    juce::StringArray getMidiPortConnections (int portIndex, bool isInput);

    operator jack_client_t*() const { return client; }

private:
    JackClient (const JackClient&);
    JackClient& operator= (const JackClient&);
    jack_client_t* client;
    juce::String name, mainInPrefix, mainOutPrefix;
    int numMainIns, numMainOuts;
    /* Element-NSPA: forced MIDI port counts (0 = none).  Unlike the
     * audio counts, there's no "mirror hardware" mode for MIDI — JACK
     * MIDI is a host-declared concept, so 0 simply means no Element-
     * exposed JACK MIDI ports.  Counts > 0 cause JackAudioIODevice to
     * register that many JACK MIDI ports + add per-period drain/fill
     * to the audio process callback. */
    int numMidiIns  = 0;
    int numMidiOuts = 0;
    /* Element-NSPA: per-port enable bitmasks (bit N = port N+1).
     * Default = all enabled.  Live-readable from the RT JACK process
     * callback via relaxed atomics; UI mutates via the setters above. */
    std::atomic<uint32_t> midiInEnableMask  { ~0u };
    std::atomic<uint32_t> midiOutEnableMask { ~0u };
    juce::Array<JackPort::Ptr> ports;
};

} // namespace element
#endif
