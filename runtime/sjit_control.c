#include "sjit_control.h"

#include "sjit_number.h"

#include <limits.h>
#include <math.h>
#include <string.h>

static double scratch_round(double value) {
    if (isnan(value)) {
        return 0.0;
    }
    if (isinf(value)) {
        return value;
    }
    return floor(value + 0.5);
}

SLoopState *sjit_control_loop_state_at_depth(SFrame *frame, const void *key, int scope_depth, int create) {
    if (!frame || !key) {
        return NULL;
    }
    if (scope_depth < 0) {
        scope_depth = 0;
    }
    if (frame->loop_state_cache_index >= 0 &&
        frame->loop_state_cache_index < frame->loop_state_count) {
        SLoopState *cached = &frame->loop_states[frame->loop_state_cache_index];
        if (cached->key == key && cached->scope_depth == scope_depth) {
            return cached;
        }
    }
    int empty = -1;
    for (int i = 0; i < frame->loop_state_count; ++i) {
        if (frame->loop_states[i].key == key && frame->loop_states[i].scope_depth == scope_depth) {
            frame->loop_state_cache_index = i;
            return &frame->loop_states[i];
        }
        if (!frame->loop_states[i].key && empty < 0) {
            empty = i;
        }
    }
    if (!create) {
        return NULL;
    }
    if (empty < 0) {
        if (frame->loop_state_count >= SJIT_MAX_LOOP_STATES) {
            return NULL;
        }
        empty = frame->loop_state_count++;
    }
    SLoopState *state = &frame->loop_states[empty];
    memset(state, 0, sizeof(*state));
    state->key = key;
    state->scope_depth = scope_depth;
    frame->loop_state_cache_index = empty;
    return state;
}

SLoopState *sjit_control_loop_state(SFrame *frame, const void *key, int create) {
    return sjit_control_loop_state_at_depth(frame, key, 0, create);
}

int sjit_control_repeat_should_enter_at_depth(
    SRuntime *runtime,
    SFrame *frame,
    const void *key,
    int scope_depth,
    SValue times) {
    SLoopState *state = sjit_control_loop_state_at_depth(frame, key, scope_depth, 0);
    if (!state) {
        state = sjit_control_loop_state_at_depth(frame, key, scope_depth, 1);
        if (state) {
            state->counter = scratch_round(sjit_to_number_fast(runtime, times));
        }
    }
    if (!state) {
        return 0;
    }
    state->counter -= 1.0;
    return state->counter >= 0.0;
}

int sjit_control_repeat_should_enter(SRuntime *runtime, SFrame *frame, const void *key, SValue times) {
    return sjit_control_repeat_should_enter_at_depth(runtime, frame, key, 0, times);
}

static void shrink_loop_state_count(SFrame *frame) {
    if (!frame) {
        return;
    }
    while (frame->loop_state_count > 0 &&
           !frame->loop_states[frame->loop_state_count - 1].key) {
        --frame->loop_state_count;
    }
}

void sjit_control_loop_reset_at_depth(SFrame *frame, const void *key, int scope_depth) {
    SLoopState *state = sjit_control_loop_state_at_depth(frame, key, scope_depth, 0);
    if (state) {
        memset(state, 0, sizeof(*state));
        frame->loop_state_cache_index = -1;
        shrink_loop_state_count(frame);
    }
}

void sjit_control_loop_reset(SFrame *frame, const void *key) {
    sjit_control_loop_reset_at_depth(frame, key, 0);
}

void sjit_control_loop_reset_from_depth(SFrame *frame, int min_scope_depth) {
    if (!frame) {
        return;
    }
    if (min_scope_depth < 0) {
        min_scope_depth = 0;
    }
    for (int i = 0; i < frame->loop_state_count; ++i) {
        if (frame->loop_states[i].key && frame->loop_states[i].scope_depth >= min_scope_depth) {
            memset(&frame->loop_states[i], 0, sizeof(frame->loop_states[i]));
        }
    }
    frame->loop_state_cache_index = -1;
    shrink_loop_state_count(frame);
}

void sjit_control_repeat_reset(SFrame *frame) {
    if (frame) {
        for (int i = 0; i < frame->loop_state_count; ++i) {
            memset(&frame->loop_states[i], 0, sizeof(frame->loop_states[i]));
        }
        frame->loop_state_count = 0;
        frame->loop_state_cache_index = -1;
        frame->loop_counter = INT_MIN;
    }
}

SRuntimeStatus sjit_control_wait(SRuntime *runtime, SFrame *frame, SValue duration_seconds, int resume_pc) {
    if (!runtime || !frame) {
        return SJIT_STATUS_ERROR;
    }
    if (frame->wake_time_ms < 0.0) {
        double duration = 1000.0 * sjit_to_number_fast(runtime, duration_seconds);
        if (duration < 0.0) {
            duration = 0.0;
        }
        frame->wake_time_ms = runtime->now_ms + duration;
        frame->pc = resume_pc;
        sjit_runtime_request_redraw(runtime);
        return SJIT_STATUS_YIELDED;
    }
    if (runtime->now_ms < frame->wake_time_ms) {
        frame->pc = resume_pc;
        return SJIT_STATUS_YIELDED;
    }
    frame->wake_time_ms = -1.0;
    return SJIT_STATUS_OK;
}

void sjit_control_stop_all(SRuntime *runtime) {
    sjit_runtime_stop_all(runtime);
}

void sjit_control_stop_this_script(SThread *thread) {
    if (!thread) {
        return;
    }
    thread->status = SJIT_THREAD_DONE;
    thread->is_killed = 1;
}
