/* smf_stub.c - Element-NSPA tracker engine
 *
 * No-op stubs for vht's SMF (Standard MIDI File) export. Engine code in
 * module.c references smf_new / smf_free / smf_clear / smf_dump /
 * smf_set_pos; we provide them as no-ops so the engine links. When SMF
 * export is wanted later, replace via JUCE's juce::MidiFile.
 */

#include <stdlib.h>
#include "vht_types.h"

/* Opaque type — module.c only ever passes `smf *` around, never derefs. */
typedef struct smf_t {
    int placeholder;
} smf;

smf *smf_new(void *mod_ref) {
    (void)mod_ref;
    return calloc(1, sizeof(smf));
}

void smf_free(smf *mf)  { free(mf); }
void smf_clear(smf *mf) { (void)mf; }

int smf_dump(smf *mf, const char *phname) {
    (void)mf; (void)phname;
    return 0;
}

void smf_set_pos(smf *mf, jack_nframes_t frm) {
    (void)mf; (void)frm;
}
