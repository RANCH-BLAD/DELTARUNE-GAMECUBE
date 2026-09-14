#include "gcn_audio_system.h"

#include "../noop_audio_system.h"

#include <stdlib.h>

// v1: silent audio. The noop audio system provides the full vtable as no-ops.
// v2 (later): libasnd voices + stb_vorbis streaming for BGM, DSP sfx in ARAM.

AudioSystem* GCNAudioSystem_create(void) {
    return (AudioSystem*) NoopAudioSystem_create();
}

void GCNAudioSystem_destroy(AudioSystem* audio) {
    if (audio == NULL) return;
    audio->vtable->destroy(audio);
}
