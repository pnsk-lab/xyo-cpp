#ifndef SJIT_MOTION_H
#define SJIT_MOTION_H

#include "sjit_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

void sjit_motion_goto_xy(SRuntime *runtime, SSprite *sprite, SValue x, SValue y);
void sjit_motion_point_in_direction(SRuntime *runtime, SSprite *sprite, SValue direction);
void sjit_motion_move_steps(SRuntime *runtime, SSprite *sprite, SValue steps);
void sjit_motion_turn(SRuntime *runtime, SSprite *sprite, SValue degrees, int right);
void sjit_motion_point_towards(SRuntime *runtime, SSprite *sprite, SValue target);
void sjit_motion_goto(SRuntime *runtime, SSprite *sprite, SValue target);
SRuntimeStatus sjit_motion_glide_to_xy(
    SRuntime *runtime,
    SSprite *sprite,
    SFrame *frame,
    const void *statement_key,
    double duration_seconds,
    double x,
    double y,
    int resume_pc);
SRuntimeStatus sjit_motion_glide_to(
    SRuntime *runtime,
    SSprite *sprite,
    SFrame *frame,
    const void *statement_key,
    double duration_seconds,
    SValue target,
    int resume_pc);
void sjit_motion_if_on_edge_bounce(SRuntime *runtime, SSprite *sprite);
void sjit_motion_set_rotation_style(SSprite *sprite, const char *style);

#ifdef __cplusplus
}
#endif

#endif
