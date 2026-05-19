/* timeline.c - Element-NSPA tracker engine stub. See timeline.h. */

#include <stdlib.h>
#include "timeline.h"

timeline *timeline_new(struct midi_client_t *clt) {
    timeline *tl = calloc(1, sizeof(timeline));
    if (!tl) return NULL;
    tl->clt = clt;
    pthread_mutex_init(&tl->excl, NULL);
    /* Pre-allocate one default timechange so timeline_get_change(tl, 0)
     * never returns NULL — module_new() writes to it during init. */
    tl->changes = calloc(1, sizeof(timechange));
    tl->nchanges = tl->changes ? 1 : 0;
    return tl;
}

void timeline_free(timeline *tl) {
    if (!tl) return;
    pthread_mutex_destroy(&tl->excl);
    free(tl->strips);
    free(tl->slices);
    free(tl->changes);
    free(tl->ticks);
    free(tl);
}

void timeline_clear(timeline *tl)   { (void)tl; }
void timeline_reset(timeline *tl)   { (void)tl; }
void timeline_update(timeline *tl)  { (void)tl; }

void timeline_advance(timeline *tl, double period, jack_nframes_t nframes) {
    (void)tl; (void)period; (void)nframes;
}

double timeline_get_qb_time(timeline *tl, double row) {
    (void)tl; (void)row;
    return 0.0;
}

void timeline_swap_sequence(timeline *tl, int s1, int s2) {
    (void)tl; (void)s1; (void)s2;
}

void timeline_set_pos(timeline *tl, double qb, int sync) {
    (void)sync;
    if (tl) tl->pos = qb;
}

void timeline_delete_all_strips(timeline *tl, int col) {
    (void)tl; (void)col;
}

timechange *timeline_get_change(timeline *tl, int id) {
    if (!tl || !tl->changes || id < 0 || id >= tl->nchanges) return NULL;
    return &tl->changes[id];
}

void timechange_set_bpm(timeline *tl, timechange *tc, float bpm) {
    (void)tl; (void)bpm;
    if (tc) tc->bpm = bpm;
}

void timeline_update_loops_in_strips(timeline *tl) {
    (void)tl;
}
