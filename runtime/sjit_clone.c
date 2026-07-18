#include "sjit_clone.h"

#include "sjit_scheduler.h"

#include <stddef.h>

SSprite *sjit_clone_create(SRuntime *runtime, SSprite *source) {
    if (!runtime || !source) {
        return NULL;
    }
    SSprite *clone = sjit_sprite_clone(source, runtime->next_target_id++, runtime->next_drawable_id++);
    if (!clone) {
        return NULL;
    }
    sjit_runtime_add_sprite(runtime, clone);
    sjit_runtime_start_hats(runtime, SJIT_HAT_CONTROL_START_AS_CLONE, NULL);
    return clone;
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
