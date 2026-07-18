#include "sjit_sprite.h"

#include "sjit_pen.h"
#include "sjit_runtime.h"
#include "sjit_string.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static double wrap_direction(double direction) {
    if (!isfinite(direction)) {
        return 90.0;
    }
    double wrapped = fmod(direction + 180.0, 360.0);
    if (wrapped < 0.0) {
        wrapped += 360.0;
    }
    wrapped -= 180.0;
    if (wrapped <= -180.0) {
        wrapped += 360.0;
    }
    return wrapped;
}

static void destroy_costume_names(SString **names, int count) {
    if (!names) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        sjit_string_destroy(names[i]);
    }
    free(names);
}

static int wrap_costume_index(int index, int count) {
    if (count <= 0) {
        return index < 0 ? 0 : index;
    }
    int wrapped = index % count;
    if (wrapped < 0) {
        wrapped += count;
    }
    return wrapped;
}

static int wrap_costume_number(double index, int count) {
    if (count <= 0) {
        return !isfinite(index) || index < 0.0 ? 0 :
            (index > (double)INT_MAX ? INT_MAX : (int)floor(index + 0.5));
    }
    if (!isfinite(index)) {
        return 0;
    }
    const double rounded = floor(index + 0.5);
    double wrapped = fmod(rounded, (double)count);
    if (wrapped < 0.0) {
        wrapped += (double)count;
    }
    return (int)wrapped;
}

SSprite *sjit_sprite_create(int id, int drawable_id, const char *name, int is_stage) {
    SSprite *sprite = (SSprite *)calloc(1, sizeof(SSprite));
    if (!sprite) {
        return NULL;
    }
    sjit_target_init(&sprite->base, id, name ? name : "", is_stage, 1);
    sprite->sprite_id = id;
    sprite->drawable_id = drawable_id;
    sprite->x = 0.0;
    sprite->y = 0.0;
    sprite->direction = 90.0;
    sprite->size = 100.0;
    sprite->visible = is_stage ? 0 : 1;
    sprite->current_costume = 0;
    sprite->rotation_style = 0;
    sprite->draggable = 0;
    sprite->volume = 100.0;
    sprite->layer_order = id;
    sprite->pen_down = 0;
    sprite->pen_size = 1.0;
    sprite->pen_r = 0;
    sprite->pen_g = 0;
    sprite->pen_b = 255;
    sprite->pen_a = 255;
    sprite->pen_hue = 66.66666666666667;
    sprite->pen_saturation = 100.0;
    sprite->pen_brightness = 100.0;
    sprite->pen_transparency = 0.0;
    sprite->costume_names = NULL;
    sprite->costume_count = 0;
    for (int effect = 0; effect < SJIT_GRAPHIC_EFFECT_COUNT; ++effect) {
        sprite->graphic_effects[effect] = 0.0;
    }
    return sprite;
}

SSprite *sjit_sprite_clone(const SSprite *source, int id, int drawable_id) {
    if (!source) {
        return NULL;
    }
    SSprite *clone = sjit_sprite_create(id, drawable_id, sjit_string_cstr(source->base.name), source->base.is_stage);
    if (!clone) {
        return NULL;
    }
    clone->base.is_original = 0;
    clone->sprite_id = source->sprite_id;
    clone->x = source->x;
    clone->y = source->y;
    clone->direction = source->direction;
    clone->size = source->size;
    clone->visible = source->visible;
    clone->current_costume = source->current_costume;
    clone->rotation_style = source->rotation_style;
    clone->draggable = source->draggable;
    clone->volume = source->volume;
    clone->layer_order = source->layer_order + 1;
    clone->pen_down = source->pen_down;
    clone->pen_size = source->pen_size;
    clone->pen_r = source->pen_r;
    clone->pen_g = source->pen_g;
    clone->pen_b = source->pen_b;
    clone->pen_a = source->pen_a;
    clone->pen_hue = source->pen_hue;
    clone->pen_saturation = source->pen_saturation;
    clone->pen_brightness = source->pen_brightness;
    clone->pen_transparency = source->pen_transparency;
    memcpy(
        clone->graphic_effects,
        source->graphic_effects,
        sizeof(clone->graphic_effects));
    if (source->costume_count > 0) {
        const char **names = (const char **)calloc(
            (size_t)source->costume_count,
            sizeof(const char *));
        if (!names) {
            sjit_sprite_destroy(clone);
            return NULL;
        }
        for (int i = 0; i < source->costume_count; ++i) {
            names[i] = sjit_string_cstr(source->costume_names[i]);
        }
        const int copied = sjit_sprite_set_costume_names(
            clone,
            names,
            source->costume_count);
        free(names);
        if (!copied) {
            sjit_sprite_destroy(clone);
            return NULL;
        }
    }
    sjit_target_copy_variables(&clone->base, &source->base);
    return clone;
}

void sjit_sprite_destroy(SSprite *sprite) {
    if (!sprite) {
        return;
    }
    destroy_costume_names(sprite->costume_names, sprite->costume_count);
    sprite->costume_names = NULL;
    sprite->costume_count = 0;
    sjit_target_destroy(&sprite->base);
    free(sprite);
}

void sjit_sprite_set_xy(SRuntime *runtime, SSprite *sprite, double x, double y, int force) {
    (void)force;
    if (!sprite) {
        return;
    }
    if (runtime && sprite->pen_down && (sprite->x != x || sprite->y != y)) {
        sjit_pen_path_push(
            &runtime->pen,
            sprite->base.id,
            sprite->x,
            sprite->y,
            x,
            y,
            sprite->pen_size,
            sprite->pen_r,
            sprite->pen_g,
            sprite->pen_b,
            sprite->pen_a);
        runtime->pen_materialized_valid = 0;
    }
    sprite->x = x;
    sprite->y = y;
    sjit_runtime_request_redraw(runtime);
}

void sjit_sprite_set_direction(SRuntime *runtime, SSprite *sprite, double direction) {
    if (!sprite) {
        return;
    }
    sprite->direction = wrap_direction(direction);
    sjit_runtime_request_redraw(runtime);
}

void sjit_sprite_set_visible(SRuntime *runtime, SSprite *sprite, int visible) {
    if (!sprite) {
        return;
    }
    sprite->visible = visible ? 1 : 0;
    sjit_runtime_request_redraw(runtime);
}

void sjit_sprite_set_draggable(SSprite *sprite, int draggable) {
    if (sprite) {
        sprite->draggable = draggable ? 1 : 0;
    }
}

int sjit_sprite_set_costume_names(
    SSprite *sprite,
    const char *const *costume_names,
    int costume_count) {
    if (!sprite || costume_count < 0 ||
        (costume_count > 0 && !costume_names) ||
        (size_t)costume_count > SIZE_MAX / sizeof(SString *)) {
        return 0;
    }
    SString **next = NULL;
    if (costume_count > 0) {
        next = (SString **)calloc((size_t)costume_count, sizeof(SString *));
        if (!next) {
            return 0;
        }
        for (int i = 0; i < costume_count; ++i) {
            next[i] = sjit_string_new(costume_names[i] ? costume_names[i] : "");
            if (!next[i]) {
                destroy_costume_names(next, costume_count);
                return 0;
            }
        }
    }
    destroy_costume_names(sprite->costume_names, sprite->costume_count);
    sprite->costume_names = next;
    sprite->costume_count = costume_count;
    sprite->current_costume = wrap_costume_index(
        sprite->current_costume,
        costume_count);
    return 1;
}

int sjit_sprite_costume_index_by_name(const SSprite *sprite, const char *name) {
    if (!sprite || !name) {
        return -1;
    }
    for (int i = 0; i < sprite->costume_count; ++i) {
        if (strcmp(sjit_string_cstr(sprite->costume_names[i]), name) == 0) {
            return i;
        }
    }
    return -1;
}

const char *sjit_sprite_current_costume_name(const SSprite *sprite) {
    if (!sprite || sprite->current_costume < 0 ||
        sprite->current_costume >= sprite->costume_count ||
        !sprite->costume_names) {
        return "";
    }
    return sjit_string_cstr(sprite->costume_names[sprite->current_costume]);
}

void sjit_sprite_set_costume(SRuntime *runtime, SSprite *sprite, int costume_index) {
    if (!sprite) {
        return;
    }
    sprite->current_costume = wrap_costume_index(
        costume_index,
        sprite->costume_count);
    sjit_runtime_request_redraw(runtime);
}

void sjit_sprite_set_costume_number(
    SRuntime *runtime,
    SSprite *sprite,
    double costume_index) {
    if (!sprite) {
        return;
    }
    sprite->current_costume = wrap_costume_number(
        costume_index,
        sprite->costume_count);
    sjit_runtime_request_redraw(runtime);
}
