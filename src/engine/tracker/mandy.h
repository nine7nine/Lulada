/* mandy.h - Element-NSPA tracker engine stub
 *
 * Stub for vht's "mandy" (fractal turtle visualiser) module. The original
 * mandy is a sizable feature tied to Python (PyObject) and Cairo
 * rendering; we don't ship it. Keep struct fields the engine references
 * (mand->active, tracies array) so the engine compiles unchanged. All
 * stub functions are no-ops; mand->active is always 0 so guarded code
 * paths never run.
 *
 * Original mandy: Copyright (C) 2024 Remigiusz Dybka, GPL-3.0+
 * Stub: Element-NSPA, GPL-3.0+
 */

#ifndef MANDY_H_STUB
#define MANDY_H_STUB

#include "vht_types.h"

typedef struct tracy_t {
    int qnt;
} tracy;

typedef struct mandy_t {
    int active;
    unsigned int ntracies;
    tracy **tracies;
    void *trk;
} mandy;

mandy *mandy_new(void *trk);
void mandy_free(mandy *mand);
mandy *mandy_clone(mandy *src, void *trk);
void mandy_reset(mandy *mand);
void mandy_advance(mandy *mand, double tperiod, jack_nframes_t nframes);

#endif /* MANDY_H_STUB */
