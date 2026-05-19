/* midi_client.c - Element-NSPA tracker engine
 *
 * See midi_client.h. Thinned host-agnostic buffer container; no JACK.
 *
 * Original: Copyright (C) 2024 Remigiusz Dybka, GPL-3.0+
 * This variant: Element-NSPA, GPL-3.0+
 */

#include <stdlib.h>
#include <string.h>
#include "midi_client.h"
#include "module.h"
#include "sequence.h"
#include "track.h"

midi_client *midi_client_new(void *mod) {
    midi_client *clt = calloc(1, sizeof(midi_client));
    if (!clt) return NULL;

    clt->mod_ref = mod;
    clt->default_midi_port = 0;
    clt->jack_sample_rate = 48000;
    clt->jack_buffer_size = 1024;
    clt->jack_last_frame = 0;

    pthread_mutex_init(&clt->midi_buff_exl, NULL);
    pthread_mutex_init(&clt->midi_in_buff_exl, NULL);
    pthread_mutex_init(&clt->midi_ignore_buff_exl, NULL);

    return clt;
}

void midi_client_free(midi_client *clt) {
    if (!clt) return;
    pthread_mutex_destroy(&clt->midi_buff_exl);
    pthread_mutex_destroy(&clt->midi_in_buff_exl);
    pthread_mutex_destroy(&clt->midi_ignore_buff_exl);
    free(clt);
}

void midi_buff_excl_in(midi_client *clt)  { pthread_mutex_lock(&clt->midi_buff_exl); }
void midi_buff_excl_out(midi_client *clt) { pthread_mutex_unlock(&clt->midi_buff_exl); }
void midi_in_buff_excl_in(midi_client *clt)  { pthread_mutex_lock(&clt->midi_in_buff_exl); }
void midi_in_buff_excl_out(midi_client *clt) { pthread_mutex_unlock(&clt->midi_in_buff_exl); }
void midi_ignore_buff_excl_in(midi_client *clt)  { pthread_mutex_lock(&clt->midi_ignore_buff_exl); }
void midi_ignore_buff_excl_out(midi_client *clt) { pthread_mutex_unlock(&clt->midi_ignore_buff_exl); }

void midi_buffer_clear(midi_client *clt) {
    for (int p = 0; p < MIDI_CLIENT_MAX_PORTS; p++) {
        clt->curr_midi_event[p] = 0;
    }
}

void midi_buffer_add(midi_client *clt, int port, midi_event evt) {
    if (port < 0 || port >= MIDI_CLIENT_MAX_PORTS) return;
    if (clt->curr_midi_event[port] == MIDI_EVT_BUFFER_LENGTH) return;

    module *mod = (module *) clt->mod_ref;
    if (mod && mod->panic && evt.type == note_on) return;

    clt->midi_buffer[port][clt->curr_midi_event[port]++] = evt;
}

int midi_buffer_compare(const void *a, const void *b) {
    int res = (int)((midi_event *)a)->time - (int)((midi_event *)b)->time;
    if (res == 0) res = ((midi_event *)a)->channel - ((midi_event *)b)->channel;
    if (res == 0) res = ((midi_event *)b)->type - ((midi_event *)a)->type;
    if (res == 0) res = ((midi_event *)a)->note - ((midi_event *)b)->note;
    if (res == 0) res = ((midi_event *)b)->velocity - ((midi_event *)a)->velocity;
    return res;
}

/* queue_midi_note_on/off/ctrl are called from track.c on row triggers.
 * They store the event in the per-port queue buffer; the queue is
 * merged into the main midi_buffer at the start of each processBlock
 * by the JUCE-side driver (it walks midi_queue_buffer → midi_buffer).
 *
 * Live-recording path (mod->recording && mod->playing) is intentionally
 * left as a TODO — Phase 2+ work; for Phase 1 the engine is read-only
 * playback from a hardcoded pattern. */
void queue_midi_note_on(midi_client *clt, struct sequence_t *seq,
                        int port, int chn, int note, int velocity) {
    (void)seq;
    if (port < 0 || port >= MIDI_CLIENT_MAX_PORTS) return;

    midi_event evt;
    evt.type = note_on;
    evt.channel = (unsigned char)chn;
    evt.note = (unsigned char)note;
    evt.velocity = (unsigned char)velocity;
    evt.time = 0;

    midi_buff_excl_in(clt);
    if (clt->curr_midi_queue_event[port] < MIDI_EVT_BUFFER_LENGTH) {
        clt->midi_queue_buffer[port][clt->curr_midi_queue_event[port]++] = evt;
    }
    midi_buff_excl_out(clt);
}

void queue_midi_note_off(midi_client *clt, struct sequence_t *seq,
                         int port, int chn, int note) {
    (void)seq;
    if (port < 0 || port >= MIDI_CLIENT_MAX_PORTS) return;

    midi_event evt;
    evt.type = note_off;
    evt.channel = (unsigned char)chn;
    evt.note = (unsigned char)note;
    evt.velocity = 0;
    evt.time = 0;

    midi_buff_excl_in(clt);
    if (clt->curr_midi_queue_event[port] < MIDI_EVT_BUFFER_LENGTH) {
        clt->midi_queue_buffer[port][clt->curr_midi_queue_event[port]++] = evt;
    }
    midi_buff_excl_out(clt);
}

void queue_midi_ctrl(midi_client *clt, struct sequence_t *seq,
                     struct track_t *trk, int val, int ctrl) {
    (void)seq;
    if (!trk) return;

    midi_event evt;
    evt.type = control_change;
    evt.channel = (unsigned char)trk->channel;
    evt.note = (unsigned char)(ctrl > -1 ? ctrl : 0);
    evt.velocity = (unsigned char)val;
    evt.time = 0;

    /* Update lctrlvals so the engine's own CC tracking stays consistent. */
    pthread_mutex_lock(&trk->exclctrl);
    if (ctrl == -1) {
        if (trk->lctrlval) trk->lctrlval[0] = val * 127;
    } else {
        for (int c = 0; c < trk->nctrl; c++) {
            if (trk->ctrlnum[c] == ctrl)
                trk->lctrlval[c] = val;
        }
    }
    pthread_mutex_unlock(&trk->exclctrl);

    midi_buff_excl_in(clt);
    if (trk->port >= 0 && trk->port < MIDI_CLIENT_MAX_PORTS
        && clt->curr_midi_queue_event[trk->port] < MIDI_EVT_BUFFER_LENGTH) {
        clt->midi_queue_buffer[trk->port][clt->curr_midi_queue_event[trk->port]++] = evt;
    }
    midi_buff_excl_out(clt);
}

void midi_in_buffer_add(midi_client *clt, midi_event evt) {
    midi_in_buff_excl_in(clt);
    if (clt->curr_midi_in_event == MIDI_EVT_BUFFER_LENGTH) {
        midi_in_buff_excl_out(clt);
        return;
    }
    clt->midi_in_buffer[clt->curr_midi_in_event++] = evt;
    midi_in_buff_excl_out(clt);
}

void midi_in_clear_events(midi_client *clt) {
    midi_in_buff_excl_in(clt);
    clt->curr_midi_in_event = 0;
    midi_in_buff_excl_out(clt);
}

void midi_ignore_buffer_clear(midi_client *clt) {
    midi_ignore_buff_excl_in(clt);
    clt->curr_midi_ignore_event = 0;
    midi_ignore_buff_excl_out(clt);
}

void midi_ignore_buffer_add(midi_client *clt, int channel, int type, int note) {
    midi_event evt;
    evt.channel = (unsigned char)channel;
    evt.type = type;
    evt.note = (unsigned char)note;
    evt.velocity = 0;
    evt.time = 0;

    midi_ignore_buff_excl_in(clt);
    if (clt->curr_midi_ignore_event < MIDI_EVT_BUFFER_LENGTH) {
        clt->midi_ignore_buffer[clt->curr_midi_ignore_event++] = evt;
    }
    midi_ignore_buff_excl_out(clt);
}

void midi_set_freewheel(midi_client *clt, int on) {
    if (clt) clt->freewheeling = on ? 1 : 0;
}

void midi_synch_output_ports(midi_client *clt) {
    (void)clt;
    /* No-op: ports managed by JUCE bus topology, not by engine. */
}

void midi_send_transp(midi_client *clt, int play, long frames) {
    (void)clt; (void)play; (void)frames;
    /* No-op: Element drives transport. */
}
