#include "sjit_looks.h"

#include "sjit_number.h"
#include "sjit_string.h"
#include "sjit_value.h"

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

void sjit_looks_show(SRuntime *runtime, SSprite *sprite) {
    sjit_sprite_set_visible(runtime, sprite, 1);
}

void sjit_looks_hide(SRuntime *runtime, SSprite *sprite) {
    sjit_sprite_set_visible(runtime, sprite, 0);
}

void sjit_looks_switch_costume(
    SRuntime *runtime,
    SSprite *sprite,
    SValue requested_costume) {
    if (!sprite || sprite->costume_count <= 0) {
        return;
    }
    if (requested_costume.tag == SJIT_VALUE_NUMBER) {
        sjit_sprite_set_costume_number(
            runtime,
            sprite,
            requested_costume.number - 1.0);
        return;
    }

    SValue text_value = sjit_to_string(runtime, requested_costume);
    const char *text = sjit_string_cstr((const SString *)text_value.ptr);
    const int named_index = sjit_sprite_costume_index_by_name(sprite, text);
    if (named_index >= 0) {
        sjit_sprite_set_costume(runtime, sprite, named_index);
        sjit_value_destroy_fast(text_value);
        return;
    }

    double numeric_index = 0.0;
    int whitespace = 0;
    if (sjit_parse_number_for_compare_fast(
            requested_costume,
            &numeric_index,
            &whitespace) && !whitespace) {
        sjit_sprite_set_costume_number(runtime, sprite, numeric_index - 1.0);
    }
    sjit_value_destroy_fast(text_value);
}

void sjit_looks_go_to_front_back(
    SRuntime *runtime,
    SSprite *sprite,
    int front) {
    if (!runtime || !sprite) {
        return;
    }

    int extreme = sprite->layer_order;
    for (int i = 0; i < runtime->target_count; ++i) {
        const SSprite *target = runtime->targets[i];
        if (!target || target == sprite) {
            continue;
        }
        if (front) {
            if (target->layer_order > extreme) {
                extreme = target->layer_order;
            }
        } else if (target->layer_order < extreme) {
            extreme = target->layer_order;
        }
    }

    if (front) {
        if (extreme < INT_MAX) {
            sprite->layer_order = extreme + 1;
        }
    } else if (extreme > INT_MIN) {
        sprite->layer_order = extreme - 1;
    }
    sjit_runtime_request_redraw(runtime);
}

static int ascii_case_equal(const char *left, const char *right) {
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

int sjit_looks_effect_from_name(const char *effect_name) {
    static const char *const names[SJIT_GRAPHIC_EFFECT_COUNT] = {
        "color", "fisheye", "whirl", "pixelate", "mosaic", "brightness", "ghost"};
    for (int effect = 0; effect < SJIT_GRAPHIC_EFFECT_COUNT; ++effect) {
        if (ascii_case_equal(effect_name, names[effect])) {
            return effect;
        }
    }
    return -1;
}

static double clamp_effect(int effect, double value) {
    if (effect == SJIT_GRAPHIC_EFFECT_GHOST) {
        if (value < 0.0) return 0.0;
        if (value > 100.0) return 100.0;
    } else if (effect == SJIT_GRAPHIC_EFFECT_BRIGHTNESS) {
        if (value < -100.0) return -100.0;
        if (value > 100.0) return 100.0;
    }
    return value;
}

void sjit_looks_set_effect_number(
    SRuntime *runtime,
    SSprite *sprite,
    int effect,
    double value) {
    if (!sprite || effect < 0 || effect >= SJIT_GRAPHIC_EFFECT_COUNT) {
        return;
    }
    sprite->graphic_effects[effect] = clamp_effect(effect, value);
    if (sprite->visible) {
        sjit_runtime_request_redraw(runtime);
    }
}

void sjit_looks_change_effect_number(
    SRuntime *runtime,
    SSprite *sprite,
    int effect,
    double change) {
    if (!sprite || effect < 0 || effect >= SJIT_GRAPHIC_EFFECT_COUNT) {
        return;
    }
    sjit_looks_set_effect_number(
        runtime,
        sprite,
        effect,
        sprite->graphic_effects[effect] + change);
}

void sjit_looks_clear_effects(SRuntime *runtime, SSprite *sprite) {
    if (!sprite) {
        return;
    }
    for (int effect = 0; effect < SJIT_GRAPHIC_EFFECT_COUNT; ++effect) {
        sprite->graphic_effects[effect] = 0.0;
    }
    if (sprite->visible) {
        sjit_runtime_request_redraw(runtime);
    }
}

static SSprite *stage_target(SRuntime *runtime) {
    if (!runtime) {
        return NULL;
    }
    for (int i = 0; i < runtime->target_count; ++i) {
        SSprite *target = runtime->targets[i];
        if (target && target->base.is_stage) {
            return target;
        }
    }
    return NULL;
}

static void start_backdrop_hats(SRuntime *runtime, const SSprite *stage) {
    if (!runtime || !stage || stage->costume_count <= 0) {
        return;
    }
    sjit_runtime_start_hats(
        runtime,
        SJIT_HAT_EVENT_WHENBACKDROPSWITCHESTO,
        sjit_sprite_current_costume_name(stage));
}

void sjit_looks_switch_backdrop(SRuntime *runtime, SValue requested_backdrop) {
    SSprite *stage = stage_target(runtime);
    if (!stage || stage->costume_count <= 0) {
        return;
    }

    if (requested_backdrop.tag == SJIT_VALUE_NUMBER) {
        sjit_sprite_set_costume_number(
            runtime,
            stage,
            requested_backdrop.number - 1.0);
        start_backdrop_hats(runtime, stage);
        return;
    }

    SValue text_value = sjit_to_string(runtime, requested_backdrop);
    const char *text = sjit_string_cstr((const SString *)text_value.ptr);
    const int named_index = sjit_sprite_costume_index_by_name(stage, text);
    if (named_index >= 0) {
        sjit_sprite_set_costume(runtime, stage, named_index);
        sjit_value_destroy_fast(text_value);
        start_backdrop_hats(runtime, stage);
        return;
    }

    if (strcmp(text, "next backdrop") == 0) {
        sjit_sprite_set_costume(runtime, stage, stage->current_costume + 1);
        sjit_value_destroy_fast(text_value);
        start_backdrop_hats(runtime, stage);
        return;
    }
    if (strcmp(text, "previous backdrop") == 0) {
        sjit_sprite_set_costume(runtime, stage, stage->current_costume - 1);
        sjit_value_destroy_fast(text_value);
        start_backdrop_hats(runtime, stage);
        return;
    }
    if (strcmp(text, "random backdrop") == 0) {
        if (stage->costume_count > 1) {
            int selected = rand() % (stage->costume_count - 1);
            if (selected >= stage->current_costume) {
                ++selected;
            }
            sjit_sprite_set_costume(runtime, stage, selected);
        }
        sjit_value_destroy_fast(text_value);
        start_backdrop_hats(runtime, stage);
        return;
    }

    double numeric_index = 0.0;
    int whitespace = 0;
    if (sjit_parse_number_for_compare_fast(
            requested_backdrop,
            &numeric_index,
            &whitespace) && !whitespace) {
        sjit_sprite_set_costume_number(
            runtime,
            stage,
            numeric_index - 1.0);
    }
    sjit_value_destroy_fast(text_value);
    /* Scratch starts the hat for the final backdrop even when the requested
       value is invalid or resolves to the already-selected backdrop. */
    start_backdrop_hats(runtime, stage);
}

void sjit_looks_switch_backdrop_value_ptr(
    SRuntime *runtime,
    SValue *requested_backdrop) {
    sjit_looks_switch_backdrop(
        runtime,
        requested_backdrop ? *requested_backdrop : sjit_make_null());
}
