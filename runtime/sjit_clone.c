#include "sjit_clone.h"

#include "sjit_number.h"
#include "sjit_scheduler.h"
#include "sjit_string.h"
#include "sjit_value.h"

#include <ctype.h>
#include <stddef.h>

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

SSprite *sjit_clone_create(SRuntime *runtime, SSprite *source) {
    if (!runtime || !source) {
        return NULL;
    }
    SSprite *clone = sjit_sprite_clone(source, runtime->next_target_id++, runtime->next_drawable_id++);
    if (!clone) {
        return NULL;
    }
    if (!sjit_runtime_add_sprite(runtime, clone)) {
        sjit_sprite_destroy(clone);
        return NULL;
    }
    sjit_runtime_start_clone_hats(runtime, clone);
    return clone;
}

SSprite *sjit_clone_create_requested(SRuntime *runtime, SSprite *current, SValue requested) {
    if (!runtime || !current) {
        return NULL;
    }
    SValue text = sjit_to_string(runtime, requested);
    const char *name = sjit_string_cstr((const SString *)text.ptr);
    SSprite *source = current;
    if (!text_equal(name, "_myself_") && !text_equal(name, "myself")) {
        source = sjit_runtime_get_sprite_by_name(runtime, name);
    }
    sjit_value_destroy_fast(text);
    return source ? sjit_clone_create(runtime, source) : NULL;
}

void sjit_clone_delete(SRuntime *runtime, SSprite *clone) {
    if (!runtime || !clone || clone->base.is_original) {
        return;
    }
    sjit_scheduler_stop_for_target(runtime, clone->base.id, -1);
    for (int i = 0; i < runtime->target_count; ++i) {
        if (runtime->targets[i] == clone) {
            sjit_sprite_destroy(runtime->targets[i]);
            for (int j = i; j < runtime->target_count - 1; ++j) {
                runtime->targets[j] = runtime->targets[j + 1];
            }
            --runtime->target_count;
            return;
        }
    }
}
