// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace element::dsp::automation {

/** Algorithmic curve families used by the automation engine + piano-
 *  roll CC lanes.  Each algorithm interprets `curviness` differently
 *  (per-family bound + sign convention); see `evaluate` for the
 *  derivation.  Math reimplemented from Zrythm's `src/dsp/curve.cpp`
 *  (LicenseRef-Zrythm); pure functions, no code copied.
 *
 *  Why algorithmic over fixed Bezier handles: gives the user a wider
 *  expressive palette without exposing two-handle drag UX, and stays
 *  cheap at audio rate (single std::pow / expm1 / logf per sample). */
enum class CurveAlgorithm : std::uint8_t
{
    Linear = 0,    /**< Straight ramp.  Equivalent to Exponent + curviness=0
                     *   but called out explicitly so the default case in
                     *   the dispatch is a free fall-through. */
    Exponent,      /**< y = x^n.  n derived from curviness via
                     *   EXPONENT_CURVINESS_BOUND.  Classic ease-in / ease-out. */
    SuperEllipse,  /**< y = 1 - (1 - x^n)^(1/n).  From the Tracktion engine.
                     *   Smoother shoulder than pure exponent. */
    Vital,         /**< (e^(nx) - 1) / (e^n - 1).  From the Vital synth's
                     *   modulation matrix.  Asymmetric exponential -- more
                     *   musical than Exponent for many sweeps. */
    Logarithmic,   /**< Ardour-style log curve.  Useful for filter / pitch
                     *   sweeps where the perceived rate of change should be
                     *   constant in log-frequency. */
    Pulse,         /**< Hard step at (1 + curviness)/2.  Effectively a
                     *   stepped automation segment. */
};

/** Per-segment curve descriptor stored on each AutomationPoint.  The
 *  curve describes the shape FROM this point TO the next point in
 *  time order. */
struct CurveOptions
{
    CurveAlgorithm algorithm { CurveAlgorithm::Linear };
    /** Curve "amount" in [-1, +1].  Sign chooses convex vs concave
     *  (per-algorithm convention); magnitude scales by the algorithm's
     *  curviness bound.  curviness=0 is always equivalent to Linear
     *  regardless of algorithm. */
    double         curviness { 0.0 };
};

/* Per-algorithm curviness bounds -- match Zrythm's so curves saved by
 * either tool look identical at the same curviness value. */
inline constexpr double kSuperEllipseCurvinessBound = 0.82;
inline constexpr double kExponentCurvinessBound     = 0.95;
inline constexpr double kVitalCurvinessBound        = 1.00;

/** Sample the curve at normalised x in [0, 1] and return normalised y
 *  in [0, 1].
 *
 *  @param x  Position along the segment, 0 = start, 1 = end.  Clamped
 *            to [0, 1] -- callers don't need to pre-clamp.
 *  @param opts  Curve shape + amount.
 *  @param startHigher  True when the segment starts at a higher value
 *            than it ends (i.e. the segment is a descent).  Controls
 *            the curve's reflection so the same `curviness` reads as
 *            the same "shape" whether the segment rises or falls. */
double evaluate (double x, CurveOptions opts, bool startHigher) noexcept;

} // namespace element::dsp::automation
