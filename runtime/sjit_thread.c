#include "sjit_thread.h"

#include "sjit_frame.h"

#include <stdlib.h>

SThread *sjit_thread_create(int id, int target_id, int script_id, SScriptEntryFn entry, void *script_data) {
    SThread *thread = (SThread *)calloc(1, sizeof(SThread));
    if (!thread) {
        return NULL;
    }
    thread->id = id;
    thread->target_id = target_id;
    thread->script_id = script_id;
    thread->status = SJIT_THREAD_RUNNING;
    thread->is_killed = 0;
    thread->entry = entry;
    thread->script_data = script_data;
    sjit_frame_init(&thread->frame);
    return thread;
}

void sjit_thread_destroy(SThread *thread) {
    free(thread);
}

void sjit_thread_restart(SThread *thread) {
    if (!thread) {
        return;
    }
    if (thread->is_executing) {
        thread->restart_pending = 1;
        return;
    }
    thread->restart_pending = 0;
    thread->status = SJIT_THREAD_RUNNING;
    thread->is_killed = 0;
    sjit_frame_reset(&thread->frame);
}

int sjit_thread_is_alive(const SThread *thread) {
    return thread &&
        !thread->is_killed &&
        thread->status != SJIT_THREAD_DONE &&
        thread->status != SJIT_THREAD_KILLED;
}
