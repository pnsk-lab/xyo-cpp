#ifndef SJIT_LOOKS_H
#define SJIT_LOOKS_H

#include "sjit_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

void sjit_looks_show(SRuntime *runtime, SSprite *sprite);
void sjit_looks_hide(SRuntime *runtime, SSprite *sprite);
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
