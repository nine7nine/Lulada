#pragma once

#include <stdint.h>
#include "ft2_audio.h"  /* audio_t — threaded through every mix call */

enum
{
	// don't change the order of these! (yes, it looks weird)
	INTERPOLATION_DISABLED = 0,
	INTERPOLATION_SINC8    = 1,
	INTERPOLATION_LINEAR   = 2,
	INTERPOLATION_SINC16   = 3,
	INTERPOLATION_CUBIC    = 4,
	// ------

	NUM_INTERPOLATORS,
};

#define MAX_TAPS 16
#define MAX_LEFT_TAPS ((MAX_TAPS/2)-1)
#define MAX_RIGHT_TAPS (MAX_TAPS/2)

// the fractional bits are hardcoded, changing these will break things!
#define MIXER_FRAC_BITS 32

#define MIXER_FRAC_SCALE ((int64_t)1 << MIXER_FRAC_BITS)
#define MIXER_FRAC_MASK (MIXER_FRAC_SCALE-1)

/* Mix function signature: voice + per-call audio context + bufferPos +
 * numSamples.  The audio_t pointer was historically a global; threading
 * it per-call enables parallel mix targets (per-thread scratch buffers
 * in voice-pool parallel and graph multithreading). */
typedef void (*mixFunc)(void *v, audio_t *audio, uint32_t bufferPos, uint32_t numSamples);

extern const mixFunc mixFuncTab[];      // ft2_mix.c — scalar variants only
extern       mixFunc mixFuncDispatch[]; // ft2_mix_simd.c — SIMD overrides applied at init

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the dispatch table.  Copies scalar mixFuncTab entries into
 * mixFuncDispatch then patches in SIMD variants where the host CPU
 * supports the required ISA (currently AVX2+FMA for 16-bit linear).  Safe
 * to call multiple times — idempotent. */
void ft2_mix_init (void);

#ifdef __cplusplus
}
#endif
