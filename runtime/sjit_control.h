#ifndef SJIT_CONTROL_H
#define SJIT_CONTROL_H

#include "sjit_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

SLoopState *sjit_control_loop_state(SFrame *frame, const void *key, int create);
SLoopState *sjit_control_loop_state_at_depth(SFrame *frame, const void *key, int scope_depth, int create);
int sjit_control_repeat_should_enter(SRuntime *runtime, SFrame *frame, const void *key, SValue times);
int sjit_control_repeat_should_enter_at_depth(
    SRuntime *runtime,
    SFrame *frame,
    const void *key,
    int scope_depth,
    SValue times);
void sjit_control_loop_reset(SFrame *frame, const void *key);
void sjit_control_loop_reset_at_depth(SFrame *frame, const void *key, int scope_depth);
void sjit_control_loop_reset_from_depth(SFrame *frame, int min_scope_depth);
void sjit_control_repeat_reset(SFrame *frame);
SRuntimeStatus sjit_control_wait(SRuntime *runtime, SFrame *frame, SValue duration_seconds, int resume_pc);
void sjit_control_stop_all(SRuntime *runtime);
void sjit_control_stop_this_script(SThread *thread);

#ifdef __cplusplus
}
#endif

#endif
