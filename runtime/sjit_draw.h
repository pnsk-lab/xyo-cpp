#ifndef SJIT_DRAW_H
#define SJIT_DRAW_H

#include "sjit_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

void sjit_draw_buffer_init(SDrawCommandBuffer *buffer);
void sjit_draw_buffer_destroy(SDrawCommandBuffer *buffer);
void sjit_draw_buffer_clear(SDrawCommandBuffer *buffer);
int sjit_draw_push(SDrawCommandBuffer *buffer, SDrawCommand command);
int sjit_draw_append(SDrawCommandBuffer *buffer, const SDrawCommandBuffer *source);
int sjit_draw_push_sprite(SDrawCommandBuffer *buffer, const SSprite *sprite);
int sjit_draw_push_pen_stroke(
    SDrawCommandBuffer *buffer,
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

#ifdef __cplusplus
}
#endif

#endif
