/* ft2_audio_stub.c - Element-NSPA sampler engine
 *
 * Defines the single audio_t global that the vendored ft2 mixer reads
 * each callback (audio.fMixBufferL / fMixBufferR).  The SamplerNode
 * processBlock points these at its juce::AudioBuffer's write pointers
 * before invoking the mixer, and clears them after.
 *
 * The global means concurrent SamplerNode instances must not call the
 * mixer simultaneously — Element's audio engine processes graph nodes
 * sequentially on a single audio thread, so this is safe in practice.
 */

#include "ft2_audio.h"

audio_t audio = { 0 };
