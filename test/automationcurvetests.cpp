// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <boost/test/unit_test.hpp>

#include "dsp/automation/curve.hpp"
#include "dsp/automation/parameter_change_tracker.hpp"

#include <cmath>

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

BOOST_AUTO_TEST_SUITE_END()  /* AutomationCurveTests */
