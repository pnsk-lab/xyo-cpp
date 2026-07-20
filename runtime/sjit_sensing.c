#include "sjit_sensing.h"

#include "sjit_number.h"
#include "sjit_string.h"
#include "sjit_value.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static void sprite_half_size(const SSprite *sprite, double *half_width, double *half_height);

static int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static int parse_color(SRuntime *runtime, SValue value, int *r, int *g, int *b) {
    if (!runtime || !r || !g || !b) {
        return 0;
    }
    SValue text_value = sjit_to_string(runtime, value);
    const char *text = sjit_string_cstr((const SString *)text_value.ptr);
    if (text[0] == '#') {
        ++text;
    }
    int result = 0;
    if (strlen(text) == 6) {
        const int r0 = hex_digit(text[0]);
        const int r1 = hex_digit(text[1]);
        const int g0 = hex_digit(text[2]);
        const int g1 = hex_digit(text[3]);
        const int b0 = hex_digit(text[4]);
        const int b1 = hex_digit(text[5]);
        if (r0 >= 0 && r1 >= 0 && g0 >= 0 && g1 >= 0 && b0 >= 0 && b1 >= 0) {
            *r = r0 * 16 + r1;
            *g = g0 * 16 + g1;
            *b = b0 * 16 + b1;
            result = 1;
        }
    } else if (strlen(text) == 3) {
        const int r0 = hex_digit(text[0]);
        const int g0 = hex_digit(text[1]);
        const int b0 = hex_digit(text[2]);
        if (r0 >= 0 && g0 >= 0 && b0 >= 0) {
            *r = r0 * 17;
            *g = g0 * 17;
            *b = b0 * 17;
            result = 1;
        }
    }
    sjit_value_destroy_fast(text_value);
    return result;
}

static int sampled_color(
    SRuntime *runtime,
    int sample_target_id,
    int subject_target_id,
    double x,
    double y,
    int *r,
    int *g,
    int *b,
    int *a) {
    if (!runtime || !runtime->sensing_color_sampler) {
        return 0;
    }
    return runtime->sensing_color_sampler(
        runtime->sensing_color_sampler_context,
        runtime,
        sample_target_id,
        subject_target_id,
        x,
        y,
        r,
        g,
        b,
        a);
}

static int colors_match(int r0, int g0, int b0, int r1, int g1, int b1) {
    /* A small tolerance accounts for anti-aliased edges while preserving the
       exact result for normal opaque costume pixels. */
    return abs(r0 - r1) <= 8 && abs(g0 - g1) <= 8 && abs(b0 - b1) <= 8;
}

static int sample_owner_color(
    SRuntime *runtime,
    const SSprite *sprite,
    double x,
    double y,
    int *r,
    int *g,
    int *b) {
    int a = 0;
    return sprite && sampled_color(runtime, sprite->base.id, 0, x, y, r, g, b, &a) && a > 0;
}

static int sample_owner_bounds(const SSprite *sprite, double *left, double *right, double *bottom, double *top) {
    if (!sprite || !left || !right || !bottom || !top) {
        return 0;
    }
    double half_width = 0.0;
    double half_height = 0.0;
    sprite_half_size(sprite, &half_width, &half_height);
    if (!(half_width > 0.0) || !(half_height > 0.0)) {
        return 0;
    }
    *left = sprite->x - half_width;
    *right = sprite->x + half_width;
    *bottom = sprite->y - half_height;
    *top = sprite->y + half_height;
    return 1;
}

static int iterate_color_overlap(
    SRuntime *runtime,
    SSprite *sprite,
    int first_r,
    int first_g,
    int first_b,
    int second_r,
    int second_g,
    int second_b,
    int require_second_color) {
    double left = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double top = 0.0;
    if (!runtime || !sprite || !runtime->sensing_color_sampler ||
        !sample_owner_bounds(sprite, &left, &right, &bottom, &top)) {
        return 0;
    }
    const int start_x = (int)floor(left);
    const int end_x = (int)ceil(right);
    const int start_y = (int)floor(bottom);
    const int end_y = (int)ceil(top);
    int samples = 0;
    for (int yi = start_y; yi <= end_y && samples < 750000; ++yi) {
        for (int xi = start_x; xi <= end_x && samples < 750000; ++xi) {
            ++samples;
            const double x = (double)xi + 0.5;
            const double y = (double)yi + 0.5;
            int owner_r = 0;
            int owner_g = 0;
            int owner_b = 0;
            if (!sample_owner_color(runtime, sprite, x, y, &owner_r, &owner_g, &owner_b) ||
                !colors_match(owner_r, owner_g, owner_b, first_r, first_g, first_b)) {
                continue;
            }
            int scene_r = 0;
            int scene_g = 0;
            int scene_b = 0;
            int scene_a = 0;
            const int has_scene = sampled_color(
                runtime,
                0,
                sprite->base.id,
                x,
                y,
                &scene_r,
                &scene_g,
                &scene_b,
                &scene_a) && scene_a > 0;
            if (!require_second_color) {
                if (has_scene && colors_match(scene_r, scene_g, scene_b, first_r, first_g, first_b)) {
                    return 1;
                }
            } else if (has_scene && colors_match(scene_r, scene_g, scene_b, second_r, second_g, second_b)) {
                return 1;
            }
        }
    }
    return 0;
}

static SSprite *target_for_value(SRuntime *runtime, SValue value) {
    if (!runtime) {
        return NULL;
    }
    SValue text = sjit_to_string(runtime, value);
    const char *name = sjit_string_cstr((const SString *)text.ptr);
    SSprite *target = NULL;
    if (text_equal(name, "_mouse_") || text_equal(name, "mouse pointer")) {
        target = NULL;
    } else if (text_equal(name, "_stage_") || text_equal(name, "stage")) {
        for (int i = 0; i < runtime->target_count; ++i) {
            if (runtime->targets[i] && runtime->targets[i]->base.is_stage) {
                target = runtime->targets[i];
                break;
            }
        }
    } else {
        target = sjit_runtime_get_sprite_by_name(runtime, name);
    }
    sjit_value_destroy_fast(text);
    return target;
}

static void sprite_half_size(const SSprite *sprite, double *half_width, double *half_height) {
    double width = sjit_sprite_current_costume_width(sprite);
    double height = sjit_sprite_current_costume_height(sprite);
    if (width <= 0.0) {
        width = 20.0;
    }
    if (height <= 0.0) {
        height = 20.0;
    }
    const double scale = sprite ? fmax(0.0, sprite->size) / 100.0 : 1.0;
    if (half_width) {
        *half_width = width * scale * 0.5;
    }
    if (half_height) {
        *half_height = height * scale * 0.5;
    }
}

static int boxes_overlap(const SSprite *left, const SSprite *right) {
    if (!left || !right || !left->visible || !right->visible) {
        return 0;
    }
    double left_w = 0.0;
    double left_h = 0.0;
    double right_w = 0.0;
    double right_h = 0.0;
    sprite_half_size(left, &left_w, &left_h);
    sprite_half_size(right, &right_w, &right_h);
    return fabs(left->x - right->x) <= left_w + right_w &&
        fabs(left->y - right->y) <= left_h + right_h;
}

int sjit_sensing_touching_object(SRuntime *runtime, SSprite *sprite, SValue object) {
    if (!runtime || !sprite) {
        return 0;
    }
    SValue text = sjit_to_string(runtime, object);
    const char *name = sjit_string_cstr((const SString *)text.ptr);
    int touching = 0;
    if (text_equal(name, "_mouse_") || text_equal(name, "mouse pointer")) {
        double half_width = 0.0;
        double half_height = 0.0;
        sprite_half_size(sprite, &half_width, &half_height);
        touching = sprite->visible &&
            runtime->input.mouse_x >= sprite->x - half_width &&
            runtime->input.mouse_x <= sprite->x + half_width &&
            runtime->input.mouse_y >= sprite->y - half_height &&
            runtime->input.mouse_y <= sprite->y + half_height;
    } else if (text_equal(name, "_edge_") || text_equal(name, "edge")) {
        double half_width = 0.0;
        double half_height = 0.0;
        sprite_half_size(sprite, &half_width, &half_height);
        touching = sprite->visible && (
            sprite->x - half_width <= -240.0 ||
            sprite->x + half_width >= 240.0 ||
            sprite->y - half_height <= -180.0 ||
            sprite->y + half_height >= 180.0);
    } else {
        SSprite *other = target_for_value(runtime, object);
        touching = other && other != sprite && boxes_overlap(sprite, other);
    }
    sjit_value_destroy_fast(text);
    return touching;
}

int sjit_sensing_touching_color(SRuntime *runtime, SSprite *sprite, SValue color) {
    int r = 0;
    int g = 0;
    int b = 0;
    if (!parse_color(runtime, color, &r, &g, &b)) {
        return 0;
    }
    return iterate_color_overlap(runtime, sprite, r, g, b, 0, 0, 0, 0);
}

int sjit_sensing_color_touching_color(
    SRuntime *runtime,
    SSprite *sprite,
    SValue color,
    SValue color2) {
    int first_r = 0;
    int first_g = 0;
    int first_b = 0;
    int second_r = 0;
    int second_g = 0;
    int second_b = 0;
    if (!parse_color(runtime, color, &first_r, &first_g, &first_b) ||
        !parse_color(runtime, color2, &second_r, &second_g, &second_b)) {
        return 0;
    }
    return iterate_color_overlap(
        runtime,
        sprite,
        first_r,
        first_g,
        first_b,
        second_r,
        second_g,
        second_b,
        1);
}

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
        int *a)) {
    if (!runtime) {
        return;
    }
    runtime->sensing_color_sampler_context = context;
    runtime->sensing_color_sampler = sampler;
}

double sjit_sensing_distance_to(SRuntime *runtime, SSprite *sprite, SValue target) {
    if (!runtime || !sprite) {
        return 10000.0;
    }
    if (sprite->base.is_stage) {
        return 10000.0;
    }
    SValue text = sjit_to_string(runtime, target);
    const char *name = sjit_string_cstr((const SString *)text.ptr);
    double x = runtime->input.mouse_x;
    double y = runtime->input.mouse_y;
    if (text_equal(name, "_random_") || text_equal(name, "random position")) {
        x = (double)(rand() % 481) - 240.0;
        y = (double)(rand() % 361) - 180.0;
    } else if (!text_equal(name, "_mouse_") && !text_equal(name, "mouse pointer")) {
        SSprite *other = target_for_value(runtime, target);
        if (!other) {
            sjit_value_destroy_fast(text);
            return 10000.0;
        }
        x = other->x;
        y = other->y;
    }
    sjit_value_destroy_fast(text);
    return hypot(x - sprite->x, y - sprite->y);
}

SValue sjit_sensing_attribute_of(
    SRuntime *runtime,
    SSprite *current,
    SValue attribute,
    SValue object) {
    if (!runtime) {
        return sjit_make_string("");
    }
    SValue attr_text = sjit_to_string(runtime, attribute);
    SValue object_text = sjit_to_string(runtime, object);
    const char *attr = sjit_string_cstr((const SString *)attr_text.ptr);
    const char *object_name = sjit_string_cstr((const SString *)object_text.ptr);
    SSprite *target = text_equal(object_name, "_myself_") ? current :
        target_for_value(runtime, object);
    SValue result = sjit_make_string("");
    if (target) {
        if (target->base.is_stage &&
            (text_equal(attr, "backdrop #") || text_equal(attr, "background #"))) {
            result = sjit_make_number(target->costume_count > 0 ? target->current_costume + 1.0 : 0.0);
        } else if (target->base.is_stage && text_equal(attr, "backdrop name")) {
            result = sjit_make_string(sjit_sprite_current_costume_name(target));
        } else if (text_equal(attr, "x position")) {
            result = sjit_make_number(target->x);
        } else if (text_equal(attr, "y position")) {
            result = sjit_make_number(target->y);
        } else if (text_equal(attr, "direction")) {
            result = sjit_make_number(target->direction);
        } else if (text_equal(attr, "costume #")) {
            result = sjit_make_number(target->costume_count > 0 ? target->current_costume + 1.0 : 0.0);
        } else if (text_equal(attr, "costume name")) {
            result = sjit_make_string(sjit_sprite_current_costume_name(target));
        } else if (text_equal(attr, "size")) {
            result = sjit_make_number(target->size);
        } else if (text_equal(attr, "volume")) {
            result = sjit_make_number(target->volume);
        } else if (text_equal(attr, "layer #")) {
            result = sjit_make_number(target->layer_order);
        } else {
            SVariable *variable = sjit_target_lookup_variable(
                &target->base, 0, attr, SJIT_VAR_SCALAR);
            if (variable) {
                result = sjit_value_clone(variable->value);
            }
        }
    }
    sjit_value_destroy_fast(attr_text);
    sjit_value_destroy_fast(object_text);
    return result;
}

SValue sjit_sensing_current(const char *menu) {
    time_t now = time(NULL);
    struct tm local_time;
    if (!localtime_r(&now, &local_time)) {
        return sjit_make_number(0.0);
    }
    if (text_equal(menu, "YEAR")) return sjit_make_number(local_time.tm_year + 1900.0);
    if (text_equal(menu, "MONTH")) return sjit_make_number(local_time.tm_mon + 1.0);
    if (text_equal(menu, "DATE")) return sjit_make_number(local_time.tm_mday);
    if (text_equal(menu, "DAYOFWEEK")) return sjit_make_number(local_time.tm_wday + 1.0);
    if (text_equal(menu, "HOUR")) return sjit_make_number(local_time.tm_hour);
    if (text_equal(menu, "MINUTE")) return sjit_make_number(local_time.tm_min);
    if (text_equal(menu, "SECOND")) return sjit_make_number(local_time.tm_sec);
    return sjit_make_number(0.0);
}
