// Copyright 2026 Element-NSPA <johnstonljordan@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Originally derived from NON-DAW timeline/src/Engine/types.h
// (Jonathan Moore Liles, GPLv2-or-later).  Adapted for Element-NSPA:
// removed JACK dependency (nframes_t is a plain uint32_t here, since
// the disk-streaming layer is JACK-agnostic and our audio engine uses
// JUCE's AudioBuffer / sample-count plumbing).

#pragma once

#include <cstdint>

namespace element {

using nframes_t = std::uint32_t;
using sample_t  = float;

} // namespace element
