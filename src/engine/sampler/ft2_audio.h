/* ft2_audio.h - Element-NSPA sampler engine
 *
 * Thinned-down extract from 8bitbubsy/ft2-clone (BSD-3-Clause).
 *
 * Upstream's ft2_audio.h pulls in SDL2 + ft2_replayer.h for an audio
 * device manager, BPM/tick tables, etc. — we want NONE of that. The
 * vendored mixer (ft2_mix.c / ft2_mix_interpolation.c / ft2_mix_macros.h)
 * references exactly:
 *   - struct voice_t  (the per-voice mixer state)
 *   - audio.fMixBufferL[]  +  audio.fMixBufferR[]
 *
 * So this file defines a minimal audio_t with just those two fields,
 * plus voice_t verbatim from upstream. The JUCE-side Ft2SamplerVoice
 * sets the buffers each processBlock and clears them after.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* === voice_t — verbatim from upstream ft2_audio.h ====================== */
typedef struct
{
    const int8_t *base8, *revBase8;
    const int16_t *base16, *revBase16;
    bool active, samplingBackwards, isFadeOutVoice, hasLooped;
    uint8_t scopeVolume, mixFuncOffset, panning, loopType;
    int32_t position, sampleEnd, loopStart, loopLength;
    uint32_t volumeRampLength;
    uint64_t positionFrac, delta, scopeDelta;

    // if (loopEnabled && hasLooped && samplingPos <= loopStart+MAX_LEFT_TAPS)
    //     readFixedTapsFromThisPointer();
    const int8_t *leftEdgeTaps8;
    const int16_t *leftEdgeTaps16;

    const float *fSincLUT;
    float fVolume, fCurrVolumeL, fCurrVolumeR;
    float fVolumeLDelta, fVolumeRDelta, fTargetVolumeL, fTargetVolumeR;
} voice_t;

/* === audio_t — thinned to just the output bus pointers + ramp const === */
typedef struct
{
    float *fMixBufferL, *fMixBufferR;
    float fQuickVolRampSamplesMul; /* unused by no-ramp mix funcs */
} audio_t;

extern audio_t audio;

#ifdef __cplusplus
}
#endif
