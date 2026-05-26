// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <boost/test/unit_test.hpp>

#include "services/automation/automation_target.hpp"

#include <element/parameter.hpp>
#include <element/juce/audio_basics.hpp>
#include <element/juce/audio_processors.hpp>

#include <atomic>

using element::automation::AutomationTarget;

namespace {

/* Minimal capture-only juce::AudioProcessorParameter subclass.
 * Records the most recent setValue() call so tests can assert. */
class FakePluginParam : public juce::AudioProcessorParameter
{
public:
    std::atomic<float> last { -1.0f };
    int                setCallCount { 0 };

    float getValue() const override                                  { return last.load(); }
    void  setValue (float v) override                                { last.store (v); ++setCallCount; }
    float getDefaultValue() const override                           { return 0.5f; }
    juce::String getName (int) const override                        { return "fake"; }
    juce::String getLabel() const override                           { return {}; }
    float getValueForText (const juce::String&) const override       { return 0.f; }
};

/* Minimal element::Parameter -- just to drive setValue capture.
 * Not registered with any Processor; getParameterIndex() == -1 is
 * fine for this test since we never call beginChangeGesture. */
class FakeNodeParam : public element::Parameter
{
public:
    std::atomic<float> last { -1.0f };
    int                setCallCount { 0 };

    int   getPortIndex()    const noexcept override { return 0; }
    int   getParameterIndex() const noexcept override { return 0; }
    float getValue()        const override          { return last.load(); }
    void  setValue (float v) override               { last.store (v); ++setCallCount; }
    float getDefaultValue() const override          { return 0.5f; }
    float getValueForText (const juce::String&) const override { return 0.f; }
    juce::String getName (int) const override       { return "fake"; }
    juce::String getLabel() const override          { return {}; }
};

} // namespace

BOOST_AUTO_TEST_SUITE (AutomationTargetTests)

BOOST_AUTO_TEST_CASE (default_constructed_target_is_invalid)
{
    AutomationTarget t;
    BOOST_CHECK (! t.isValid());
    BOOST_CHECK (t.kind == AutomationTarget::Kind::Invalid);
}

BOOST_AUTO_TEST_CASE (invalid_target_write_is_noop)
{
    AutomationTarget t;
    juce::MidiBuffer buf;
    t.writeCoarseValue (0.5f, &buf);
    BOOST_CHECK (buf.isEmpty());
    /* Nothing observable -- the test is that we don't crash on a
     * default-constructed (Invalid) target. */
}

BOOST_AUTO_TEST_CASE (plugin_param_write_coarse_dispatches_to_setvalue)
{
    FakePluginParam p;
    AutomationTarget t;
    t.kind        = AutomationTarget::Kind::PluginParam;
    t.pluginParam = &p;

    t.writeCoarseValue (0.42f);
    BOOST_CHECK_EQUAL (p.setCallCount, 1);
    BOOST_CHECK_CLOSE (p.last.load(), 0.42f, 1e-4);

    /* Out-of-range clamps. */
    t.writeCoarseValue (-1.0f);
    BOOST_CHECK_CLOSE (p.last.load(), 0.0f, 1e-4);
    t.writeCoarseValue (2.0f);
    BOOST_CHECK_CLOSE (p.last.load(), 1.0f, 1e-4);
}

BOOST_AUTO_TEST_CASE (node_param_write_coarse_dispatches_to_setvalue)
{
    juce::ReferenceCountedObjectPtr<FakeNodeParam> p { new FakeNodeParam() };
    AutomationTarget t;
    t.kind      = AutomationTarget::Kind::NodeParam;
    t.nodeParam = p.get();  /* ParameterPtr is ref-counted; just up-cast */

    t.writeCoarseValue (0.75f);
    BOOST_CHECK_EQUAL (p->setCallCount, 1);
    BOOST_CHECK_CLOSE (p->last.load(), 0.75f, 1e-4);
}

BOOST_AUTO_TEST_CASE (midi_cc_write_coarse_emits_event_at_offset_zero)
{
    AutomationTarget t;
    t.kind         = AutomationTarget::Kind::MidiCc;
    t.midiChannel  = 3;
    t.midiCcNumber = 74;

    juce::MidiBuffer buf;
    t.writeCoarseValue (1.0f, &buf);

    BOOST_REQUIRE_EQUAL (buf.getNumEvents(), 1);
    juce::MidiBuffer::Iterator it (buf);
    juce::MidiMessage msg;
    int samplePos = -1;
    BOOST_REQUIRE (it.getNextEvent (msg, samplePos));
    BOOST_CHECK_EQUAL (samplePos, 0);
    BOOST_CHECK (msg.isController());
    BOOST_CHECK_EQUAL (msg.getChannel(),          3);
    BOOST_CHECK_EQUAL (msg.getControllerNumber(), 74);
    BOOST_CHECK_EQUAL (msg.getControllerValue(),  127);
}

BOOST_AUTO_TEST_CASE (midi_cc_write_coarse_without_buffer_is_noop)
{
    AutomationTarget t;
    t.kind         = AutomationTarget::Kind::MidiCc;
    t.midiChannel  = 1;
    t.midiCcNumber = 7;
    /* No buffer pointer -> the engine is operating in a no-MIDI-out
     * context this block; drop silently rather than crashing. */
    t.writeCoarseValue (0.5f, nullptr);
}

BOOST_AUTO_TEST_CASE (midi_cc_emit_event_at_frame_offset)
{
    AutomationTarget t;
    t.kind         = AutomationTarget::Kind::MidiCc;
    t.midiChannel  = 1;
    t.midiCcNumber = 1;

    juce::MidiBuffer buf;
    t.emitEventAt (128, 0.5f, buf);
    t.emitEventAt (256, 0.6f, buf);

    BOOST_REQUIRE_EQUAL (buf.getNumEvents(), 2);

    juce::MidiBuffer::Iterator it (buf);
    juce::MidiMessage msg;
    int samplePos = -1;

    BOOST_REQUIRE (it.getNextEvent (msg, samplePos));
    BOOST_CHECK_EQUAL (samplePos, 128);
    BOOST_CHECK_EQUAL (msg.getControllerValue(), juce::roundToInt (0.5f * 127.f));

    BOOST_REQUIRE (it.getNextEvent (msg, samplePos));
    BOOST_CHECK_EQUAL (samplePos, 256);
    BOOST_CHECK_EQUAL (msg.getControllerValue(), juce::roundToInt (0.6f * 127.f));
}

BOOST_AUTO_TEST_CASE (midi_channel_zero_defaults_to_one)
{
    /* JUCE conventions: channel 0 is invalid.  Engine binding may
     * leave midiChannel at 0 when the user picked "any channel" --
     * coerce to channel 1 on emit so we don't produce malformed MIDI. */
    AutomationTarget t;
    t.kind         = AutomationTarget::Kind::MidiCc;
    t.midiChannel  = 0;
    t.midiCcNumber = 10;

    juce::MidiBuffer buf;
    t.writeCoarseValue (0.5f, &buf);

    BOOST_REQUIRE_EQUAL (buf.getNumEvents(), 1);
    juce::MidiBuffer::Iterator it (buf);
    juce::MidiMessage msg;
    int samplePos = -1;
    BOOST_REQUIRE (it.getNextEvent (msg, samplePos));
    BOOST_CHECK_EQUAL (msg.getChannel(), 1);
}

BOOST_AUTO_TEST_CASE (non_midi_target_emit_event_is_noop)
{
    /* emitEventAt is only meaningful for MidiCc -- plugin/node param
     * SA consumption is the node's responsibility in Phase 1.
     * Calling it on a plugin target must not crash + must not write
     * anything to the buffer. */
    FakePluginParam p;
    AutomationTarget t;
    t.kind        = AutomationTarget::Kind::PluginParam;
    t.pluginParam = &p;

    juce::MidiBuffer buf;
    t.emitEventAt (64, 0.5f, buf);
    BOOST_CHECK (buf.isEmpty());
    BOOST_CHECK_EQUAL (p.setCallCount, 0);  /* not a coarse write either */
}

BOOST_AUTO_TEST_SUITE_END()
