#include "sjit_draw.h"

#include "sjit_sprite.h"

#include <stdlib.h>
#include <string.h>

static int ensure_draw_capacity(SDrawCommandBuffer *buffer, int wanted) {
    if (!buffer) {
        return 0;
    }
    if (wanted <= buffer->capacity) {
        return 1;
    }
    int next = buffer->capacity > 0 ? buffer->capacity : SJIT_INITIAL_CAPACITY;
    while (next < wanted) {
        next *= 2;
    }
    SDrawCommand *items = (SDrawCommand *)realloc(buffer->items, sizeof(SDrawCommand) * (size_t)next);
    if (!items) {
        return 0;
    }
    buffer->items = items;
    buffer->capacity = next;
    return 1;
}

void sjit_draw_buffer_init(SDrawCommandBuffer *buffer) {
    if (!buffer) {
        return;
    }
    buffer->items = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

void sjit_draw_buffer_destroy(SDrawCommandBuffer *buffer) {
    if (!buffer) {
        return;
    }
    free(buffer->items);
    buffer->items = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
}

void sjit_draw_buffer_clear(SDrawCommandBuffer *buffer) {
    if (buffer) {
        buffer->length = 0;
    }
}

int sjit_draw_push(SDrawCommandBuffer *buffer, SDrawCommand command) {
    if (!buffer || !ensure_draw_capacity(buffer, buffer->length + 1)) {
        return 0;
    }
    buffer->items[buffer->length++] = command;
    return 1;
}

int sjit_draw_append(SDrawCommandBuffer *buffer, const SDrawCommandBuffer *source) {
    if (!buffer || !source) {
        return 0;
    }
    const int source_length = source->length;
    if (source_length <= 0) {
        return 1;
    }
    if (!ensure_draw_capacity(buffer, buffer->length + source_length)) {
        return 0;
    }
    memmove(
        buffer->items + buffer->length,
        source->items,
        sizeof(SDrawCommand) * (size_t)source_length);
    buffer->length += source_length;
    return 1;
}

int sjit_draw_push_sprite(SDrawCommandBuffer *buffer, const SSprite *sprite) {
    if (!sprite) {
        return 0;
    }
    SDrawCommand command;
    command.kind = SJIT_DRAW_SPRITE;
    command.target_id = sprite->base.id;
    command.drawable_id = sprite->drawable_id;
    command.costume_id = sprite->current_costume;
    command.x = sprite->x;
    command.y = sprite->y;
    command.x2 = sprite->x;
    command.y2 = sprite->y;
    command.direction = sprite->direction;
    command.size = sprite->size;
    command.pen_width = sprite->pen_size;
    command.r = 247;
    command.g = 154;
    command.b = 43;
    command.a = 255;
    command.visible = sprite->visible;
    command.layer = sprite->layer_order;
    return sjit_draw_push(buffer, command);
}

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
    int a) {
    SDrawCommand command;
    command.kind = SJIT_DRAW_PEN_STROKE;
    command.target_id = target_id;
    command.drawable_id = 0;
    command.costume_id = 0;
    command.x = x;
    command.y = y;
    command.x2 = x2;
    command.y2 = y2;
    command.direction = 0.0;
    command.size = 0.0;
    command.pen_width = width;
    command.r = r;
    command.g = g;
    command.b = b;
    command.a = a;
    command.visible = 1;
    command.layer = 0;
    return sjit_draw_push(buffer, command);
}
