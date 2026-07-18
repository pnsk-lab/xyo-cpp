#include "sjit_scheduler.h"

#include "sjit_script.h"
#include "sjit_thread_pool.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef struct {
    SThread *thread;
    SRuntimeStatus result;
    int owner_target_id;
    const void *script_data;
} SParallelStep;

typedef struct {
    SRuntime *runtime;
    SParallelStep *steps;
} SParallelBatchContext;

static int ensure_thread_capacity(SRuntime *runtime, int wanted) {
    if (!runtime) {
        return 0;
    }
    if (wanted <= runtime->thread_capacity) {
        return 1;
    }
    int next = runtime->thread_capacity > 0 ? runtime->thread_capacity : SJIT_INITIAL_CAPACITY;
    while (next < wanted) {
        next *= 2;
    }
    SThread **threads = (SThread **)realloc(runtime->threads, sizeof(SThread *) * (size_t)next);
    if (!threads) {
        return 0;
    }
    runtime->threads = threads;
    runtime->thread_capacity = next;
    return 1;
}

static SScriptRegistration *find_script(SRuntime *runtime, int script_id) {
    if (!runtime) {
        return NULL;
    }
    for (int i = 0; i < runtime->script_count; ++i) {
        if (runtime->scripts[i].script_id == script_id) {
            return &runtime->scripts[i];
        }
    }
    return NULL;
}

static int ensure_parallel_step_capacity(SRuntime *runtime, int wanted) {
    if (!runtime || wanted < 0) {
        return 0;
    }
    if (wanted <= runtime->parallel_step_capacity) {
        return runtime->parallel_steps != NULL || wanted == 0;
    }
    int next = runtime->parallel_step_capacity > 0 ?
        runtime->parallel_step_capacity : SJIT_INITIAL_CAPACITY;
    while (next < wanted) {
        if (next > INT32_MAX / 2) {
            next = wanted;
            break;
        }
        next *= 2;
    }
    SParallelStep *steps = (SParallelStep *)realloc(
        runtime->parallel_steps,
        sizeof(SParallelStep) * (size_t)next);
    if (!steps) {
        return 0;
    }
    runtime->parallel_steps = steps;
    runtime->parallel_step_capacity = next;
    return 1;
}

static int parallel_value_is_primitive(SValue value) {
    return value.tag == SJIT_VALUE_NUMBER ||
        value.tag == SJIT_VALUE_BOOL ||
        value.tag == SJIT_VALUE_NULL;
}

static int parallel_dependencies_are_valid(
    const SScriptRegistration *registration,
    const SSprite *owner) {
    if (!registration || !owner ||
        registration->ownership_variable_count < 0 ||
        registration->ownership_variable_count >
            owner->base.variable_count ||
        (registration->ownership_variable_count > 0 &&
         (!registration->ownership_variable_indices ||
          !owner->base.variables))) {
        return 0;
    }
    for (int i = 0;
         i < registration->ownership_variable_count;
         ++i) {
        const int index = registration->ownership_variable_indices[i];
        if (index < 0 || index >= owner->base.variable_count) {
            return 0;
        }
        const SVariable *variable = &owner->base.variables[index];
        if (variable->type != SJIT_VAR_SCALAR || variable->is_cloud ||
            (variable->scalar_kind != SJIT_SCALAR_NUMBER &&
             variable->scalar_kind != SJIT_SCALAR_BOOL) ||
            !parallel_value_is_primitive(variable->value)) {
            return 0;
        }
    }
    return 1;
}

static SScriptRegistration *parallel_registration_for_thread(
    SRuntime *runtime,
    const SThread *thread) {
    if (!runtime || !thread) {
        return NULL;
    }
    SScriptRegistration *match = NULL;
    int matches = 0;
    int script_id_matches = 0;
    for (int i = 0; i < runtime->script_count; ++i) {
        SScriptRegistration *candidate = &runtime->scripts[i];
        if (candidate->script_id == thread->script_id) {
            ++script_id_matches;
        }
        if (candidate->script_id == thread->script_id &&
            candidate->target_id == thread->target_id &&
            candidate->entry == thread->entry &&
            candidate->script_data == thread->script_data) {
            match = candidate;
            ++matches;
        }
    }
    if (script_id_matches != 1 || matches != 1 ||
        !match->ownership_analyzed ||
        !match->parallel_safe || !match->ownership_script ||
        match->ownership_script != thread->script_data ||
        match->parallel_owner_target_id != thread->target_id ||
        match->ownership_entry != match->entry ||
        match->ownership_procedure_count !=
            match->ownership_script->procedure_count ||
        (match->ownership_procedure_count > 0 &&
         !match->ownership_script->procedures)) {
        return NULL;
    }
    for (int i = 0; i < match->ownership_procedure_count; ++i) {
        if (!match->ownership_procedure_entries ||
            match->ownership_procedure_entries[i] !=
                match->ownership_script->procedures[i].jit_entry) {
            return NULL;
        }
    }
    SSprite *owner = sjit_runtime_get_sprite(runtime, thread->target_id);
    if (!owner || owner->base.id != match->parallel_owner_target_id ||
        owner->base.is_stage || !owner->base.is_original) {
        return NULL;
    }
    /* The semantic AST is immutable after compilation/attestation, whose
       cache checks proved that native embedded locations match these indices.
       Guarded interpreter caches may refresh without changing that mapping.
       Only scalar values and kinds remain ownership-relevant dynamic state,
       so validate the compact manifest instead of rescanning the complete
       reachable tree on every scheduler pass. */
    if (!parallel_dependencies_are_valid(match, owner)) {
        return NULL;
    }
    return match;
}

static int parallel_step_conflicts(
    const SParallelStep *steps,
    int count,
    int owner_target_id,
    const void *script_data) {
    for (int i = 0; i < count; ++i) {
        if (steps[i].owner_target_id == owner_target_id ||
            steps[i].script_data == script_data) {
            return 1;
        }
    }
    return 0;
}

static SJitThreadPool *scheduler_thread_pool(SRuntime *runtime) {
    if (!runtime) {
        return NULL;
    }
#ifdef SJIT_PROFILE_RUNTIME
    /* Runtime profiling counters are intentionally cheap and non-atomic. */
    runtime->thread_pool_initialized = 1;
    runtime->thread_pool_parallelism = 1;
    return NULL;
#else
    if (!runtime->thread_pool_initialized) {
        runtime->thread_pool_initialized = 1;
        const int requested =
            sjit_thread_pool_environment_parallelism();
        if (requested >= 2) {
            runtime->thread_pool = sjit_thread_pool_create(requested);
        }
        runtime->thread_pool_parallelism = runtime->thread_pool ?
            sjit_thread_pool_parallelism(
                (const SJitThreadPool *)runtime->thread_pool) : 1;
        if (getenv("SJIT_LOG_THREAD_POOL") != NULL) {
            fprintf(
                stderr,
                "sjit: thread pool requested=%d active=%d\n",
                requested,
                runtime->thread_pool_parallelism);
        }
    }
    return (SJitThreadPool *)runtime->thread_pool;
#endif
}

void sjit_scheduler_add_thread(SRuntime *runtime, int target_id, int script_id) {
    SScriptRegistration *registration = find_script(runtime, script_id);
    if (registration && registration->target_id == target_id) {
        sjit_scheduler_start_script(runtime, registration);
    }
}

SThread *sjit_scheduler_start_script(SRuntime *runtime, SScriptRegistration *registration) {
    if (!runtime || !registration || !registration->entry || !ensure_thread_capacity(runtime, runtime->thread_count + 1)) {
        return NULL;
    }
    SThread *thread = sjit_thread_create(
        runtime->next_thread_id++,
        registration->target_id,
        registration->script_id,
        registration->entry,
        registration->script_data);
    if (!thread) {
        return NULL;
    }
    runtime->threads[runtime->thread_count++] = thread;
    if (registration->invocation_count != UINT64_MAX) {
        ++registration->invocation_count;
    }
    return thread;
}

void sjit_scheduler_restart_thread(SRuntime *runtime, int thread_id) {
    if (!runtime) {
        return;
    }
    for (int i = 0; i < runtime->thread_count; ++i) {
        if (runtime->threads[i]->id == thread_id) {
            sjit_thread_restart(runtime->threads[i]);
            SScriptRegistration *registration = find_script(
                runtime, runtime->threads[i]->script_id);
            if (registration && registration->invocation_count != UINT64_MAX) {
                ++registration->invocation_count;
            }
            return;
        }
    }
}

void sjit_scheduler_stop_thread(SRuntime *runtime, int thread_id) {
    if (!runtime) {
        return;
    }
    for (int i = 0; i < runtime->thread_count; ++i) {
        if (runtime->threads[i]->id == thread_id) {
            runtime->threads[i]->status = SJIT_THREAD_KILLED;
            runtime->threads[i]->is_killed = 1;
            return;
        }
    }
}

void sjit_scheduler_stop_for_target(SRuntime *runtime, int target_id, int except_thread_id) {
    if (!runtime) {
        return;
    }
    for (int i = 0; i < runtime->thread_count; ++i) {
        SThread *thread = runtime->threads[i];
        if (thread->target_id == target_id && thread->id != except_thread_id) {
            thread->status = SJIT_THREAD_KILLED;
            thread->is_killed = 1;
        }
    }
}

static int status_from_runtime(SRuntimeStatus status) {
    switch (status) {
    case SJIT_STATUS_OK:
        return SJIT_THREAD_RUNNING;
    case SJIT_STATUS_YIELDED:
        return SJIT_THREAD_YIELD;
    case SJIT_STATUS_YIELD_TICK:
        return SJIT_THREAD_YIELD_TICK;
    case SJIT_STATUS_WAITING:
        return SJIT_THREAD_PROMISE_WAIT;
    case SJIT_STATUS_DONE:
        return SJIT_THREAD_DONE;
    case SJIT_STATUS_ERROR:
    default:
        return SJIT_THREAD_KILLED;
    }
}

static double scheduler_now_ms(void) {
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == TIME_UTC) {
        return ((double)ts.tv_sec * 1000.0) + ((double)ts.tv_nsec / 1000000.0);
    }
    return ((double)clock() * 1000.0) / (double)CLOCKS_PER_SEC;
}

static void commit_thread_result(
    SRuntime *runtime,
    SThread *thread,
    SRuntimeStatus result,
    int *stepped,
    int *active,
    SRuntimeStatus *aggregate) {
    if (!runtime || !thread || !stepped || !active || !aggregate) {
        return;
    }
    thread->is_executing = 0;
    ++*stepped;
    if (thread->restart_pending) {
        /* start-hats may target the thread which is currently running.
           Commit that restart only after its old entry has unwound so neither
           native continuation stores nor this result can overwrite the fresh
           frame.  stop-all still wins over an earlier restart. */
        if (thread->is_killed || runtime->stopped) {
            thread->restart_pending = 0;
            thread->status = SJIT_THREAD_KILLED;
        } else {
            int previous_status = status_from_runtime(result);
            if (result == SJIT_STATUS_YIELDED) {
                previous_status = SJIT_THREAD_RUNNING;
            }
            sjit_thread_restart(thread);
            if (previous_status == SJIT_THREAD_RUNNING) {
                ++*active;
            }
            *aggregate = SJIT_STATUS_YIELDED;
        }
        return;
    }
    thread->status = status_from_runtime(result);
    if (result == SJIT_STATUS_YIELDED) {
        thread->status = SJIT_THREAD_RUNNING;
    }
    if (thread->status == SJIT_THREAD_RUNNING) {
        ++*active;
    }
    if (thread->status == SJIT_THREAD_RUNNING ||
        thread->status == SJIT_THREAD_YIELD_TICK ||
        thread->status == SJIT_THREAD_PROMISE_WAIT) {
        *aggregate = result == SJIT_STATUS_OK ?
            SJIT_STATUS_YIELDED : result;
    }
}

static void run_sequential_step(
    SRuntime *runtime,
    SThread *thread,
    int *stepped,
    int *active,
    SRuntimeStatus *aggregate) {
    thread->is_executing = 1;
    const SRuntimeStatus result =
        thread->entry(runtime, thread, &thread->frame);
    commit_thread_result(
        runtime, thread, result, stepped, active, aggregate);
}

static void run_parallel_step_task(void *opaque, int task_index) {
    SParallelBatchContext *context = (SParallelBatchContext *)opaque;
    SParallelStep *step = &context->steps[task_index];
    step->result = step->thread->entry(
        context->runtime,
        step->thread,
        &step->thread->frame);
}

static void saturating_add_u64(uint64_t *value, uint64_t amount) {
    if (!value) {
        return;
    }
    if (UINT64_MAX - *value < amount) {
        *value = UINT64_MAX;
    } else {
        *value += amount;
    }
}

static void flush_parallel_steps(
    SRuntime *runtime,
    SParallelStep *steps,
    int *pending_count,
    int *stepped,
    int *active,
    SRuntimeStatus *aggregate) {
    if (!runtime || !steps || !pending_count || *pending_count <= 0) {
        return;
    }
    const int count = *pending_count;
    *pending_count = 0;
    if (count == 1) {
        run_sequential_step(
            runtime, steps[0].thread, stepped, active, aggregate);
        return;
    }
    SJitThreadPool *pool = scheduler_thread_pool(runtime);
    if (!pool) {
        for (int i = 0; i < count; ++i) {
            run_sequential_step(
                runtime, steps[i].thread, stepped, active, aggregate);
        }
        return;
    }
    for (int i = 0; i < count; ++i) {
        steps[i].thread->is_executing = 1;
    }
    SParallelBatchContext context = {runtime, steps};
    if (!sjit_thread_pool_run(
            pool, count, run_parallel_step_task, &context)) {
        for (int i = 0; i < count; ++i) {
            steps[i].thread->is_executing = 0;
            run_sequential_step(
                runtime, steps[i].thread, stepped, active, aggregate);
        }
        return;
    }
    saturating_add_u64(&runtime->parallel_batch_count, 1);
    saturating_add_u64(
        &runtime->parallel_task_count, (uint64_t)count);
    if (getenv("SJIT_LOG_THREAD_POOL") != NULL &&
        runtime->parallel_batch_count == 1) {
        fprintf(
            stderr,
            "sjit: ownership-proven parallel batch active tasks=%d\n",
            count);
    }
    /* Completion order is intentionally ignored.  Commit in the original
       thread-vector order to preserve status, aggregate, and cleanup order. */
    for (int i = 0; i < count; ++i) {
        commit_thread_result(
            runtime,
            steps[i].thread,
            steps[i].result,
            stepped,
            active,
            aggregate);
    }
}

SRuntimeStatus sjit_scheduler_tick(SRuntime *runtime) {
    if (!runtime) {
        return SJIT_STATUS_ERROR;
    }
    SRuntimeStatus aggregate = SJIT_STATUS_DONE;
    const double started_ms = scheduler_now_ms();
    const double base_runtime_now_ms = runtime->now_ms;
    const double work_time_ms = 0.75 * runtime->current_step_time_ms;
    double elapsed_ms = 0.0;
    int ran_first_pass = 0;
    int active = runtime->thread_count > 0 ? 1 : 0;
    for (int pass = 0;
         runtime->thread_count > 0 &&
             active > 0 &&
             pass < SJIT_MAX_SCHEDULER_PASSES &&
             elapsed_ms < work_time_ms &&
             (runtime->turbo_mode || !runtime->redraw_requested);
         ++pass) {
        active = 0;
        int stepped = 0;
        const int count_at_start = runtime->thread_count;
        const int have_parallel_steps =
            ensure_parallel_step_capacity(runtime, count_at_start);
        SParallelStep *parallel_steps =
            (SParallelStep *)runtime->parallel_steps;
        int pending_count = 0;
        for (int i = 0; i < count_at_start; ++i) {
            SThread *thread = runtime->threads[i];
            if (!sjit_thread_is_alive(thread)) {
                flush_parallel_steps(
                    runtime,
                    parallel_steps,
                    &pending_count,
                    &stepped,
                    &active,
                    &aggregate);
                continue;
            }
            if (thread->status == SJIT_THREAD_PROMISE_WAIT) {
                flush_parallel_steps(
                    runtime,
                    parallel_steps,
                    &pending_count,
                    &stepped,
                    &active,
                    &aggregate);
                aggregate = SJIT_STATUS_WAITING;
                continue;
            }
            if (thread->status == SJIT_THREAD_YIELD_TICK) {
                if (!ran_first_pass) {
                    thread->status = SJIT_THREAD_RUNNING;
                } else {
                    flush_parallel_steps(
                        runtime,
                        parallel_steps,
                        &pending_count,
                        &stepped,
                        &active,
                        &aggregate);
                    continue;
                }
            }
            if (thread->status == SJIT_THREAD_RUNNING || thread->status == SJIT_THREAD_YIELD) {
                SScriptRegistration *registration =
                    have_parallel_steps ?
                        parallel_registration_for_thread(runtime, thread) :
                        NULL;
                if (registration) {
                    if (parallel_step_conflicts(
                            parallel_steps,
                            pending_count,
                            registration->parallel_owner_target_id,
                            registration->ownership_script)) {
                        flush_parallel_steps(
                            runtime,
                            parallel_steps,
                            &pending_count,
                            &stepped,
                            &active,
                            &aggregate);
                    }
                    SParallelStep *step =
                        &parallel_steps[pending_count++];
                    step->thread = thread;
                    step->result = SJIT_STATUS_ERROR;
                    step->owner_target_id =
                        registration->parallel_owner_target_id;
                    step->script_data = registration->ownership_script;
                } else {
                    flush_parallel_steps(
                        runtime,
                        parallel_steps,
                        &pending_count,
                        &stepped,
                        &active,
                        &aggregate);
                    run_sequential_step(
                        runtime,
                        thread,
                        &stepped,
                        &active,
                        &aggregate);
                }
            } else {
                flush_parallel_steps(
                    runtime,
                    parallel_steps,
                    &pending_count,
                    &stepped,
                    &active,
                    &aggregate);
            }
        }
        flush_parallel_steps(
            runtime,
            parallel_steps,
            &pending_count,
            &stepped,
            &active,
            &aggregate);
        ran_first_pass = 1;
        sjit_runtime_remove_done_threads(runtime);
        if (stepped == 0) {
            break;
        }
        elapsed_ms = scheduler_now_ms() - started_ms;
        if (runtime->turbo_mode) {
            runtime->now_ms = base_runtime_now_ms + elapsed_ms;
        }
    }
    if (runtime->turbo_mode) {
        runtime->now_ms = base_runtime_now_ms + elapsed_ms;
    }
    return aggregate;
}
