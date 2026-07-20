#ifndef SJIT_LOOKS_H
#define SJIT_LOOKS_H

#include "sjit_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

void sjit_looks_show(SRuntime *runtime, SSprite *sprite);
void sjit_looks_hide(SRuntime *runtime, SSprite *sprite);
void sjit_looks_say(SRuntime *runtime, SSprite *sprite, SValue message, int thought);
void sjit_looks_next_costume(SRuntime *runtime, SSprite *sprite);
void sjit_looks_next_backdrop(SRuntime *runtime);
void sjit_looks_change_size(SRuntime *runtime, SSprite *sprite, double change);
void sjit_looks_change_stretch(SRuntime *runtime, SSprite *sprite, double change);
void sjit_looks_set_stretch(SRuntime *runtime, SSprite *sprite, double value);
void sjit_looks_go_forward_backward_layers(SRuntime *runtime, SSprite *sprite, int layers);
void sjit_looks_hide_all_sprites(SRuntime *runtime);
SValue sjit_looks_backdrop_number_name(SRuntime *runtime, int number_name);
void sjit_looks_switch_backdrop_and_wait(
    SRuntime *runtime,
    SFrame *frame,
    SValue requested_backdrop,
    int resume_pc,
    SRuntimeStatus *status);
void sjit_looks_switch_costume(SRuntime *runtime, SSprite *sprite, SValue requested_costume);
void sjit_looks_go_to_front_back(SRuntime *runtime, SSprite *sprite, int front);
void sjit_looks_switch_backdrop(SRuntime *runtime, SValue requested_backdrop);
void sjit_looks_switch_backdrop_value_ptr(SRuntime *runtime, SValue *requested_backdrop);
int sjit_looks_effect_from_name(const char *effect_name);
void sjit_looks_set_effect_number(
    SRuntime *runtime,
    SSprite *sprite,
    int effect,
    double value);
void sjit_looks_change_effect_number(
    SRuntime *runtime,
    SSprite *sprite,
    int effect,
    double change);
void sjit_looks_clear_effects(SRuntime *runtime, SSprite *sprite);

#ifdef __cplusplus
}
#endif

#endif
