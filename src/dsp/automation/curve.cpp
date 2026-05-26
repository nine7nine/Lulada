// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dsp/automation/curve.hpp"

#include <algorithm>
#include <cmath>

namespace element::dsp::automation {

namespace {

/* Equality check tolerant of double epsilon -- a curviness of 0
 * computed from a slider can show up as ~1e-17 instead of exact zero. */
inline bool nearZero (double v) noexcept
{
    return std::abs (v) < 1e-10;
}

} // namespace

double evaluate (double x, CurveOptions opts, bool startHigher) noexcept
{
    x = std::clamp (x, 0.0, 1.0);
    const bool curveUp = opts.curviness >= 0.0;
    double val = -1.0;

    switch (opts.algorithm)
    {
        case CurveAlgorithm::Linear:
        {
            /* Linear ignores curviness entirely.  The reflection convention
             * matches Exponent so a Linear segment between two points
             * draws the expected straight ramp regardless of direction. */
            if (! startHigher) val = x;
            else               val = 1.0 - x;
            break;
        }

        case CurveAlgorithm::Exponent:
        {
            double c = opts.curviness * kExponentCurvinessBound;
            c = 1.0 - std::abs (c);
            if (! startHigher) x = 1.0 - x;
            if (curveUp)        x = 1.0 - x;
            val = nearZero (c) ? x : std::pow (x, c);
            if (! curveUp) val = 1.0 - val;
            break;
        }

        case CurveAlgorithm::SuperEllipse:
        {
            double c = opts.curviness * kSuperEllipseCurvinessBound;
            c = 1.0 - std::abs (c);
            if (! startHigher) x = 1.0 - x;
            if (curveUp)        x = 1.0 - x;
            val = nearZero (c)
                    ? x
                    : std::pow (1.0 - std::pow (x, c), 1.0 / c);
            if (curveUp) val = 1.0 - val;
            break;
        }

        case CurveAlgorithm::Vital:
        {
            double c = opts.curviness * kVitalCurvinessBound;
            c = -c * 10.0;
            if (startHigher) x = 1.0 - x;
            val = nearZero (c) ? x : std::expm1 (c * x) / std::expm1 (c);
            break;
        }

        case CurveAlgorithm::Logarithmic:
        {
            /* Two-branch logarithmic curve -- the inner exponent must be
             * derived from curviness magnitude (the sign is consumed by
             * curveUp).  The expression below matches Zrythm's; comments
             * inline to keep the derivation discoverable. */
            constexpr float bound = 1e-12f;
            const float mag = std::clamp (
                static_cast<float> (std::abs (opts.curviness)),
                0.01f, 1.f - bound);
            const float s = mag * 10.f;
            const float c = std::clamp ((10.f - s) / std::pow (s, s),
                                          bound, 10.f);

            if (! startHigher) x = 1.0 - x;
            if (curveUp)        x = 1.0 - x;

            const float a  = std::log (c);
            const float b  = 1.f / std::log (1.f + (1.f / c));
            const float xf = static_cast<float> (x);
            float fval;
            if (curveUp)
                fval = (std::log (xf + c) - a) * b;
            else
                fval = (a - std::log (xf + c)) * b + 1.f;
            val = static_cast<double> (fval);
            break;
        }

        case CurveAlgorithm::Pulse:
        {
            /* Hard step at (1 + curviness)/2.  Useful for stepped
             * automation segments without dragging in a separate "step"
             * region type. */
            const double threshold = (1.0 + opts.curviness) * 0.5;
            val = (threshold > x) ? 0.0 : 1.0;
            if (startHigher) val = 1.0 - val;
            break;
        }
    }

    return std::clamp (val, 0.0, 1.0);
}

} // namespace element::dsp::automation
