#include "sjit_frame.h"

#include "sjit_value.h"

#include <limits.h>
#include <string.h>

void sjit_frame_init(SFrame *frame) {
    if (!frame) {
        return;
    }
    memset(frame, 0, sizeof(SFrame));
    frame->pc = 0;
    frame->return_pc = -1;
    frame->wake_time_ms = -1.0;
    frame->loop_counter = INT_MIN;
    frame->loop_state_cache_index = -1;
    frame->started_thread_begin = -1;
    frame->started_thread_count = -1;
    for (int i = 0; i < SJIT_MAX_LOCALS; ++i) {
        frame->locals[i] = sjit_make_null();
    }
    for (int i = 0; i < SJIT_MAX_STACK; ++i) {
        frame->stack[i] = sjit_make_null();
    }
    for (int i = 0; i < SJIT_MAX_PARAMS; ++i) {
        frame->params[i] = sjit_make_null();
    }
}

void sjit_frame_reset(SFrame *frame) {
    sjit_frame_init(frame);
}

int sjit_frame_stack_push(SFrame *frame, SValue value) {
    if (!frame || frame->stack_top >= SJIT_MAX_STACK) {
        return 0;
    }
    frame->stack[frame->stack_top++] = sjit_value_clone(value);
    return 1;
}

SValue sjit_frame_stack_pop(SFrame *frame) {
    if (!frame || frame->stack_top <= 0) {
        return sjit_make_null();
    }
    SValue value = frame->stack[--frame->stack_top];
    frame->stack[frame->stack_top] = sjit_make_null();
    return value;
}
