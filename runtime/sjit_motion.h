#ifndef SJIT_MOTION_H
#define SJIT_MOTION_H

#include "sjit_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

void sjit_motion_goto_xy(SRuntime *runtime, SSprite *sprite, SValue x, SValue y);
void sjit_motion_point_in_direction(SRuntime *runtime, SSprite *sprite, SValue direction);

#ifdef __cplusplus
}
#endif

#endif

