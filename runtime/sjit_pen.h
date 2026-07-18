#ifndef SJIT_PEN_H
#define SJIT_PEN_H

#include "sjit_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

void sjit_pen_clear(SRuntime *runtime);
void sjit_pen_down(SRuntime *runtime, SSprite *sprite);
void sjit_pen_up(SRuntime *runtime, SSprite *sprite);
void sjit_pen_stamp(SRuntime *runtime, SSprite *sprite);
void sjit_pen_set_size(SRuntime *runtime, SSprite *sprite, SValue size);
void sjit_pen_set_color_rgb(SRuntime *runtime, SSprite *sprite, int r, int g, int b);
void sjit_pen_set_color_value(SRuntime *runtime, SSprite *sprite, SValue value);
void sjit_pen_set_color_value_and_change_param_number(
    SRuntime *runtime,
    SSprite *sprite,
    SValue value,
    int param_id,
    double delta);
void sjit_pen_set_number_color_and_change_brightness(
    SRuntime *runtime,
    SSprite *sprite,
    double color,
    double delta);
void sjit_pen_change_color_param(SRuntime *runtime, SSprite *sprite, const char *param, SValue delta);
int sjit_pen_color_param_id(const char *param);
void sjit_pen_change_color_param_number(SRuntime *runtime, SSprite *sprite, int param_id, double delta);
void sjit_pen_change_brightness(SRuntime *runtime, SSprite *sprite, SValue delta);
void sjit_pen_path_init(SPenPathBuffer *path);
void sjit_pen_path_destroy(SPenPathBuffer *path);
void sjit_pen_path_clear(SPenPathBuffer *path);
int sjit_pen_path_push(
    SPenPathBuffer *path,
    int target_id,
    double x,
    double y,
    double x2,
    double y2,
    double width,
    int r,
    int g,
    int b,
    int a);
int sjit_pen_path_append_draw(SDrawCommandBuffer *draw, const SPenPathBuffer *path);

#ifdef __cplusplus
}
#endif

#endif
