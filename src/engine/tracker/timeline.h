/* timeline.h - Element-NSPA tracker engine stub
 *
 * Stub for vht's timeline (song-arrangement layer). Struct layout
 * preserved verbatim from upstream so the engine's field accesses
 * compile; all functions are no-ops returning sensible defaults. Wire
 * the real implementation when song-arrangement / Phase 5+ work lands.
 *
 * Original timeline: Copyright (C) 2024 Remigiusz Dybka, GPL-3.0+
 * Stub: Element-NSPA, GPL-3.0+
 */

#ifndef TIMELINE_H_STUB
#define TIMELINE_H_STUB

#include <pthread.h>
#include "vht_types.h"
#include "sequence.h"

typedef struct timeslice_t {
    float bpm;
    double length;
    double time;
} timeslice;

typedef struct timechange_t {
    float bpm;
    int linked;
    long row;
    int tag;
} timechange;

typedef struct timestrip_t {
    sequence *seq;
    int col;
    long start;
    int length;
    int rpb_start;
    int rpb_end;
    int tag;
    int enabled;
} timestrip;

typedef struct timeline_t {
    timeslice *slices;
    timechange *changes;
    timestrip *strips;
    double *ticks;
    long nslices;
    int nchanges;
    int nstrips;
    int nticks;
    int ncols;
    long loop_start;
    long loop_end;
    int loop_active;
    int length;
    double time_length;
    double pos;

    pthread_mutex_t excl;
    struct midi_client_t *clt;
} timeline;

timeline *timeline_new(struct midi_client_t *clt);
void timeline_free(timeline *tl);
void timeline_clear(timeline *tl);
void timeline_reset(timeline *tl);
void timeline_update(timeline *tl);
void timeline_advance(timeline *tl, double period, jack_nframes_t nframes);
double timeline_get_qb_time(timeline *tl, double row);
void timeline_swap_sequence(timeline *tl, int s1, int s2);
void timeline_set_pos(timeline *tl, double qb, int sync);
void timeline_delete_all_strips(timeline *tl, int col);

timechange *timeline_get_change(timeline *tl, int id);
void timechange_set_bpm(timeline *tl, timechange *tc, float bpm);

void timeline_update_loops_in_strips(timeline *tl);

#endif /* TIMELINE_H_STUB */
