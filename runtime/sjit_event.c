#include "sjit_event.h"

int sjit_event_broadcast(SRuntime *runtime, const char *message) {
    return sjit_runtime_start_hats(runtime, SJIT_HAT_EVENT_WHENBROADCASTRECEIVED, message ? message : "");
}

SRuntimeStatus sjit_event_broadcast_and_wait(SRuntime *runtime, SFrame *frame, const char *message, int resume_pc) {
    if (!runtime || !frame) {
        return SJIT_STATUS_ERROR;
    }
    if (frame->started_thread_count < 0) {
        const int begin_id = sjit_runtime_next_thread_id(runtime);
        const int count = sjit_event_broadcast(runtime, message);
        frame->started_thread_begin = begin_id;
        frame->started_thread_count = count;
        frame->pc = resume_pc;
        if (count == 0) {
            frame->started_thread_begin = -1;
            frame->started_thread_count = -1;
            return SJIT_STATUS_OK;
        }
        return SJIT_STATUS_YIELDED;
    }
    if (sjit_runtime_count_threads_in_id_range(runtime, frame->started_thread_begin, frame->started_thread_count) > 0) {
        frame->pc = resume_pc;
        return SJIT_STATUS_YIELDED;
    }
    frame->started_thread_begin = -1;
    frame->started_thread_count = -1;
    return SJIT_STATUS_OK;
}
