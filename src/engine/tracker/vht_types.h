/* vht_types.h - Element-NSPA tracker engine type shim
 *
 * Vendored from rdybka/vht (vahatraker libvht). Engine originally
 * targeted JACK directly; this shim lets the engine compile without
 * <jack/jack.h> so it can be driven from JUCE processBlock instead.
 *
 * jack_nframes_t is uint32_t in JACK's headers anyway — same width,
 * same semantics. Renaming via typedef preserves the engine source
 * verbatim while removing the JACK header dependency.
 *
 * Original engine: Copyright (C) 2024 Remigiusz Dybka, GPL-3.0+
 * Shim: Element-NSPA, GPL-3.0+
 */

#ifndef VHT_TYPES_H
#define VHT_TYPES_H

#include <stdint.h>

typedef uint32_t jack_nframes_t;

/* Shared compile-time sizes used by both midi_client buffers and
 * track-side per-record arrays (track.h uses MIDI_EVT_BUFFER_LENGTH for
 * rec_upd updates[]). Original vht code put this in midi_client.h; we
 * hoist it here so headers don't need to cross-include. */
#define MIDI_EVT_BUFFER_LENGTH 1023

#endif /* VHT_TYPES_H */
