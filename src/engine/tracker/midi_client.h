/* midi_client.h - Element-NSPA tracker engine
 *
 * Thinned replacement for vht's midi_client. Original drove JACK
 * directly; this version is a host-agnostic MIDI buffer container that
 * the engine writes into and the JUCE-side driver drains each
 * processBlock.
 *
 * The "jack_*" field names are retained so vendored vht engine source
 * (module.c, sequence.c, track.c, etc.) compiles unchanged. Underneath,
 * no JACK is involved — values come from JUCE's prepareToPlay /
 * processBlock and a host-maintained sample counter.
 *
 * Original midi_client.c/.h: Copyright (C) 2024 Remigiusz Dybka, GPL-3.0+
 * This thinned variant: Element-NSPA, GPL-3.0+
 */

#ifndef MIDI_CLIENT_H_
#define MIDI_CLIENT_H_

#include <pthread.h>
#include "vht_types.h"
#include "midi_event.h"

#define MIDI_CLIENT_MAX_PORTS 16
/* MIDI_EVT_BUFFER_LENGTH lives in vht_types.h so track.h can see it. */

/* forward decls — full struct defs come from sequence.h / track.h via
   their own includes wherever those are needed. */
struct sequence_t;
struct track_t;

typedef struct midi_client_t {
    void *mod_ref;
    int default_midi_port;
    int running;
    int freewheeling;
    int dump_notes;

    /* Host audio info — populated by JUCE-side driver each processBlock. */
    jack_nframes_t jack_sample_rate;
    jack_nframes_t jack_buffer_size;
    jack_nframes_t jack_last_frame;

    /* Mutexes around the buffers below. */
    pthread_mutex_t midi_buff_exl;
    pthread_mutex_t midi_in_buff_exl;
    pthread_mutex_t midi_ignore_buff_exl;

    /* Per-port output event buffers. Engine writes via midi_buffer_add /
       queue_midi_note_on / queue_midi_note_off / queue_midi_ctrl. JUCE
       driver reads after module_advance, sorts, emits into per-port
       JUCE MidiBuffer outputs, then calls midi_buffer_clear. */
    int curr_midi_event[MIDI_CLIENT_MAX_PORTS];
    int curr_midi_queue_event[MIDI_CLIENT_MAX_PORTS];
    midi_event midi_buffer[MIDI_CLIENT_MAX_PORTS][MIDI_EVT_BUFFER_LENGTH];
    midi_event midi_queue_buffer[MIDI_CLIENT_MAX_PORTS][MIDI_EVT_BUFFER_LENGTH];

    /* MIDI-in event ring (UI-facing activity display, host-thread reader). */
    int curr_midi_in_event;
    midi_event midi_in_buffer[MIDI_EVT_BUFFER_LENGTH];

    /* MIDI-in ignore list (used by record-thru to suppress echo). */
    int curr_midi_ignore_event;
    midi_event midi_ignore_buffer[MIDI_EVT_BUFFER_LENGTH];
} midi_client;

/* lifecycle */
midi_client *midi_client_new(void *mod);
void midi_client_free(midi_client *clt);

/* mutex helpers (engine calls these around buffer access) */
void midi_buff_excl_in(midi_client *clt);
void midi_buff_excl_out(midi_client *clt);
void midi_in_buff_excl_in(midi_client *clt);
void midi_in_buff_excl_out(midi_client *clt);
void midi_ignore_buff_excl_in(midi_client *clt);
void midi_ignore_buff_excl_out(midi_client *clt);

/* output buffer ops */
void midi_buffer_clear(midi_client *clt);
void midi_buffer_add(midi_client *clt, int port, midi_event evt);
int midi_buffer_compare(const void *a, const void *b);

/* engine-side queue helpers — called from track.c on row triggers */
void queue_midi_note_on(midi_client *clt, struct sequence_t *seq,
                        int port, int chn, int note, int velocity);
void queue_midi_note_off(midi_client *clt, struct sequence_t *seq,
                         int port, int chn, int note);
void queue_midi_ctrl(midi_client *clt, struct sequence_t *seq,
                     struct track_t *trk, int val, int ctrl);

/* MIDI-in (populated by host driver, consumed by engine in module_advance) */
void midi_in_buffer_add(midi_client *clt, midi_event evt);
void midi_in_clear_events(midi_client *clt);
void midi_ignore_buffer_clear(midi_client *clt);
void midi_ignore_buffer_add(midi_client *clt, int channel, int type, int note);

/* state flags */
void midi_set_freewheel(midi_client *clt, int on);

/* No-op stubs — preserved for engine source compatibility. The
   JUCE-side driver handles transport + port discovery natively. */
void midi_synch_output_ports(midi_client *clt);
void midi_send_transp(midi_client *clt, int play, long frames);

#endif /* MIDI_CLIENT_H_ */
