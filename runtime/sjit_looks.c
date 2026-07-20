#include "sjit_looks.h"

#include "sjit_number.h"
#include "sjit_string.h"
#include "sjit_value.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static SSprite *stage_target(SRuntime *runtime);

void sjit_looks_show(SRuntime *runtime, SSprite *sprite) {
    sjit_sprite_set_visible(runtime, sprite, 1);
}

void sjit_looks_hide(SRuntime *runtime, SSprite *sprite) {
    sjit_sprite_set_visible(runtime, sprite, 0);
}

void sjit_looks_say(SRuntime *runtime, SSprite *sprite, SValue message, int thought) {
    if (!runtime) {
        return;
    }
    SValue text = sjit_to_string(runtime, message);
    const char *raw = sjit_string_cstr((const SString *)text.ptr);
    if (sprite) {
        sjit_string_destroy(sprite->bubble_text);
        sprite->bubble_text = raw[0] == '\0' ? NULL : sjit_string_new(raw);
        sprite->bubble_thought = raw[0] == '\0' ? 0 : (thought ? 1 : 0);
        sprite->bubble_until_ms = -1.0;
    }
    if (raw[0] != '\0') {
        printf("%s: %s\n", thought ? "think" : "say", raw);
    }
    sjit_value_destroy_fast(text);
    sjit_runtime_request_redraw(runtime);
}

void sjit_looks_next_costume(SRuntime *runtime, SSprite *sprite) {
    if (sprite && sprite->costume_count > 0) {
        sjit_sprite_set_costume(runtime, sprite, sprite->current_costume + 1);
    }
}

void sjit_looks_next_backdrop(SRuntime *runtime) {
    if (!runtime) {
        return;
    }
    SSprite *stage = stage_target(runtime);
    if (!stage || stage->costume_count <= 0) {
        return;
    }
    sjit_sprite_set_costume(runtime, stage, stage->current_costume + 1);
    sjit_runtime_start_hats(
        runtime,
        SJIT_HAT_EVENT_WHENBACKDROPSWITCHESTO,
        sjit_sprite_current_costume_name(stage));
}

void sjit_looks_change_size(SRuntime *runtime, SSprite *sprite, double change) {
    if (!sprite || sprite->base.is_stage) {
        return;
    }
    if (!isfinite(change)) {
        change = 0.0;
    }
    sprite->size = fmax(0.0, sprite->size + change);
    sjit_runtime_request_redraw(runtime);
}

void sjit_looks_change_stretch(SRuntime *runtime, SSprite *sprite, double change) {
    /* Scratch 3 retains this Scratch 2 compatibility opcode as a no-op. */
    (void)runtime;
    (void)sprite;
    (void)change;
}

void sjit_looks_set_stretch(SRuntime *runtime, SSprite *sprite, double value) {
    /* Scratch 3 retains this Scratch 2 compatibility opcode as a no-op. */
    (void)runtime;
    (void)sprite;
    (void)value;
}

void sjit_looks_go_forward_backward_layers(SRuntime *runtime, SSprite *sprite, int layers) {
    if (!sprite || sprite->base.is_stage) {
        return;
    }
    const long long next = (long long)sprite->layer_order + (long long)layers;
    sprite->layer_order = next > INT_MAX ? INT_MAX :
        (next < INT_MIN ? INT_MIN : (int)next);
    sjit_runtime_request_redraw(runtime);
}

void sjit_looks_hide_all_sprites(SRuntime *runtime) {
    /* Scratch 3 retains this Scratch 2 compatibility opcode as a no-op. */
    (void)runtime;
}

SValue sjit_looks_backdrop_number_name(SRuntime *runtime, int number_name) {
    SSprite *stage = stage_target(runtime);
    if (number_name) {
        return sjit_make_number(
            stage && stage->costume_count > 0 ? stage->current_costume + 1.0 : 0.0);
    }
    return sjit_make_string(sjit_sprite_current_costume_name(stage));
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
    if (!runtime || !sprite || sprite->base.is_stage) {
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

static int start_backdrop_hats(SRuntime *runtime, const SSprite *stage) {
    if (!runtime || !stage || stage->costume_count <= 0) {
        return 0;
    }
    return sjit_runtime_start_hats(
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

void sjit_looks_switch_backdrop_and_wait(
    SRuntime *runtime,
    SFrame *frame,
    SValue requested_backdrop,
    int resume_pc,
    SRuntimeStatus *status) {
    if (status) {
        *status = SJIT_STATUS_OK;
    }
    if (!runtime) {
        return;
    }
    if (!frame) {
        sjit_looks_switch_backdrop(runtime, requested_backdrop);
        return;
    }
    if (frame->timed_event_kind != 2) {
        frame->timed_event_kind = 2;
        frame->started_thread_begin = sjit_runtime_next_thread_id(runtime);
        sjit_looks_switch_backdrop(runtime, requested_backdrop);
        frame->started_thread_count =
            sjit_runtime_next_thread_id(runtime) - frame->started_thread_begin;
        frame->pc = resume_pc;
        if (frame->started_thread_count > 0) {
            if (status) *status = SJIT_STATUS_YIELDED;
            return;
        }
        frame->timed_event_kind = 0;
        frame->started_thread_begin = -1;
        frame->started_thread_count = -1;
        return;
    }
    if (sjit_runtime_count_threads_in_id_range(
            runtime,
            frame->started_thread_begin,
            frame->started_thread_count) > 0) {
        frame->pc = resume_pc;
        if (status) *status = SJIT_STATUS_YIELDED;
        return;
    }
    frame->timed_event_kind = 0;
    frame->started_thread_begin = -1;
    frame->started_thread_count = -1;
}

void sjit_looks_switch_backdrop_value_ptr(
    SRuntime *runtime,
    SValue *requested_backdrop) {
    sjit_looks_switch_backdrop(
        runtime,
        requested_backdrop ? *requested_backdrop : sjit_make_null());
}
