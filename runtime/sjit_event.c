#include "sjit_event.h"

#include "sjit_number.h"
#include "sjit_script.h"
#include "sjit_sensing.h"
#include "sjit_value.h"

#include <stdlib.h>
#include <string.h>

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

static int edge_greater_truth(
    SRuntime *runtime,
    const SScriptRegistration *registration) {
    if (!runtime || !registration || !registration->match_value) {
        return 0;
    }
    const char *encoded = sjit_string_cstr(registration->match_value);
    const char *separator = strchr(encoded, '|');
    if (!separator || separator == encoded) {
        return 0;
    }
    char menu[32];
    const size_t menu_length = (size_t)(separator - encoded);
    if (menu_length >= sizeof(menu)) {
        return 0;
    }
    memcpy(menu, encoded, menu_length);
    menu[menu_length] = '\0';
    double threshold = 0.0;
    char *end = NULL;
    SCompiledScript *script = registration->script_data ?
        (SCompiledScript *)registration->script_data : NULL;
    if (script && script->hat_edge_value) {
        SValue value = sjit_script_eval_expr(runtime, script, script->hat_edge_value);
        threshold = sjit_to_number_fast(runtime, value);
        sjit_value_destroy_fast(value);
    } else {
        threshold = strtod(separator + 1, &end);
        if (!end || *end != '\0') {
            return 0;
        }
    }
    double current = 0.0;
    if (strcmp(menu, "timer") == 0) {
        current = (runtime->now_ms - runtime->timer_start_ms) / 1000.0;
    } else if (strcmp(menu, "loudness") == 0) {
        if (runtime->loudness < 0.0) {
            return 0;
        }
        current = runtime->loudness;
    } else {
        return 0;
    }
    return current > threshold;
}

void sjit_event_poll_edge_hats(SRuntime *runtime) {
    if (!runtime) {
        return;
    }
    for (int i = 0; i < runtime->script_count; ++i) {
        SScriptRegistration *registration = &runtime->scripts[i];
        if (!registration->edge_activated) {
            continue;
        }
        SSprite *sprite = sjit_runtime_get_sprite(runtime, registration->target_id);
        int truth = 0;
        if (registration->opcode_id == SJIT_HAT_EVENT_WHENTOUCHINGOBJECT) {
            SCompiledScript *script = registration->script_data ?
                (SCompiledScript *)registration->script_data : NULL;
            SValue object = script && script->hat_edge_value ?
                sjit_script_eval_expr(runtime, script, script->hat_edge_value) :
                sjit_make_string(sjit_string_cstr(registration->match_value));
            truth = sjit_sensing_touching_object(runtime, sprite, object);
            sjit_value_destroy_fast(object);
        } else if (registration->opcode_id == SJIT_HAT_EVENT_WHENGREATERTHAN) {
            truth = edge_greater_truth(runtime, registration);
        }
        if (!registration->edge_initialized) {
            registration->edge_initialized = 1;
            registration->edge_last_truth = truth;
            continue;
        }
        if (truth && !registration->edge_last_truth) {
            sjit_runtime_start_edge_hat(runtime, registration);
        }
        registration->edge_last_truth = truth;
    }
}
