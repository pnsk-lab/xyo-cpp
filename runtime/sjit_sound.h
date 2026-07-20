#ifndef SJIT_SOUND_H
#define SJIT_SOUND_H

#include "sjit_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

void sjit_sound_play(SRuntime *runtime, SSprite *sprite, SValue sound, int wait);
void sjit_sound_stop_all(SRuntime *runtime);
void sjit_sound_set_effect(SRuntime *runtime, SSprite *sprite, SValue effect, double value, int change);
void sjit_sound_clear_effects(SRuntime *runtime, SSprite *sprite);
void sjit_sound_set_volume(SRuntime *runtime, SSprite *sprite, double value, int change);
double sjit_sound_volume(const SSprite *sprite);

#ifdef __cplusplus
}
#endif

#endif
