/* mandy.c - Element-NSPA tracker engine stub
 *
 * See mandy.h. No-op implementations.
 */

#include <stdlib.h>
#include "mandy.h"

mandy *mandy_new(void *trk) {
    mandy *m = calloc(1, sizeof(mandy));
    if (m) m->trk = trk;
    return m;
}

void mandy_free(mandy *mand) {
    free(mand);
}

mandy *mandy_clone(mandy *src, void *trk) {
    (void)src;
    return mandy_new(trk);
}

void mandy_reset(mandy *mand) {
    (void)mand;
}

void mandy_advance(mandy *mand, double tperiod, jack_nframes_t nframes) {
    (void)mand;
    (void)tperiod;
    (void)nframes;
}
