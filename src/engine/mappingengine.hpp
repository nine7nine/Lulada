// SPDX-FileCopyrightText: 2023 Kushview, LLC
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <element/juce.hpp>
#include <element/controller.hpp>
#include <element/signals.hpp>

#include <atomic>

namespace element {

class ControllerMapHandler;
class ControllerMapInput;
class Processor;
class Node;
class MidiEngine;
namespace automation { class AutomationEngine; }

class MappingEngine
{
public:
    using CapturedEventSignal = Signal<void()>;

    MappingEngine();
    ~MappingEngine();

    bool addInput (const Controller&, MidiEngine&);
    bool addHandler (const Control&, const Node&, const int);

    bool removeInput (const Controller&);
    bool refreshInput (const Controller&);
    void clear();
    void startMapping();
    void stopMapping();

    void capture (const bool start = true) { capturedEvent.capture.set (start); }
    juce::MidiMessage getCapturedMidiMessage() const { return capturedEvent.message; }
    Control getCapturedControl() const { return capturedEvent.control; }
    CapturedEventSignal& capturedSignal() { return capturedEvent.callback; }

    /** Wire the per-graph AutomationEngine so this MappingEngine's
     *  handlers can consult its mute table before each
     *  setValueNotifyingHost write.  Called by RootGraph on
     *  construction (passes the graph's engine pointer) + on
     *  destruction (passes nullptr to clear).  Atomic store so the
     *  MIDI thread sees the change without a lock. */
    void setAutomationEngine (automation::AutomationEngine* e) noexcept
    {
        automationEngine.store (e, std::memory_order_release);
    }

    /** MIDI-thread accessor used by ControllerMapHandler subclasses
     *  before they call setValueNotifyingHost.  Returns nullptr when
     *  no graph has wired itself in (e.g. test bench, headless tools)
     *  -- callers must null-check + treat absence as "not muted". */
    automation::AutomationEngine* getAutomationEngine() const noexcept
    {
        return automationEngine.load (std::memory_order_acquire);
    }

private:
    friend class ControllerMapInput;
    class Inputs;
    std::unique_ptr<Inputs> inputs;

    /** Live AutomationEngine pointer.  Set by RootGraph + read by
     *  ControllerMapHandler subclasses on the MIDI thread.  Lifetime
     *  is RootGraph's responsibility -- cleared back to nullptr in
     *  RootGraph's dtor before the engine is destroyed. */
    std::atomic<automation::AutomationEngine*> automationEngine { nullptr };

    class CapturedEvent : public juce::AsyncUpdater
    {
    public:
        CapturedEvent() { capture.set (false); }
        ~CapturedEvent() {}
        inline void handleAsyncUpdate() override
        {
            capture.set (false);
            callback();
        }

    private:
        friend class MappingEngine;
        juce::Atomic<bool> capture;
        Control control;
        juce::MidiMessage message;
        CapturedEventSignal callback;
    } capturedEvent;

    bool captureNextEvent (ControllerMapInput&, const Control&, const juce::MidiMessage&);
};

} // namespace element
