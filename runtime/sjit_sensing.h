#ifndef SJIT_SENSING_H
#define SJIT_SENSING_H

#include "sjit_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

int sjit_sensing_touching_object(SRuntime *runtime, SSprite *sprite, SValue object);
int sjit_sensing_touching_color(SRuntime *runtime, SSprite *sprite, SValue color);
int sjit_sensing_color_touching_color(
    SRuntime *runtime,
    SSprite *sprite,
    SValue color,
    SValue color2);
void sjit_sensing_set_color_sampler(
    SRuntime *runtime,
    void *context,
    int (*sampler)(
        void *context,
        const SRuntime *runtime,
        int sample_target_id,
        int subject_target_id,
        double x,
        double y,
        int *r,
        int *g,
        int *b,
        int *a));
double sjit_sensing_distance_to(SRuntime *runtime, SSprite *sprite, SValue target);
SValue sjit_sensing_attribute_of(
    SRuntime *runtime,
    SSprite *current,
    SValue attribute,
    SValue object);
SValue sjit_sensing_current(const char *menu);

#ifdef __cplusplus
}
#endif

#endif
