/* ft2_mix_simd.c - Element-NSPA sampler engine — AVX2 SIMD mixer paths
 *
 * SIMD variants of the four hottest 16-bit linear-interp mixers from
 * ft2_mix.c.  They match the scalar code BIT-FOR-BIT — same int32
 * (s1-s0)*frac>>15 math, same volume ramp semantics, same LIMIT_MIX_NUM
 * driven outer loop, same WRAP_LOOP / HANDLE_SAMPLE_END exit.
 *
 * Covered variants:
 *   - mix16bNoLoopLIntrp     (index 21)
 *   - mix16bLoopLIntrp       (index 22)
 *   - mix16bRampNoLoopLIntrp (index 51)
 *   - mix16bRampLoopLIntrp   (index 52)
 *
 * Dispatch:
 *   mixFuncDispatch[] is a writable copy of mixFuncTab[].  ft2_mix_init()
 *   patches the four entries above with their _AVX2 versions if the host
 *   CPU supports AVX2+FMA.  Callers (sampler.cpp) read from
 *   mixFuncDispatch[] instead of mixFuncTab[].
 *
 * Pingpong / cubic / sinc / 8-bit paths keep their scalar entries — only
 * the most common path (16-bit + linear interp) is SIMD-accelerated.
 * Adding more later is mechanical.
 *
 * Future: SSE4.1 path
 * ~~~~~~~~~~~~~~~~~~~
 * SSE4.1 has _mm_mullo_epi32 + _mm_srai_epi32 so the integer
 * (s1-s0)*frac>>15 + add s0 part vectorizes the same way at 4-lane width.
 * The blocker is that SSE has NO gather instruction — _mm_i32gather_*
 * arrived with AVX2 — so each 4-lane chunk would need 4 scalar loads to
 * fetch (s0, s1) pairs.  That kills most of the speedup on a SSE-only
 * CPU.  Cubic / sinc are still pure scalar even on AVX2 hosts so they
 * are higher-leverage SIMD targets than an SSE backport.  Revisit
 * if/when sustained pre-AVX2 hardware (e.g. Bulldozer / pre-Haswell)
 * shows up as a real load.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "ft2_audio.h"
#include "ft2_mix.h"
#include "ft2_mix_macros.h"

/* ----------------------------------------------------------------------- */
/*                          Dispatch table                                 */
/* ----------------------------------------------------------------------- */

/* mixFuncTab[] has 60 entries (15 variants × 4 = 8bit-noramp, 16bit-noramp,
 * 8bit-ramp, 16bit-ramp).  See ft2_mix.c for layout. */
#define FT2_MIX_TAB_SIZE 60

mixFunc mixFuncDispatch[FT2_MIX_TAB_SIZE];

/* Indices into the dispatch table (must match mixFuncTab[] order in
 * ft2_mix.c).  Layout: each 15-entry block = 5 interp modes × 3 loop modes.
 * Within a block: idx = interpOffset*3 + loopOff (where loopOff is
 * 0=NoLoop, 1=Loop, 2=Pingpong).  Interp order: None, Sinc8, Linear,
 * Sinc16, Cubic. */
#define FT2_IDX_16B_LINTRP_NOLOOP       (15 + 2*3 + 0)   /* 21 */
#define FT2_IDX_16B_LINTRP_LOOP         (15 + 2*3 + 1)   /* 22 */
#define FT2_IDX_16B_RAMP_LINTRP_NOLOOP  (45 + 2*3 + 0)   /* 51 */
#define FT2_IDX_16B_RAMP_LINTRP_LOOP    (45 + 2*3 + 1)   /* 52 */

/* ----------------------------------------------------------------------- */
/*                          AVX2 mix kernels                               */
/* ----------------------------------------------------------------------- */

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))

#include <immintrin.h>

/* Per-iteration scalar precompute that feeds the SIMD gather.  Computes
 * 8 lane offsets (sample index into base16, relative to chunk-start
 * smpPtr) and 8 lane fractions (the low 32 bits of positionFrac at each
 * lane).  After the 8-pack:
 *   *outNextOff   = total integer advance to apply to smpPtr
 *   *outNextFrac  = positionFrac after 8 steps
 *
 * Matches scalar INC_POS semantics exactly:
 *   positionFrac += delta
 *   smpPtr += positionFrac >> 32
 *   positionFrac &= MIXER_FRAC_MASK   */
static inline __attribute__((always_inline))
void simd_precompute_lanes (int32_t offs[8], uint32_t fracs[8],
                            int32_t *outNextOff, uint64_t *outNextFrac,
                            uint64_t startFrac, uint64_t delta)
{
    int32_t  cumOff = 0;
    uint64_t curF   = startFrac;
    for (int k = 0; k < 8; ++k)
    {
        offs[k]  = cumOff;
        fracs[k] = (uint32_t) curF;
        uint64_t next = curF + delta;
        cumOff += (int32_t)(next >> MIXER_FRAC_BITS);
        curF    = next & MIXER_FRAC_MASK;
    }
    *outNextOff  = cumOff;
    *outNextFrac = curF;
}

/* SIMD core: takes 8 fully-prepared lane samples and writes them into
 * mix buffers with the supplied volume vectors.  Used by all four
 * variants — only the volume-vector construction differs (ramp vs.
 * flat) between callers. */
static inline __attribute__((always_inline, target("avx2,fma")))
void simd_mix8_16bit_lintrp (const int16_t *smpPtr,
                             const int32_t offs[8], const uint32_t fracs[8],
                             __m256 vVolL, __m256 vVolR,
                             float *fMixBufferL, float *fMixBufferR)
{
    /* Gather 8 × 32-bit words at (smpPtr + offs[k]*sizeof(int16_t)).
     * Each word packs (s0, s1) as little-endian (s0 in low 16, s1 in
     * high 16).  scale=2 → byte-stride = offs[k] * 2 bytes. */
    const __m256i vOffs  = _mm256_loadu_si256 ((const __m256i*) offs);
    const __m256i vWords = _mm256_i32gather_epi32 ((const int*) smpPtr, vOffs, 2);

    /* Extract s0 (low 16) and s1 (high 16) as sign-extended int32. */
    const __m256i vS0 = _mm256_srai_epi32 (_mm256_slli_epi32 (vWords, 16), 16);
    const __m256i vS1 = _mm256_srai_epi32 (vWords, 16);

    /* Bit-exact scalar formula:
     *   frac15  = (frac >> 17) & 0x7FFF                     // top 15 bits
     *   sample  = (s0 + ((s1 - s0) * frac15) >> 15)         // int32
     *   fSample = sample * (1.0f / 32768.0f)
     */
    const __m256i vFracs   = _mm256_loadu_si256 ((const __m256i*) fracs);
    const __m256i vFrac15  = _mm256_srli_epi32 (vFracs, 17);            /* 0..32767 */
    const __m256i vDiff    = _mm256_sub_epi32 (vS1, vS0);
    const __m256i vMul     = _mm256_mullo_epi32 (vDiff, vFrac15);
    const __m256i vShifted = _mm256_srai_epi32 (vMul, 15);
    const __m256i vResult  = _mm256_add_epi32 (vS0, vShifted);

    /* Convert to float and scale. */
    const __m256  vScale   = _mm256_set1_ps (1.0f / 32768.0f);
    const __m256  vSample  = _mm256_mul_ps (_mm256_cvtepi32_ps (vResult), vScale);

    /* Multiply by per-lane volume and accumulate into mix buffer. */
    __m256 vML = _mm256_loadu_ps (fMixBufferL);
    __m256 vMR = _mm256_loadu_ps (fMixBufferR);
    vML = _mm256_fmadd_ps (vSample, vVolL, vML);
    vMR = _mm256_fmadd_ps (vSample, vVolR, vMR);
    _mm256_storeu_ps (fMixBufferL, vML);
    _mm256_storeu_ps (fMixBufferR, vMR);
}

/* ----------------------------------------------------------------------- */
/*  No-ramp variants                                                       */
/* ----------------------------------------------------------------------- */

__attribute__((target("avx2,fma")))
static void mix16bNoLoopLIntrp_AVX2 (voice_t *v, audio_t *audio, uint32_t bufferPos, uint32_t numSamples)
{
    const int16_t *base, *smpPtr;
    float fSample, *fMixBufferL, *fMixBufferR;
    int32_t position;
    uint32_t i, samplesToMix, samplesLeft;
    uint64_t positionFrac;

    GET_VOL
    GET_MIXER_VARS
    SET_BASE16

    const __m256 vVolL = _mm256_set1_ps (fVolumeL);
    const __m256 vVolR = _mm256_set1_ps (fVolumeR);

    samplesLeft = numSamples;
    while (samplesLeft > 0)
    {
        LIMIT_MIX_NUM
        samplesLeft -= samplesToMix;

        i = 0;
        while (i + 8 <= samplesToMix)
        {
            int32_t  offs[8];
            uint32_t fracs[8];
            int32_t  nextOff;
            uint64_t nextFrac;
            simd_precompute_lanes (offs, fracs, &nextOff, &nextFrac, positionFrac, delta);

            simd_mix8_16bit_lintrp (smpPtr, offs, fracs, vVolL, vVolR, fMixBufferL, fMixBufferR);

            fMixBufferL += 8;
            fMixBufferR += 8;
            smpPtr      += nextOff;
            positionFrac = nextFrac;
            i += 8;
        }

        for (; i < samplesToMix; ++i)
        {
            RENDER_16BIT_SMP_LINTRP
            INC_POS
        }

        HANDLE_SAMPLE_END
    }

    SET_BACK_MIXER_POS
}

__attribute__((target("avx2,fma")))
static void mix16bLoopLIntrp_AVX2 (voice_t *v, audio_t *audio, uint32_t bufferPos, uint32_t numSamples)
{
    const int16_t *base, *smpPtr;
    float fSample, *fMixBufferL, *fMixBufferR;
    int32_t position;
    uint32_t i, samplesToMix, samplesLeft;
    uint64_t positionFrac;

    GET_VOL
    GET_MIXER_VARS
    SET_BASE16

    const __m256 vVolL = _mm256_set1_ps (fVolumeL);
    const __m256 vVolR = _mm256_set1_ps (fVolumeR);

    samplesLeft = numSamples;
    while (samplesLeft > 0)
    {
        LIMIT_MIX_NUM
        samplesLeft -= samplesToMix;

        i = 0;
        while (i + 8 <= samplesToMix)
        {
            int32_t  offs[8];
            uint32_t fracs[8];
            int32_t  nextOff;
            uint64_t nextFrac;
            simd_precompute_lanes (offs, fracs, &nextOff, &nextFrac, positionFrac, delta);

            simd_mix8_16bit_lintrp (smpPtr, offs, fracs, vVolL, vVolR, fMixBufferL, fMixBufferR);

            fMixBufferL += 8;
            fMixBufferR += 8;
            smpPtr      += nextOff;
            positionFrac = nextFrac;
            i += 8;
        }

        for (; i < samplesToMix; ++i)
        {
            RENDER_16BIT_SMP_LINTRP
            INC_POS
        }

        WRAP_LOOP
    }

    SET_BACK_MIXER_POS
}

/* ----------------------------------------------------------------------- */
/*  Ramp variants                                                          */
/* ----------------------------------------------------------------------- */

__attribute__((target("avx2,fma")))
static void mix16bRampNoLoopLIntrp_AVX2 (voice_t *v, audio_t *audio, uint32_t bufferPos, uint32_t numSamples)
{
    const int16_t *base, *smpPtr;
    float fSample, *fMixBufferL, *fMixBufferR;
    int32_t position;
    float fVolumeLDelta, fVolumeRDelta, fVolumeL, fVolumeR;
    uint32_t i, samplesToMix, samplesLeft;
    uint64_t positionFrac;

    GET_VOL_RAMP
    GET_MIXER_VARS_RAMP
    SET_BASE16

    /* Per-lane increment vector: {0, 1, 2, 3, 4, 5, 6, 7}.  Used to
     * build the volume vector for each 8-lane chunk:
     *   vVol_k = vVol_base + lane_k * vVolDelta   */
    const __m256 vLane = _mm256_setr_ps (0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f);

    samplesLeft = numSamples;
    while (samplesLeft > 0)
    {
        LIMIT_MIX_NUM
        LIMIT_MIX_NUM_RAMP
        samplesLeft -= samplesToMix;

        i = 0;
        while (i + 8 <= samplesToMix)
        {
            int32_t  offs[8];
            uint32_t fracs[8];
            int32_t  nextOff;
            uint64_t nextFrac;
            simd_precompute_lanes (offs, fracs, &nextOff, &nextFrac, positionFrac, delta);

            const __m256 vVolDelL = _mm256_set1_ps (fVolumeLDelta);
            const __m256 vVolDelR = _mm256_set1_ps (fVolumeRDelta);
            const __m256 vVolL    = _mm256_fmadd_ps (vLane, vVolDelL, _mm256_set1_ps (fVolumeL));
            const __m256 vVolR    = _mm256_fmadd_ps (vLane, vVolDelR, _mm256_set1_ps (fVolumeR));

            simd_mix8_16bit_lintrp (smpPtr, offs, fracs, vVolL, vVolR, fMixBufferL, fMixBufferR);

            fMixBufferL += 8;
            fMixBufferR += 8;
            smpPtr      += nextOff;
            positionFrac = nextFrac;
            fVolumeL    += 8.0f * fVolumeLDelta;
            fVolumeR    += 8.0f * fVolumeRDelta;
            i += 8;
        }

        for (; i < samplesToMix; ++i)
        {
            RENDER_16BIT_SMP_LINTRP
            VOLUME_RAMPING
            INC_POS
        }

        HANDLE_SAMPLE_END
    }

    SET_VOL_BACK
    SET_BACK_MIXER_POS
}

__attribute__((target("avx2,fma")))
static void mix16bRampLoopLIntrp_AVX2 (voice_t *v, audio_t *audio, uint32_t bufferPos, uint32_t numSamples)
{
    const int16_t *base, *smpPtr;
    float fSample, *fMixBufferL, *fMixBufferR;
    int32_t position;
    float fVolumeLDelta, fVolumeRDelta, fVolumeL, fVolumeR;
    uint32_t i, samplesToMix, samplesLeft;
    uint64_t positionFrac;

    GET_VOL_RAMP
    GET_MIXER_VARS_RAMP
    SET_BASE16

    const __m256 vLane = _mm256_setr_ps (0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f);

    samplesLeft = numSamples;
    while (samplesLeft > 0)
    {
        LIMIT_MIX_NUM
        LIMIT_MIX_NUM_RAMP
        samplesLeft -= samplesToMix;

        i = 0;
        while (i + 8 <= samplesToMix)
        {
            int32_t  offs[8];
            uint32_t fracs[8];
            int32_t  nextOff;
            uint64_t nextFrac;
            simd_precompute_lanes (offs, fracs, &nextOff, &nextFrac, positionFrac, delta);

            const __m256 vVolDelL = _mm256_set1_ps (fVolumeLDelta);
            const __m256 vVolDelR = _mm256_set1_ps (fVolumeRDelta);
            const __m256 vVolL    = _mm256_fmadd_ps (vLane, vVolDelL, _mm256_set1_ps (fVolumeL));
            const __m256 vVolR    = _mm256_fmadd_ps (vLane, vVolDelR, _mm256_set1_ps (fVolumeR));

            simd_mix8_16bit_lintrp (smpPtr, offs, fracs, vVolL, vVolR, fMixBufferL, fMixBufferR);

            fMixBufferL += 8;
            fMixBufferR += 8;
            smpPtr      += nextOff;
            positionFrac = nextFrac;
            fVolumeL    += 8.0f * fVolumeLDelta;
            fVolumeR    += 8.0f * fVolumeRDelta;
            i += 8;
        }

        for (; i < samplesToMix; ++i)
        {
            RENDER_16BIT_SMP_LINTRP
            VOLUME_RAMPING
            INC_POS
        }

        WRAP_LOOP
    }

    SET_VOL_BACK
    SET_BACK_MIXER_POS
}

#endif /* __x86_64__ */

/* ----------------------------------------------------------------------- */
/*                          Init / dispatch patch                          */
/* ----------------------------------------------------------------------- */

extern const mixFunc mixFuncTab[]; /* defined in ft2_mix.c */

void ft2_mix_init (void)
{
    /* Default: scalar entries everywhere. */
    memcpy (mixFuncDispatch, mixFuncTab, sizeof (mixFunc) * FT2_MIX_TAB_SIZE);

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    if (__builtin_cpu_supports ("avx2") && __builtin_cpu_supports ("fma"))
    {
        mixFuncDispatch[FT2_IDX_16B_LINTRP_NOLOOP]      = (mixFunc) mix16bNoLoopLIntrp_AVX2;
        mixFuncDispatch[FT2_IDX_16B_LINTRP_LOOP]        = (mixFunc) mix16bLoopLIntrp_AVX2;
        mixFuncDispatch[FT2_IDX_16B_RAMP_LINTRP_NOLOOP] = (mixFunc) mix16bRampNoLoopLIntrp_AVX2;
        mixFuncDispatch[FT2_IDX_16B_RAMP_LINTRP_LOOP]   = (mixFunc) mix16bRampLoopLIntrp_AVX2;
    }
#endif
}
