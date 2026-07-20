#include "sjit_motion.h"

#include "sjit_number.h"
#include "sjit_string.h"
#include "sjit_value.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>

static const double SJIT_PI = 3.14159265358979323846;

static int text_equal(const char *left, const char *right) {
    if (!left || !right) {
        return 0;
    }
    while (*left && *right) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return 0;
        }
        ++left;
        ++right;
    }
    return *left == '\0' && *right == '\0';
}

void sjit_motion_goto_xy(SRuntime *runtime, SSprite *sprite, SValue x, SValue y) {
    sjit_sprite_set_xy(runtime, sprite, sjit_to_number_fast(runtime, x), sjit_to_number_fast(runtime, y), 0);
}

void sjit_motion_point_in_direction(SRuntime *runtime, SSprite *sprite, SValue direction) {
    sjit_sprite_set_direction(runtime, sprite, sjit_to_number_fast(runtime, direction));
}

void sjit_motion_move_steps(SRuntime *runtime, SSprite *sprite, SValue steps) {
    if (!sprite) {
        return;
    }
    const double distance = sjit_to_number_fast(runtime, steps);
    const double radians = sprite->direction * SJIT_PI / 180.0;
    sjit_sprite_set_xy(
        runtime,
        sprite,
        sprite->x + distance * sin(radians),
        sprite->y + distance * cos(radians),
        0);
}

void sjit_motion_turn(SRuntime *runtime, SSprite *sprite, SValue degrees, int right) {
    if (!sprite) {
        return;
    }
    const double amount = sjit_to_number_fast(runtime, degrees);
    sjit_sprite_set_direction(
        runtime,
        sprite,
        sprite->direction + (right ? amount : -amount));
}

static SSprite *motion_target(SRuntime *runtime, SValue target) {
    if (!runtime) {
        return NULL;
    }
    SValue text = sjit_to_string(runtime, target);
    const char *name = sjit_string_cstr((const SString *)text.ptr);
    SSprite *result = NULL;
    if (!text_equal(name, "_mouse_") && !text_equal(name, "mouse pointer") &&
        !text_equal(name, "_random_") && !text_equal(name, "random position")) {
        result = sjit_runtime_get_sprite_by_name(runtime, name);
    }
    sjit_value_destroy_fast(text);
    return result;
}

void sjit_motion_point_towards(SRuntime *runtime, SSprite *sprite, SValue target) {
    if (!runtime || !sprite) {
        return;
    }
    SValue text = sjit_to_string(runtime, target);
    const char *name = sjit_string_cstr((const SString *)text.ptr);
    double x = 0.0;
    double y = 0.0;
    if (text_equal(name, "_mouse_") || text_equal(name, "mouse pointer")) {
        x = runtime->input.mouse_x;
        y = runtime->input.mouse_y;
    } else if (text_equal(name, "_random_") || text_equal(name, "random position")) {
        sjit_sprite_set_direction(runtime, sprite, (double)(rand() % 361) - 180.0);
        sjit_value_destroy_fast(text);
        return;
    } else {
        SSprite *other = motion_target(runtime, target);
        if (!other) {
            sjit_value_destroy_fast(text);
            return;
        }
        x = other->x;
        y = other->y;
    }
    sjit_value_destroy_fast(text);
    if (x == sprite->x && y == sprite->y) {
        return;
    }
    sjit_sprite_set_direction(
        runtime,
        sprite,
        atan2(x - sprite->x, y - sprite->y) * 180.0 / SJIT_PI);
}

void sjit_motion_goto(SRuntime *runtime, SSprite *sprite, SValue target) {
    if (!runtime || !sprite) {
        return;
    }
    SValue text = sjit_to_string(runtime, target);
    const char *name = sjit_string_cstr((const SString *)text.ptr);
    double x = sprite->x;
    double y = sprite->y;
    if (text_equal(name, "_mouse_") || text_equal(name, "mouse pointer")) {
        x = runtime->input.mouse_x;
        y = runtime->input.mouse_y;
    } else if (text_equal(name, "_random_") || text_equal(name, "random position")) {
        x = (double)(rand() % 481) - 240.0;
        y = (double)(rand() % 361) - 180.0;
    } else {
        SSprite *other = motion_target(runtime, target);
        if (!other) {
            sjit_value_destroy_fast(text);
            return;
        }
        x = other->x;
        y = other->y;
    }
    sjit_value_destroy_fast(text);
    sjit_sprite_set_xy(runtime, sprite, x, y, 0);
}

SRuntimeStatus sjit_motion_glide_to_xy(
    SRuntime *runtime,
    SSprite *sprite,
    SFrame *frame,
    const void *statement_key,
    double duration_seconds,
    double x,
    double y,
    int resume_pc) {
    if (!runtime || !sprite) {
        return SJIT_STATUS_OK;
    }
    if (!frame) {
        sjit_sprite_set_xy(runtime, sprite, x, y, 0);
        return SJIT_STATUS_OK;
    }
    if (!frame->glide_active || frame->timed_statement != statement_key) {
        frame->timed_statement = statement_key;
        frame->timed_event_kind = 1;
        frame->glide_active = 1;
        frame->glide_start_x = sprite->x;
        frame->glide_start_y = sprite->y;
        frame->glide_end_x = x;
        frame->glide_end_y = y;
        const double duration_ms = fmax(0.0, duration_seconds) * 1000.0;
        frame->glide_finish_ms = runtime->now_ms + duration_ms;
        if (duration_ms <= 0.0) {
            sjit_sprite_set_xy(runtime, sprite, x, y, 0);
            frame->glide_active = 0;
            frame->timed_statement = NULL;
            return SJIT_STATUS_OK;
        }
    }
    const double duration_ms = fmax(0.0, duration_seconds) * 1000.0;
    const double elapsed = duration_ms - (frame->glide_finish_ms - runtime->now_ms);
    if (runtime->now_ms < frame->glide_finish_ms) {
        const double fraction = duration_ms > 0.0 ?
            fmin(1.0, fmax(0.0, elapsed / duration_ms)) : 1.0;
        sjit_sprite_set_xy(
            runtime,
            sprite,
            frame->glide_start_x + (frame->glide_end_x - frame->glide_start_x) * fraction,
            frame->glide_start_y + (frame->glide_end_y - frame->glide_start_y) * fraction,
            0);
        frame->pc = resume_pc;
        return SJIT_STATUS_YIELDED;
    }
    sjit_sprite_set_xy(runtime, sprite, frame->glide_end_x, frame->glide_end_y, 0);
    frame->glide_active = 0;
    frame->timed_statement = NULL;
    return SJIT_STATUS_OK;
}

SRuntimeStatus sjit_motion_glide_to(
    SRuntime *runtime,
    SSprite *sprite,
    SFrame *frame,
    const void *statement_key,
    double duration_seconds,
    SValue target,
    int resume_pc) {
    if (!runtime || !sprite) {
        return SJIT_STATUS_OK;
    }
    SValue text = sjit_to_string(runtime, target);
    const char *name = sjit_string_cstr((const SString *)text.ptr);
    double x = sprite->x;
    double y = sprite->y;
    if (text_equal(name, "_mouse_") || text_equal(name, "mouse pointer")) {
        x = runtime->input.mouse_x;
        y = runtime->input.mouse_y;
    } else if (text_equal(name, "_random_") || text_equal(name, "random position")) {
        x = (double)(rand() % 481) - 240.0;
        y = (double)(rand() % 361) - 180.0;
    } else {
        SSprite *other = motion_target(runtime, target);
        if (other) {
            x = other->x;
            y = other->y;
        }
    }
    sjit_value_destroy_fast(text);
    return sjit_motion_glide_to_xy(
        runtime,
        sprite,
        frame,
        statement_key,
        duration_seconds,
        x,
        y,
        resume_pc);
}

void sjit_motion_if_on_edge_bounce(SRuntime *runtime, SSprite *sprite) {
    if (!runtime || !sprite || sprite->base.is_stage) {
        return;
    }
    double width = sjit_sprite_current_costume_width(sprite);
    double height = sjit_sprite_current_costume_height(sprite);
    if (width <= 0.0) width = 20.0;
    if (height <= 0.0) height = 20.0;
    const double scale = fmax(0.0, sprite->size) / 100.0;
    const double half_width = width * scale * 0.5;
    const double half_height = height * scale * 0.5;
    double x = sprite->x;
    double y = sprite->y;
    int hit_x = 0;
    int hit_y = 0;
    if (x - half_width <= -240.0) { x = -240.0 + half_width; hit_x = 1; }
    if (x + half_width >= 240.0) { x = 240.0 - half_width; hit_x = 1; }
    if (y - half_height <= -180.0) { y = -180.0 + half_height; hit_y = 1; }
    if (y + half_height >= 180.0) { y = 180.0 - half_height; hit_y = 1; }
    if (!hit_x && !hit_y) {
        return;
    }
    const double radians = sprite->direction * SJIT_PI / 180.0;
    double vx = sin(radians);
    double vy = cos(radians);
    if (hit_x) vx = -vx;
    if (hit_y) vy = -vy;
    sjit_sprite_set_xy(runtime, sprite, x, y, 0);
    sjit_sprite_set_direction(runtime, sprite, atan2(vx, vy) * 180.0 / SJIT_PI);
}

void sjit_motion_set_rotation_style(SSprite *sprite, const char *style) {
    if (!sprite || !style) {
        return;
    }
    if (text_equal(style, "left-right")) {
        sprite->rotation_style = 1;
    } else if (text_equal(style, "don't rotate")) {
        sprite->rotation_style = 2;
    } else {
        sprite->rotation_style = 0;
    }
}
