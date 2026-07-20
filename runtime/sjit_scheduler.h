#ifndef SJIT_SCHEDULER_H
#define SJIT_SCHEDULER_H

#include "sjit_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

void sjit_scheduler_add_thread(SRuntime *runtime, int target_id, int script_id);
SThread *sjit_scheduler_start_script(SRuntime *runtime, SScriptRegistration *registration);
SThread *sjit_scheduler_start_script_for_target(
    SRuntime *runtime,
    SScriptRegistration *registration,
    int target_id);
void sjit_scheduler_restart_thread(SRuntime *runtime, int thread_id);
void sjit_scheduler_stop_thread(SRuntime *runtime, int thread_id);
void sjit_scheduler_stop_for_target(SRuntime *runtime, int target_id, int except_thread_id);
SRuntimeStatus sjit_scheduler_tick(SRuntime *runtime);

#ifdef __cplusplus
}
#endif

#endif
