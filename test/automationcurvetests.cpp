// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <boost/test/unit_test.hpp>

#include "dsp/automation/curve.hpp"
#include "dsp/automation/parameter_change_tracker.hpp"
#include "dsp/automation/automation_point.hpp"
#include "dsp/automation/automation_region.hpp"
#include "dsp/automation/automation_track.hpp"

#include <element/juce/core.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

using element::dsp::automation::AutomationMode;
using element::dsp::automation::AutomationPoint;
using element::dsp::automation::AutomationRecordMode;
using element::dsp::automation::AutomationRegion;
using element::dsp::automation::AutomationTrack;
using element::dsp::automation::CurveAlgorithm;
using element::dsp::automation::CurveOptions;
using element::dsp::automation::evaluate;
using element::dsp::automation::ParameterChangeTracker;

namespace {

inline bool nearly (double a, double b, double tol = 1e-9)
{
    return std::abs (a - b) <= tol;
}

} // namespace

BOOST_AUTO_TEST_SUITE (AutomationCurveTests)

/* ---------- Edge cases shared by every algorithm ---------- */

BOOST_AUTO_TEST_CASE (output_is_clamped_to_unit_interval)
{
    for (auto algo : { CurveAlgorithm::Linear,
                        CurveAlgorithm::Exponent,
                        CurveAlgorithm::SuperEllipse,
                        CurveAlgorithm::Vital,
                        CurveAlgorithm::Logarithmic,
                        CurveAlgorithm::Pulse })
    {
        for (double c : { -1.0, -0.5, 0.0, 0.5, 1.0 })
        {
            for (double x : { 0.0, 0.25, 0.5, 0.75, 1.0 })
            {
                for (bool startHigher : { false, true })
                {
                    const double y = evaluate (x, { algo, c }, startHigher);
                    BOOST_REQUIRE_GE (y, 0.0);
                    BOOST_REQUIRE_LE (y, 1.0);
                }
            }
        }
    }
}

BOOST_AUTO_TEST_CASE (input_outside_unit_interval_clamps)
{
    /* x < 0 -> behaves as x = 0;  x > 1 -> behaves as x = 1. */
    const CurveOptions opts { CurveAlgorithm::Linear, 0.0 };
    BOOST_CHECK (nearly (evaluate (-0.5, opts, false), evaluate (0.0, opts, false)));
    BOOST_CHECK (nearly (evaluate ( 1.5, opts, false), evaluate (1.0, opts, false)));
}

/* ---------- Linear ---------- */

BOOST_AUTO_TEST_CASE (linear_is_identity_when_ascending)
{
    const CurveOptions opts { CurveAlgorithm::Linear, 0.0 };
    BOOST_CHECK (nearly (evaluate (0.0, opts, false), 0.0));
    BOOST_CHECK (nearly (evaluate (0.5, opts, false), 0.5));
    BOOST_CHECK (nearly (evaluate (1.0, opts, false), 1.0));
}

BOOST_AUTO_TEST_CASE (linear_reflects_when_descending)
{
    const CurveOptions opts { CurveAlgorithm::Linear, 0.0 };
    BOOST_CHECK (nearly (evaluate (0.0, opts, true),  1.0));
    BOOST_CHECK (nearly (evaluate (0.5, opts, true),  0.5));
    BOOST_CHECK (nearly (evaluate (1.0, opts, true),  0.0));
}

BOOST_AUTO_TEST_CASE (linear_ignores_curviness)
{
    const CurveOptions a { CurveAlgorithm::Linear,  0.7 };
    const CurveOptions b { CurveAlgorithm::Linear, -0.7 };
    BOOST_CHECK (nearly (evaluate (0.3, a, false), evaluate (0.3, b, false)));
}

/* ---------- Exponent ---------- */

BOOST_AUTO_TEST_CASE (exponent_zero_curviness_matches_linear)
{
    /* When curviness == 0 every algorithm should degenerate to a
     * straight ramp.  Confirms the per-algorithm "if c == 0 -> x"
     * fall-through in evaluate(). */
    const CurveOptions lin { CurveAlgorithm::Linear,   0.0 };
    const CurveOptions exp { CurveAlgorithm::Exponent, 0.0 };
    for (double x : { 0.0, 0.25, 0.5, 0.75, 1.0 })
        BOOST_CHECK (nearly (evaluate (x, lin, false),
                              evaluate (x, exp, false), 1e-6));
}

BOOST_AUTO_TEST_CASE (exponent_endpoints_pin_to_zero_and_one)
{
    /* Endpoints must always pin -- otherwise points wouldn't meet
     * neighbours visually + the curve would have visible "gaps". */
    for (double c : { -0.9, -0.5, 0.5, 0.9 })
    {
        const CurveOptions opts { CurveAlgorithm::Exponent, c };
        BOOST_CHECK (nearly (evaluate (0.0, opts, false), 0.0, 1e-6));
        BOOST_CHECK (nearly (evaluate (1.0, opts, false), 1.0, 1e-6));
    }
}

/* ---------- SuperEllipse ---------- */

BOOST_AUTO_TEST_CASE (superellipse_zero_curviness_matches_linear)
{
    const CurveOptions lin { CurveAlgorithm::Linear,       0.0 };
    const CurveOptions se  { CurveAlgorithm::SuperEllipse, 0.0 };
    for (double x : { 0.0, 0.5, 1.0 })
        BOOST_CHECK (nearly (evaluate (x, lin, false),
                              evaluate (x, se, false), 1e-6));
}

BOOST_AUTO_TEST_CASE (superellipse_endpoints_pin)
{
    const CurveOptions opts { CurveAlgorithm::SuperEllipse, 0.6 };
    BOOST_CHECK (nearly (evaluate (0.0, opts, false), 0.0, 1e-6));
    BOOST_CHECK (nearly (evaluate (1.0, opts, false), 1.0, 1e-6));
}

/* ---------- Vital ---------- */

BOOST_AUTO_TEST_CASE (vital_zero_curviness_matches_linear)
{
    const CurveOptions lin { CurveAlgorithm::Linear, 0.0 };
    const CurveOptions v   { CurveAlgorithm::Vital,  0.0 };
    for (double x : { 0.0, 0.5, 1.0 })
        BOOST_CHECK (nearly (evaluate (x, lin, false),
                              evaluate (x, v, false), 1e-6));
}

/* ---------- Pulse ---------- */

BOOST_AUTO_TEST_CASE (pulse_steps_at_threshold)
{
    /* Pulse with curviness=0 steps at x = 0.5. */
    const CurveOptions opts { CurveAlgorithm::Pulse, 0.0 };
    BOOST_CHECK (nearly (evaluate (0.49, opts, false), 0.0));
    BOOST_CHECK (nearly (evaluate (0.51, opts, false), 1.0));
}

BOOST_AUTO_TEST_CASE (pulse_threshold_shifts_with_curviness)
{
    /* curviness +1 -> threshold = 1.0  (always 0).
     * curviness -1 -> threshold = 0.0  (always 1). */
    BOOST_CHECK (nearly (evaluate (0.99, { CurveAlgorithm::Pulse,  1.0 }, false), 0.0));
    BOOST_CHECK (nearly (evaluate (0.01, { CurveAlgorithm::Pulse, -1.0 }, false), 1.0));
}

/* ---------- ParameterChangeTracker ---------- */

BOOST_AUTO_TEST_SUITE (ParameterChangeTrackerTests)

BOOST_AUTO_TEST_CASE (begin_block_seeds_all_fields_from_knob)
{
    ParameterChangeTracker t;
    t.beginBlock (0.42);
    BOOST_CHECK (nearly (t.baseValue,      0.42));
    BOOST_CHECK (nearly (t.automatedValue, 0.42));
    BOOST_CHECK (nearly (t.modulatedValue, 0.42));
    BOOST_CHECK (! t.changedThisBlock);
}

BOOST_AUTO_TEST_CASE (apply_automation_overrides_base)
{
    ParameterChangeTracker t;
    t.beginBlock (0.20);
    t.applyAutomation (0.80);
    BOOST_CHECK (nearly (t.baseValue,      0.20));   /* knob untouched */
    BOOST_CHECK (nearly (t.automatedValue, 0.80));
    BOOST_CHECK (nearly (t.modulatedValue, 0.80));
}

BOOST_AUTO_TEST_CASE (finalize_block_sets_changed_when_modulated_differs)
{
    ParameterChangeTracker t;
    t.beginBlock (0.5); t.finalizeBlock();      /* first block -- changed (was 0) */
    BOOST_CHECK (t.changedThisBlock);

    t.beginBlock (0.5); t.finalizeBlock();      /* same value -- unchanged */
    BOOST_CHECK (! t.changedThisBlock);

    t.beginBlock (0.6); t.finalizeBlock();      /* different -- changed */
    BOOST_CHECK (t.changedThisBlock);

    t.beginBlock (0.6);
    t.applyAutomation (0.9);                     /* automation kicks the value */
    t.finalizeBlock();
    BOOST_CHECK (t.changedThisBlock);
}

BOOST_AUTO_TEST_SUITE_END()  /* ParameterChangeTrackerTests */

/* ---------- AutomationRegion sampling ---------- */

BOOST_AUTO_TEST_SUITE (AutomationRegionTests)

BOOST_AUTO_TEST_CASE (empty_region_samples_neutral)
{
    AutomationRegion r;
    BOOST_CHECK (nearly (r.sampleAtBeats (0.0),  0.5));
    BOOST_CHECK (nearly (r.sampleAtBeats (4.2),  0.5));
}

BOOST_AUTO_TEST_CASE (single_point_region_returns_constant)
{
    AutomationRegion r;
    AutomationRegion::PointList pts { { 0.0, 0.42, {} } };
    r.setPoints (pts);
    BOOST_CHECK (nearly (r.sampleAtBeats (0.0),  0.42));
    BOOST_CHECK (nearly (r.sampleAtBeats (5.0),  0.42));
}

BOOST_AUTO_TEST_CASE (linear_segment_interpolates)
{
    AutomationRegion r;
    AutomationRegion::PointList pts {
        { 0.0, 0.0, { CurveAlgorithm::Linear, 0.0 } },
        { 4.0, 1.0, { CurveAlgorithm::Linear, 0.0 } }
    };
    r.setPoints (pts);
    BOOST_CHECK (nearly (r.sampleAtBeats (0.0), 0.0));
    BOOST_CHECK (nearly (r.sampleAtBeats (1.0), 0.25));
    BOOST_CHECK (nearly (r.sampleAtBeats (2.0), 0.5));
    BOOST_CHECK (nearly (r.sampleAtBeats (3.0), 0.75));
    BOOST_CHECK (nearly (r.sampleAtBeats (4.0), 1.0));
}

BOOST_AUTO_TEST_CASE (linear_segment_descending_reflects)
{
    AutomationRegion r;
    AutomationRegion::PointList pts {
        { 0.0, 0.8, { CurveAlgorithm::Linear, 0.0 } },
        { 2.0, 0.2, { CurveAlgorithm::Linear, 0.0 } }
    };
    r.setPoints (pts);
    BOOST_CHECK (nearly (r.sampleAtBeats (1.0), 0.5));
}

BOOST_AUTO_TEST_CASE (out_of_range_clamps_to_endpoints)
{
    AutomationRegion r;
    AutomationRegion::PointList pts {
        { 1.0, 0.2, {} },
        { 3.0, 0.8, {} }
    };
    r.setPoints (pts);
    BOOST_CHECK (nearly (r.sampleAtBeats (0.0), 0.2));   /* before first */
    BOOST_CHECK (nearly (r.sampleAtBeats (5.0), 0.8));   /* after last */
}

BOOST_AUTO_TEST_CASE (each_curve_algo_endpoint_pins)
{
    /* For every algo, the interpolated value at t=0 must equal the
     * from-point value and at t=1 must equal the to-point value. */
    for (auto algo : { CurveAlgorithm::Linear,
                        CurveAlgorithm::Exponent,
                        CurveAlgorithm::SuperEllipse,
                        CurveAlgorithm::Vital,
                        CurveAlgorithm::Logarithmic,
                        CurveAlgorithm::Pulse })
    {
        AutomationRegion r;
        AutomationRegion::PointList pts {
            { 0.0, 0.3, { algo, 0.5 } },
            { 2.0, 0.7, { algo, 0.5 } }
        };
        r.setPoints (pts);
        BOOST_CHECK (nearly (r.sampleAtBeats (0.0), 0.3, 1e-6));
        BOOST_CHECK (nearly (r.sampleAtBeats (2.0), 0.7, 1e-6));
    }
}

BOOST_AUTO_TEST_CASE (setpoints_sorts_unordered_input)
{
    AutomationRegion r;
    AutomationRegion::PointList pts {
        { 4.0, 1.0, {} },
        { 0.0, 0.0, {} },
        { 2.0, 0.5, {} }
    };
    r.setPoints (pts);
    /* Linear interp must work correctly even though input was
     * shuffled.  If sort failed, sampleAtBeats(2.0) wouldn't be 0.5. */
    BOOST_CHECK (nearly (r.sampleAtBeats (2.0), 0.5));
}

BOOST_AUTO_TEST_CASE (cow_snapshot_swap_is_visible_atomically)
{
    /* Smoke test for the leaked-ptr-trash-bin + epoch-gated reclaim
     * pattern.  The UI side publishes a new snapshot; the audio side
     * -- a separate thread simulating the render callback -- bumps
     * the audio epoch + samples in a tight loop and sees ONE coherent
     * value, never a torn read.  Can't TSan-prove race-freedom from
     * inside the test but can confirm the published value materialises
     * atomically AND no in-flight reader observes a freed pointer. */
    AutomationRegion r;
    AutomationRegion::PointList initial { { 0.0, 0.25, {} } };
    r.setPoints (initial);

    std::atomic<bool> done { false };
    std::atomic<int>  seenA { 0 };
    std::atomic<int>  seenB { 0 };

    std::thread reader ([&] ()
    {
        while (! done.load (std::memory_order_acquire))
        {
            r.advanceAudioEpoch();           /* per-block epoch tick */
            const double v = r.sampleAtBeats (0.0);
            if (nearly (v, 0.25)) seenA.fetch_add (1, std::memory_order_relaxed);
            else if (nearly (v, 0.75)) seenB.fetch_add (1, std::memory_order_relaxed);
            else BOOST_FAIL ("torn snapshot read: " << v);
        }
    });

    /* UI thread: publish + sweep concurrent with the reader.  The
     * epoch gate must keep sweepTrash() from reclaiming any snapshot
     * the reader is currently using. */
    for (int i = 0; i < 1000; ++i)
    {
        AutomationRegion::PointList next { { 0.0, (i & 1) ? 0.75 : 0.25, {} } };
        r.setPoints (next);
        if ((i % 16) == 0)
            r.sweepTrash();
        std::this_thread::sleep_for (std::chrono::microseconds (10));
    }
    done.store (true, std::memory_order_release);
    reader.join();

    BOOST_CHECK_GT (seenA.load() + seenB.load(), 0);

    /* Final sweep -- audio thread is gone, all trash must be safely
     * reclaimable now. */
    r.advanceAudioEpoch();
    r.sweepTrash();
}

BOOST_AUTO_TEST_CASE (region_xml_round_trip)
{
    AutomationRegion r;
    r.id = juce::Uuid();
    r.positionBeats = 8.0;
    r.lengthBeats   = 4.0;
    r.looped        = true;
    AutomationRegion::PointList pts {
        { 0.0, 0.1, { CurveAlgorithm::Linear, 0.0 } },
        { 2.0, 0.9, { CurveAlgorithm::SuperEllipse, -0.4 } },
        { 4.0, 0.5, { CurveAlgorithm::Exponent, 0.3 } }
    };
    r.setPoints (pts);

    const auto vt = r.toValueTree();
    auto restored = AutomationRegion::fromValueTree (vt);
    BOOST_REQUIRE (restored != nullptr);

    BOOST_CHECK (restored->id == r.id);
    BOOST_CHECK_EQUAL (restored->positionBeats, r.positionBeats);
    BOOST_CHECK_EQUAL (restored->lengthBeats,   r.lengthBeats);
    BOOST_CHECK (restored->looped);

    /* Sampling parity: the restored region must produce the same
     * values at the same beat offsets. */
    for (double t : { 0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0 })
        BOOST_CHECK (nearly (restored->sampleAtBeats (t), r.sampleAtBeats (t), 1e-9));
}

BOOST_AUTO_TEST_SUITE_END()  /* AutomationRegionTests */

/* ---------- AutomationTrack region resolution ---------- */

BOOST_AUTO_TEST_SUITE (AutomationTrackTests)

BOOST_AUTO_TEST_CASE (empty_track_returns_no_active_region)
{
    AutomationTrack t;
    BOOST_CHECK (t.findActiveRegion (0.0) == nullptr);
    BOOST_CHECK (t.findActiveRegion (100.0) == nullptr);
}

BOOST_AUTO_TEST_CASE (single_region_resolution)
{
    AutomationTrack t;
    auto r = std::make_unique<AutomationRegion>();
    r->id = juce::Uuid();
    r->positionBeats = 4.0;
    r->lengthBeats   = 4.0;
    auto* raw = r.get();
    t.addRegion (std::move (r));

    BOOST_CHECK (t.findActiveRegion (3.999) == nullptr);
    BOOST_CHECK (t.findActiveRegion (4.0)   == raw);
    BOOST_CHECK (t.findActiveRegion (6.0)   == raw);
    BOOST_CHECK (t.findActiveRegion (8.0)   == nullptr);  /* end-exclusive */
}

BOOST_AUTO_TEST_CASE (multi_region_binary_search_with_cache)
{
    AutomationTrack t;
    AutomationRegion* a = nullptr;
    AutomationRegion* b = nullptr;
    AutomationRegion* c = nullptr;

    {
        auto r = std::make_unique<AutomationRegion>();
        r->positionBeats =  0.0; r->lengthBeats = 4.0;
        a = r.get();
        t.addRegion (std::move (r));
    }
    {
        auto r = std::make_unique<AutomationRegion>();
        r->positionBeats =  8.0; r->lengthBeats = 4.0;
        b = r.get();
        t.addRegion (std::move (r));
    }
    {
        auto r = std::make_unique<AutomationRegion>();
        r->positionBeats = 16.0; r->lengthBeats = 4.0;
        c = r.get();
        t.addRegion (std::move (r));
    }

    BOOST_CHECK (t.findActiveRegion ( 2.0) == a);
    BOOST_CHECK (t.findActiveRegion ( 5.0) == nullptr);   /* gap */
    BOOST_CHECK (t.findActiveRegion (10.0) == b);
    BOOST_CHECK (t.findActiveRegion (12.0) == nullptr);   /* end-exclusive */
    BOOST_CHECK (t.findActiveRegion (18.0) == c);

    /* Hit cache: repeated calls in the same region must reuse the
     * cached pointer.  We can't observe this directly without a
     * counter, but the call must continue returning the correct
     * region without crashing. */
    for (int i = 0; i < 100; ++i)
        BOOST_CHECK (t.findActiveRegion (18.0) == c);
}

BOOST_AUTO_TEST_CASE (remove_region_clears_cache)
{
    AutomationTrack t;
    auto r = std::make_unique<AutomationRegion>();
    r->id = juce::Uuid();
    r->positionBeats = 0.0;
    r->lengthBeats   = 4.0;
    const auto id = r->id;
    auto* raw = r.get();
    t.addRegion (std::move (r));

    BOOST_CHECK (t.findActiveRegion (1.0) == raw);
    t.removeRegion (id);
    BOOST_CHECK (t.findActiveRegion (1.0) == nullptr);
    t.sweepTrash();   /* reclaims displaced snapshot + the region */
}

BOOST_AUTO_TEST_CASE (mode_atomic_round_trip)
{
    AutomationTrack t;
    BOOST_CHECK (t.getMode() == AutomationMode::Off);
    t.setMode (AutomationMode::Read);
    BOOST_CHECK (t.getMode() == AutomationMode::Read);
    t.setMode (AutomationMode::Record);
    BOOST_CHECK (t.getMode() == AutomationMode::Record);
    t.setMode (AutomationMode::Off);
    BOOST_CHECK (t.getMode() == AutomationMode::Off);
}

BOOST_AUTO_TEST_CASE (track_xml_round_trip_internal_target)
{
    AutomationTrack t;
    t.id = juce::Uuid();
    t.targetKey.nodeId  = juce::Uuid();
    t.targetKey.paramId = "volume";
    t.setMode (AutomationMode::Read);
    t.setRecordMode (AutomationRecordMode::Latch);

    auto r = std::make_unique<AutomationRegion>();
    r->id = juce::Uuid();
    r->positionBeats = 0.0;
    r->lengthBeats   = 4.0;
    AutomationRegion::PointList pts { { 0.0, 0.0, {} }, { 4.0, 1.0, {} } };
    r->setPoints (pts);
    t.addRegion (std::move (r));

    const auto vt = t.toValueTree();
    auto restored = AutomationTrack::fromValueTree (vt);
    BOOST_REQUIRE (restored != nullptr);
    BOOST_CHECK (restored->id == t.id);
    BOOST_CHECK (restored->targetKey == t.targetKey);
    BOOST_CHECK (restored->getMode() == AutomationMode::Read);
    BOOST_CHECK (restored->getRecordMode() == AutomationRecordMode::Latch);
    BOOST_CHECK (restored->findActiveRegion (2.0) != nullptr);
}

BOOST_AUTO_TEST_CASE (write_fifo_push_drain_round_trip)
{
    AutomationTrack t;
    using element::dsp::automation::AutomationWriteEvent;

    BOOST_CHECK_EQUAL (t.getNumPendingWriteEvents(), 0);

    /* Push a handful of events from "the UI thread". */
    BOOST_REQUIRE (t.tryPushWriteEvent ({ 0.10, 1.0 }));
    BOOST_REQUIRE (t.tryPushWriteEvent ({ 0.20, 1.5 }));
    BOOST_REQUIRE (t.tryPushWriteEvent ({ 0.30, 2.0 }));
    BOOST_CHECK_EQUAL (t.getNumPendingWriteEvents(), 3);

    /* Drain on "the audio thread" -- events come out in order. */
    AutomationWriteEvent buf[8];
    const int n = t.drainWriteEvents (buf, 8);
    BOOST_REQUIRE_EQUAL (n, 3);
    BOOST_CHECK_CLOSE (buf[0].valueNormalized, 0.10, 1e-9);
    BOOST_CHECK_CLOSE (buf[1].hostBeats,       1.5,  1e-9);
    BOOST_CHECK_CLOSE (buf[2].hostBeats,       2.0,  1e-9);
    BOOST_CHECK_EQUAL (t.getNumPendingWriteEvents(), 0);
}

BOOST_AUTO_TEST_CASE (write_fifo_returns_false_when_full)
{
    AutomationTrack t;
    using element::dsp::automation::AutomationWriteEvent;

    /* AbstractFifo's published capacity is N-1 usable slots (one
     * slot is reserved as the empty/full sentinel).  Push until
     * exhausted then assert overflow returns false. */
    int pushed = 0;
    while (t.tryPushWriteEvent ({ (double) pushed / 1000.0, (double) pushed }))
        ++pushed;
    BOOST_CHECK_GT (pushed, 0);
    BOOST_CHECK_EQUAL (t.tryPushWriteEvent ({ 0.0, 0.0 }), false);

    /* Drain partial and confirm we can push again. */
    AutomationWriteEvent buf[16];
    const int drained = t.drainWriteEvents (buf, 16);
    BOOST_CHECK_GT (drained, 0);
    BOOST_CHECK (t.tryPushWriteEvent ({ 0.5, 99.0 }));
}

BOOST_AUTO_TEST_CASE (write_fifo_drain_with_zero_max_out_is_safe_noop)
{
    AutomationTrack t;
    using element::dsp::automation::AutomationWriteEvent;
    t.tryPushWriteEvent ({ 0.5, 1.0 });

    AutomationWriteEvent buf[1];
    BOOST_CHECK_EQUAL (t.drainWriteEvents (buf,     0), 0);
    BOOST_CHECK_EQUAL (t.drainWriteEvents (nullptr, 8), 0);
    BOOST_CHECK_EQUAL (t.getNumPendingWriteEvents(), 1);   /* untouched */
}

BOOST_AUTO_TEST_CASE (write_fifo_cross_thread_smoke)
{
    /* SPSC stress: one thread pushes, one thread drains in a loop.
     * Verifies no event tearing + no event loss + no leak. */
    AutomationTrack t;
    using element::dsp::automation::AutomationWriteEvent;

    constexpr int kTotal = 5000;
    std::atomic<int> drainedSum  { 0 };
    std::atomic<bool> producerDone { false };

    std::thread consumer ([&] ()
    {
        AutomationWriteEvent buf[32];
        while (true)
        {
            const int n = t.drainWriteEvents (buf, 32);
            for (int i = 0; i < n; ++i)
                drainedSum.fetch_add ((int) buf[i].valueNormalized,
                                      std::memory_order_relaxed);
            if (n == 0 && producerDone.load (std::memory_order_acquire)
                && t.getNumPendingWriteEvents() == 0)
                return;
        }
    });

    int expectedSum = 0;
    for (int i = 0; i < kTotal; ++i)
    {
        /* Spin-wait if FIFO temporarily full -- production-side
         * back-pressure rather than dropping. */
        while (! t.tryPushWriteEvent ({ (double) i, 0.0 }))
            std::this_thread::yield();
        expectedSum += i;
    }
    producerDone.store (true, std::memory_order_release);
    consumer.join();
    BOOST_CHECK_EQUAL (drainedSum.load(), expectedSum);
}

BOOST_AUTO_TEST_CASE (track_xml_round_trip_midi_target)
{
    AutomationTrack t;
    t.id = juce::Uuid();
    t.targetKey.midiCcChannel = 3;
    t.targetKey.midiCcNumber  = 74;
    t.setMode (AutomationMode::Read);

    const auto vt = t.toValueTree();
    auto restored = AutomationTrack::fromValueTree (vt);
    BOOST_REQUIRE (restored != nullptr);
    BOOST_CHECK (restored->targetKey.isMidi());
    BOOST_CHECK_EQUAL (restored->targetKey.midiCcChannel, 3);
    BOOST_CHECK_EQUAL (restored->targetKey.midiCcNumber,  74);
}

BOOST_AUTO_TEST_SUITE_END()  /* AutomationTrackTests */

BOOST_AUTO_TEST_SUITE_END()  /* AutomationCurveTests */
