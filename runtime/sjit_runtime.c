#include "sjit_runtime.h"

#include "sjit_draw.h"
#include "sjit_ownership_internal.h"
#include "sjit_pen.h"
#include "sjit_scheduler.h"
#include "sjit_string.h"
#include "sjit_thread_pool.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Runtime construction/destruction and scheduling are host-thread operations
   in the current ABI, so this identity source follows the same ownership
   model.  Zero is reserved for an unspecialized compiled script. */
static uint64_t next_runtime_instance_id = 1;

static int runtime_pen_path_logical_count(const SRuntime *runtime) {
    if (!runtime) {
        return 0;
    }
    const int tile_count = runtime->pen_raster_tile.active ?
        runtime->pen_raster_tile.command_count : 0;
    if (tile_count > INT_MAX - runtime->pen.length) {
        return INT_MAX;
    }
    return tile_count + runtime->pen.length;
}

static int ensure_pen_materialized_capacity(SRuntime *runtime, int wanted) {
    if (!runtime || wanted < 0) {
        return 0;
    }
    if (wanted <= runtime->pen_materialized_capacity) {
        return runtime->pen_materialized_items != NULL || wanted == 0;
    }
    int next = runtime->pen_materialized_capacity > 0 ?
        runtime->pen_materialized_capacity : SJIT_INITIAL_CAPACITY;
    while (next < wanted) {
        if (next > INT_MAX / 2) {
            next = wanted;
            break;
        }
        next *= 2;
    }
    if ((size_t)next > SIZE_MAX / sizeof(SDrawCommand)) {
        return 0;
    }
    SDrawCommand *items = (SDrawCommand *)realloc(
        runtime->pen_materialized_items,
        sizeof(SDrawCommand) * (size_t)next);
    if (!items) {
        return 0;
    }
    runtime->pen_materialized_items = items;
    runtime->pen_materialized_capacity = next;
    return 1;
}

static const SDrawCommand *materialize_runtime_pen_path(SRuntime *runtime) {
    if (!runtime || !runtime->pen_raster_tile.active) {
        return runtime ? runtime->pen.items : NULL;
    }
    if (runtime->pen_materialized_valid) {
        return runtime->pen_materialized_items;
    }
    const int count = runtime_pen_path_logical_count(runtime);
    if (!ensure_pen_materialized_capacity(runtime, count)) {
        return NULL;
    }
    const SPenRasterTile *tile = &runtime->pen_raster_tile;
    if (!tile->pixels || !tile->active_bits ||
        tile->width != SJIT_PEN_RASTER_TILE_WIDTH ||
        tile->height != SJIT_PEN_RASTER_TILE_HEIGHT ||
        tile->stride != SJIT_PEN_RASTER_TILE_WIDTH * 4 ||
        tile->rows_filled < 0 || tile->rows_filled > tile->height) {
        return NULL;
    }
    int written = 0;
    for (int row = 0; row < tile->rows_filled; ++row) {
        for (int column = 0; column < tile->width; ++column) {
            const int logical_index = row * tile->width + column;
            if ((tile->active_bits[logical_index >> 3] &
                 (unsigned char)(1u << (logical_index & 7))) == 0) {
                continue;
            }
            const int pixel_y = tile->height - 1 - row;
            const unsigned char *pixel = tile->pixels +
                (size_t)pixel_y * (size_t)tile->stride +
                (size_t)column * 4u;
            SDrawCommand command = {0};
            command.kind = SJIT_DRAW_PEN_STROKE;
            command.target_id = tile->target_id;
            command.x = tile->origin_x + (double)column * tile->step;
            command.y = tile->origin_y + (double)row * tile->step;
            command.x2 = command.x;
            command.y2 = command.y;
            command.pen_width = tile->pen_width;
            command.r = pixel[0];
            command.g = pixel[1];
            command.b = pixel[2];
            command.a = pixel[3];
            command.visible = 1;
            runtime->pen_materialized_items[written++] = command;
        }
    }
    if (written != tile->command_count) {
        return NULL;
    }
    if (runtime->pen.length > 0) {
        memmove(
            runtime->pen_materialized_items + written,
            runtime->pen.items,
            sizeof(SDrawCommand) * (size_t)runtime->pen.length);
    }
    runtime->pen_materialized_valid = 1;
    return runtime->pen_materialized_items;
}

static int ensure_target_capacity(SRuntime *runtime, int wanted) {
    if (!runtime) {
        return 0;
    }
    if (wanted <= runtime->target_capacity) {
        return 1;
    }
    int next = runtime->target_capacity > 0 ? runtime->target_capacity : SJIT_INITIAL_CAPACITY;
    while (next < wanted) {
        next *= 2;
    }
    SSprite **targets = (SSprite **)realloc(runtime->targets, sizeof(SSprite *) * (size_t)next);
    if (!targets) {
        return 0;
    }
    runtime->targets = targets;
    runtime->target_capacity = next;
    return 1;
}

static int ensure_script_capacity(SRuntime *runtime, int wanted) {
    if (!runtime) {
        return 0;
    }
    if (wanted <= runtime->script_capacity) {
        return 1;
    }
    int next = runtime->script_capacity > 0 ? runtime->script_capacity : SJIT_INITIAL_CAPACITY;
    while (next < wanted) {
        next *= 2;
    }
    SScriptRegistration *scripts = (SScriptRegistration *)realloc(
        runtime->scripts,
        sizeof(SScriptRegistration) * (size_t)next);
    if (!scripts) {
        return 0;
    }
    runtime->scripts = scripts;
    runtime->script_capacity = next;
    return 1;
}

static int ensure_variable_monitor_capacity(SRuntime *runtime, int wanted) {
    if (!runtime) {
        return 0;
    }
    if (wanted <= runtime->variable_monitor_capacity) {
        return 1;
    }
    int next = runtime->variable_monitor_capacity > 0 ?
        runtime->variable_monitor_capacity : SJIT_INITIAL_CAPACITY;
    while (next < wanted) {
        next *= 2;
    }
    SVariableMonitor *monitors = (SVariableMonitor *)realloc(
        runtime->variable_monitors,
        sizeof(SVariableMonitor) * (size_t)next);
    if (!monitors) {
        return 0;
    }
    runtime->variable_monitors = monitors;
    runtime->variable_monitor_capacity = next;
    return 1;
}

SRuntime *sjit_runtime_create(void) {
    SRuntime *runtime = (SRuntime *)calloc(1, sizeof(SRuntime));
    if (!runtime) {
        return NULL;
    }
    runtime->now_ms = 0.0;
    runtime->delta_ms = 0.0;
    runtime->next_target_id = 1;
    runtime->next_drawable_id = 1;
    runtime->next_thread_id = 1;
    runtime->timer_start_ms = 0.0;
    runtime->current_step_time_ms = 1000.0 / 60.0;
    runtime->turbo_mode = 0;
    runtime->instance_id = next_runtime_instance_id;
    if (next_runtime_instance_id != 0) {
        ++next_runtime_instance_id;
    }
    sjit_draw_buffer_init(&runtime->draw);
    sjit_pen_path_init(&runtime->pen);
    return runtime;
}

void sjit_runtime_destroy(SRuntime *runtime) {
    if (!runtime) {
        return;
    }
    sjit_thread_pool_destroy((SJitThreadPool *)runtime->thread_pool);
    runtime->thread_pool = NULL;
    for (int i = 0; i < runtime->thread_count; ++i) {
        sjit_thread_destroy(runtime->threads[i]);
    }
    for (int i = 0; i < runtime->target_count; ++i) {
        sjit_sprite_destroy(runtime->targets[i]);
    }
    for (int i = 0; i < runtime->script_count; ++i) {
        sjit_string_destroy(runtime->scripts[i].match_value);
        free(runtime->scripts[i].ownership_procedure_entries);
        free(runtime->scripts[i].ownership_variable_indices);
    }
    for (int i = 0; i < runtime->variable_monitor_count; ++i) {
        sjit_string_destroy(runtime->variable_monitors[i].id);
        sjit_string_destroy(runtime->variable_monitors[i].label);
    }
    free(runtime->threads);
    free(runtime->targets);
    free(runtime->scripts);
    free(runtime->variable_monitors);
    free(runtime->draw_owned_items);
    free(runtime->pen_raster_tile.pixels);
    free(runtime->pen_raster_tile.active_bits);
    free(runtime->pen_materialized_items);
    free(runtime->parallel_steps);
    sjit_pen_path_destroy(&runtime->pen);
    free(runtime->pen_color_cache);
    free(runtime);
}

int sjit_runtime_load_project(SRuntime *runtime, const SProject *project) {
    (void)runtime;
    (void)project;
    return 1;
}

void sjit_runtime_green_flag(SRuntime *runtime) {
    if (!runtime) {
        return;
    }
    sjit_runtime_stop_all(runtime);
    sjit_runtime_remove_done_threads(runtime);
    runtime->timer_start_ms = runtime->now_ms;
    sjit_runtime_start_hats(runtime, SJIT_HAT_EVENT_WHENFLAGCLICKED, NULL);
}

void sjit_runtime_stop_all(SRuntime *runtime) {
    if (!runtime) {
        return;
    }
    for (int i = 0; i < runtime->thread_count; ++i) {
        runtime->threads[i]->status = SJIT_THREAD_KILLED;
        runtime->threads[i]->is_killed = 1;
    }
    runtime->stopped = 1;
}

static void sjit_runtime_start_input_hats(SRuntime *runtime) {
    if (!runtime) {
        return;
    }
    for (int key = 0; key < 256; ++key) {
        if (!runtime->input.key_pressed_edge[key]) {
            continue;
        }
        char key_match[32];
        const char *name = NULL;
        if (key == SJIT_KEY_UP_ARROW) {
            name = "up arrow";
        } else if (key == SJIT_KEY_DOWN_ARROW) {
            name = "down arrow";
        } else if (key == SJIT_KEY_RIGHT_ARROW) {
            name = "right arrow";
        } else if (key == SJIT_KEY_LEFT_ARROW) {
            name = "left arrow";
        } else if (key == ' ') {
            name = "space";
        } else if (key >= 0 && key < 128 && isprint((unsigned char)key)) {
            key_match[0] = (char)tolower((unsigned char)key);
            key_match[1] = '\0';
            name = key_match;
        }
        if (name) {
            sjit_runtime_start_hats(runtime, SJIT_HAT_EVENT_WHENKEYPRESSED, name);
        }
        sjit_runtime_start_hats(runtime, SJIT_HAT_EVENT_WHENKEYPRESSED, "any");
    }
    if (runtime->input.stage_clicked) {
        sjit_runtime_start_hats(runtime, SJIT_HAT_EVENT_WHENSTAGECLICKED, NULL);
    }
    if (runtime->input.sprite_clicked_id > 0) {
        char target_match[32];
        snprintf(target_match, sizeof(target_match), "%d", runtime->input.sprite_clicked_id);
        sjit_runtime_start_hats(runtime, SJIT_HAT_EVENT_WHENTHISSPRITECLICKED, target_match);
    }
}

SRuntimeStatus sjit_runtime_tick(SRuntime *runtime) {
    if (!runtime) {
        return SJIT_STATUS_ERROR;
    }
    runtime->stopped = 0;
    sjit_runtime_remove_done_threads(runtime);
    sjit_runtime_start_input_hats(runtime);
    runtime->draw.items = runtime->draw_owned_items;
    runtime->draw.capacity = runtime->draw_owned_capacity;
    sjit_draw_buffer_clear(&runtime->draw);
    const SRuntimeStatus status = sjit_scheduler_tick(runtime);
    sjit_runtime_remove_done_threads(runtime);
    int visible_targets = 0;
    for (int i = 0; i < runtime->target_count; ++i) {
        if (runtime->targets[i]->visible) {
            ++visible_targets;
        }
    }
    if (runtime->pen_raster_tile.active) {
        SDrawCommand tile_command = {0};
        tile_command.kind = SJIT_DRAW_PEN_RASTER_TILE;
        tile_command.target_id = runtime->pen_raster_tile.target_id;
        tile_command.visible = 1;
        int compact_draw = sjit_draw_push(&runtime->draw, tile_command);
        if (compact_draw && runtime->pen.length > 0) {
            const SDrawCommandBuffer tail = {
                runtime->pen.items,
                runtime->pen.length,
                runtime->pen.capacity};
            compact_draw = sjit_draw_append(&runtime->draw, &tail);
        }
        if (compact_draw) {
            runtime->draw_pen_length = 1 + runtime->pen.length;
        } else {
            sjit_draw_buffer_clear(&runtime->draw);
            const int logical_count = runtime_pen_path_logical_count(runtime);
            const SDrawCommand *materialized =
                materialize_runtime_pen_path(runtime);
            const SDrawCommandBuffer source = {
                (SDrawCommand *)materialized,
                materialized ? logical_count : 0,
                materialized ? logical_count : 0};
            const int copied_pen = sjit_draw_append(&runtime->draw, &source);
            runtime->draw_pen_length = copied_pen ? logical_count : 0;
        }
        runtime->draw_pen_revision = runtime->pen.revision;
    } else if (runtime->pen.length <= runtime->pen.capacity &&
               visible_targets <= runtime->pen.capacity - runtime->pen.length) {
        runtime->draw.items = runtime->pen.items;
        runtime->draw.length = runtime->pen.length;
        runtime->draw.capacity = runtime->pen.capacity;
        runtime->draw_pen_revision = runtime->pen.revision;
        runtime->draw_pen_length = runtime->pen.length;
    } else {
        const int copied_pen = sjit_pen_path_append_draw(&runtime->draw, &runtime->pen);
        runtime->draw_owned_items = runtime->draw.items;
        runtime->draw_owned_capacity = runtime->draw.capacity;
        runtime->draw_pen_revision = runtime->pen.revision;
        runtime->draw_pen_length = copied_pen ? runtime->pen.length : 0;
    }
    for (int i = 0; i < runtime->target_count; ++i) {
        if (runtime->targets[i]->visible) {
            sjit_draw_push_sprite(&runtime->draw, runtime->targets[i]);
        }
    }
    if (runtime->draw.items != runtime->pen.items) {
        runtime->draw_owned_items = runtime->draw.items;
        runtime->draw_owned_capacity = runtime->draw.capacity;
    }
    runtime->redraw_requested = 0;
    return status;
}

void sjit_runtime_set_input(SRuntime *runtime, const SHostInputSnapshot *input) {
    if (runtime && input) {
        runtime->input = *input;
    }
}

void sjit_runtime_set_time(SRuntime *runtime, double now_ms, double delta_ms) {
    if (!runtime) {
        return;
    }
    runtime->now_ms = now_ms;
    runtime->delta_ms = delta_ms;
}

void sjit_runtime_set_turbo_mode(SRuntime *runtime, int enabled) {
    if (runtime) {
        runtime->turbo_mode = enabled ? 1 : 0;
    }
}

void sjit_runtime_set_current_step_time(SRuntime *runtime, double step_time_ms) {
    if (!runtime) {
        return;
    }
    runtime->current_step_time_ms = step_time_ms > 0.0 ? step_time_ms : 1000.0 / 60.0;
}

const SDrawCommandBuffer *sjit_runtime_get_draw_commands(SRuntime *runtime) {
    return runtime ? &runtime->draw : NULL;
}

void sjit_runtime_clear_draw_commands(SRuntime *runtime) {
    if (runtime) {
        sjit_draw_buffer_clear(&runtime->draw);
    }
}

const SDrawCommand *sjit_runtime_pen_path_data(const SRuntime *runtime) {
    return materialize_runtime_pen_path((SRuntime *)runtime);
}

int sjit_runtime_pen_path_count(const SRuntime *runtime) {
    return runtime_pen_path_logical_count(runtime);
}

int sjit_runtime_pen_path_revision(const SRuntime *runtime) {
    return runtime ? runtime->pen.revision : 0;
}

void sjit_runtime_request_redraw(SRuntime *runtime) {
    if (runtime) {
        runtime->redraw_requested = 1;
    }
}

SSprite *sjit_runtime_create_sprite(SRuntime *runtime, const char *name, int is_stage) {
    if (!runtime) {
        return NULL;
    }
    SSprite *sprite = sjit_sprite_create(runtime->next_target_id++, runtime->next_drawable_id++, name, is_stage);
    if (!sprite) {
        return NULL;
    }
    if (!sjit_runtime_add_sprite(runtime, sprite)) {
        sjit_sprite_destroy(sprite);
        return NULL;
    }
    return sprite;
}

int sjit_runtime_add_sprite(SRuntime *runtime, SSprite *sprite) {
    if (!runtime || !sprite || !ensure_target_capacity(runtime, runtime->target_count + 1)) {
        return 0;
    }
    runtime->targets[runtime->target_count++] = sprite;
    return 1;
}

SSprite *sjit_runtime_get_sprite(SRuntime *runtime, int target_id) {
    if (!runtime) {
        return NULL;
    }
    for (int i = 0; i < runtime->target_count; ++i) {
        if (runtime->targets[i]->base.id == target_id) {
            return runtime->targets[i];
        }
    }
    return NULL;
}

SSprite *sjit_runtime_get_sprite_by_name(SRuntime *runtime, const char *name) {
    if (!runtime) {
        return NULL;
    }
    for (int i = 0; i < runtime->target_count; ++i) {
        if (sjit_string_equals_ignore_case(runtime->targets[i]->base.name, name ? name : "")) {
            return runtime->targets[i];
        }
    }
    return NULL;
}

int sjit_runtime_register_script(
    SRuntime *runtime,
    int target_id,
    int script_id,
    int opcode_id,
    const char *match_value,
    int restart_existing_threads,
    int edge_activated,
    SScriptEntryFn entry) {
    return sjit_runtime_register_script_with_data(
        runtime,
        target_id,
        script_id,
        opcode_id,
        match_value,
        restart_existing_threads,
        edge_activated,
        entry,
        NULL);
}

int sjit_runtime_register_script_with_data(
    SRuntime *runtime,
    int target_id,
    int script_id,
    int opcode_id,
    const char *match_value,
    int restart_existing_threads,
    int edge_activated,
    SScriptEntryFn entry,
    void *script_data) {
    if (!runtime || !entry || !ensure_script_capacity(runtime, runtime->script_count + 1)) {
        return 0;
    }
    SScriptRegistration *registration = &runtime->scripts[runtime->script_count++];
    registration->target_id = target_id;
    registration->script_id = script_id;
    registration->opcode_id = opcode_id;
    registration->match_value = sjit_string_new(match_value ? match_value : "");
    registration->restart_existing_threads = restart_existing_threads;
    registration->edge_activated = edge_activated;
    registration->entry = entry;
    registration->script_data = script_data;
    registration->invocation_count = 0;
    registration->ownership_analyzed = 0;
    registration->parallel_safe = 0;
    registration->parallel_owner_target_id = 0;
    registration->ownership_reject_flags =
        SJIT_OWNERSHIP_REJECT_INVALID_REGISTRATION;
    registration->ownership_script = NULL;
    registration->ownership_entry = NULL;
    registration->ownership_procedure_entries = NULL;
    registration->ownership_procedure_count = 0;
    registration->ownership_variable_indices = NULL;
    registration->ownership_variable_count = 0;
    return 1;
}

static void clear_ownership_certificate(
    SScriptRegistration *registration) {
    if (!registration) {
        return;
    }
    free(registration->ownership_procedure_entries);
    registration->ownership_procedure_entries = NULL;
    registration->ownership_procedure_count = 0;
    free(registration->ownership_variable_indices);
    registration->ownership_variable_indices = NULL;
    registration->ownership_variable_count = 0;
    registration->ownership_entry = NULL;
    registration->ownership_script = NULL;
    registration->parallel_safe = 0;
    registration->parallel_owner_target_id = 0;
}

static void reject_registration_ownership(
    SRuntime *runtime,
    int script_id) {
    if (!runtime) {
        return;
    }
    for (int i = 0; i < runtime->script_count; ++i) {
        if (runtime->scripts[i].script_id != script_id) {
            continue;
        }
        clear_ownership_certificate(&runtime->scripts[i]);
        runtime->scripts[i].ownership_analyzed = 1;
        runtime->scripts[i].ownership_reject_flags =
            SJIT_OWNERSHIP_REJECT_INVALID_REGISTRATION;
    }
}

static int record_script_ownership(
    SRuntime *runtime,
    int script_id,
    const struct SCompiledScript *script,
    SScriptEntryFn entry,
    int native_attested) {
    if (!runtime) {
        return 0;
    }
    SScriptRegistration *registration = NULL;
    int matches = 0;
    for (int i = 0; i < runtime->script_count; ++i) {
        if (runtime->scripts[i].script_id == script_id) {
            registration = &runtime->scripts[i];
            ++matches;
        }
    }
    if (matches != 1 || !registration || !script ||
        registration->script_data != script ||
        registration->target_id != script->target_id ||
        registration->entry != entry ||
        script->procedure_count < 0 ||
        (script->procedure_count > 0 && !script->procedures) ||
        registration->opcode_id == SJIT_HAT_CONTROL_START_AS_CLONE) {
        reject_registration_ownership(runtime, script_id);
        return 0;
    }
    if (!native_attested) {
        if (entry != sjit_script_interpreter_entry) {
            reject_registration_ownership(runtime, script_id);
            return 0;
        }
        for (int i = 0; i < script->procedure_count; ++i) {
            if (script->procedures[i].jit_entry) {
                reject_registration_ownership(runtime, script_id);
                return 0;
            }
        }
    }
    clear_ownership_certificate(registration);
    SOwnershipManifest manifest =
        sjit_analyze_script_ownership_manifest(runtime, script);
    const SOwnershipAnalysis analysis = manifest.analysis;
    SProcedureEntryFn *procedure_entries = NULL;
    if (analysis.parallel_safe && script->procedure_count > 0) {
        procedure_entries = (SProcedureEntryFn *)malloc(
            sizeof(SProcedureEntryFn) * (size_t)script->procedure_count);
        if (!procedure_entries) {
            sjit_ownership_manifest_destroy(&manifest);
            registration->ownership_analyzed = 1;
            registration->ownership_reject_flags =
                SJIT_OWNERSHIP_REJECT_INVALID_TREE;
            return 0;
        }
        for (int i = 0; i < script->procedure_count; ++i) {
            procedure_entries[i] = script->procedures[i].jit_entry;
        }
    }
    registration->ownership_analyzed = 1;
    registration->parallel_safe = analysis.parallel_safe;
    registration->parallel_owner_target_id = analysis.owner_target_id;
    registration->ownership_reject_flags = analysis.reject_flags;
    registration->ownership_script = analysis.parallel_safe ? script : NULL;
    registration->ownership_entry = analysis.parallel_safe ? entry : NULL;
    registration->ownership_procedure_entries = procedure_entries;
    registration->ownership_procedure_count =
        analysis.parallel_safe ? script->procedure_count : 0;
    registration->ownership_variable_indices =
        analysis.parallel_safe ? manifest.owned_variable_indices : NULL;
    registration->ownership_variable_count =
        analysis.parallel_safe ? manifest.owned_variable_count : 0;
    manifest.owned_variable_indices = NULL;
    manifest.owned_variable_count = 0;
    sjit_ownership_manifest_destroy(&manifest);
    if (getenv("SJIT_LOG_OWNERSHIP_ANALYSIS") != NULL) {
        fprintf(
            stderr,
            "sjit: ownership script=%d target=%d parallel=%d reject=0x%llx\n",
            script_id,
            registration->target_id,
            registration->parallel_safe,
            (unsigned long long)registration->ownership_reject_flags);
    }
    return registration->parallel_safe;
}

int sjit_runtime_analyze_script_ownership(
    SRuntime *runtime,
    int script_id,
    const struct SCompiledScript *script) {
    return record_script_ownership(
        runtime,
        script_id,
        script,
        sjit_script_interpreter_entry,
        0);
}

int sjit_runtime_record_attested_script_ownership(
    SRuntime *runtime,
    int script_id,
    const SCompiledScript *script,
    SScriptEntryFn entry) {
    return record_script_ownership(
        runtime, script_id, script, entry, 1);
}

int sjit_runtime_script_parallel_safe(
    const SRuntime *runtime,
    int script_id) {
    if (!runtime) {
        return 0;
    }
    const SScriptRegistration *match = NULL;
    for (int i = 0; i < runtime->script_count; ++i) {
        if (runtime->scripts[i].script_id != script_id) {
            continue;
        }
        if (match) {
            return 0;
        }
        match = &runtime->scripts[i];
    }
    return match && match->ownership_analyzed && match->parallel_safe;
}

uint64_t sjit_runtime_script_ownership_reject_flags(
    const SRuntime *runtime,
    int script_id) {
    if (!runtime) {
        return SJIT_OWNERSHIP_REJECT_INVALID_REGISTRATION;
    }
    const SScriptRegistration *match = NULL;
    for (int i = 0; i < runtime->script_count; ++i) {
        if (runtime->scripts[i].script_id != script_id) {
            continue;
        }
        if (match) {
            return SJIT_OWNERSHIP_REJECT_INVALID_REGISTRATION;
        }
        match = &runtime->scripts[i];
    }
    return match ? match->ownership_reject_flags :
        SJIT_OWNERSHIP_REJECT_INVALID_REGISTRATION;
}

int sjit_runtime_thread_pool_parallelism(const SRuntime *runtime) {
    return runtime ? runtime->thread_pool_parallelism : 0;
}

uint64_t sjit_runtime_parallel_batch_count(const SRuntime *runtime) {
    return runtime ? runtime->parallel_batch_count : 0;
}

uint64_t sjit_runtime_parallel_task_count(const SRuntime *runtime) {
    return runtime ? runtime->parallel_task_count : 0;
}

static int registration_matches(const SScriptRegistration *registration, int opcode_id, const char *match_value) {
    if (!registration || registration->opcode_id != opcode_id) {
        return 0;
    }
    if (!match_value) {
        return 1;
    }
    return sjit_string_equals_ignore_case(registration->match_value, match_value);
}

static SThread *find_thread_for_script(SRuntime *runtime, const SScriptRegistration *registration) {
    if (!runtime || !registration) {
        return NULL;
    }
    for (int i = 0; i < runtime->thread_count; ++i) {
        SThread *thread = runtime->threads[i];
        if (sjit_thread_is_alive(thread) &&
            thread->target_id == registration->target_id &&
            thread->script_id == registration->script_id) {
            return thread;
        }
    }
    return NULL;
}

int sjit_runtime_start_hats(SRuntime *runtime, int opcode_id, const char *match_value) {
    if (!runtime) {
        return 0;
    }
    int started = 0;
    for (int i = runtime->script_count - 1; i >= 0; --i) {
        SScriptRegistration *registration = &runtime->scripts[i];
        if (!registration_matches(registration, opcode_id, match_value)) {
            continue;
        }
        SThread *existing = find_thread_for_script(runtime, registration);
        if (existing && registration->restart_existing_threads) {
            sjit_thread_restart(existing);
            if (registration->invocation_count != UINT64_MAX) {
                ++registration->invocation_count;
            }
            ++started;
        } else if (!existing) {
            if (sjit_scheduler_start_script(runtime, registration)) {
                ++started;
            }
        }
    }
    return started;
}

int sjit_runtime_count_threads_in_id_range(SRuntime *runtime, int begin_id, int count) {
    if (!runtime || begin_id < 0 || count <= 0) {
        return 0;
    }
    const int end_id = begin_id + count;
    int alive = 0;
    for (int i = 0; i < runtime->thread_count; ++i) {
        SThread *thread = runtime->threads[i];
        if (thread->id >= begin_id && thread->id < end_id && sjit_thread_is_alive(thread)) {
            ++alive;
        }
    }
    return alive;
}

int sjit_runtime_next_thread_id(const SRuntime *runtime) {
    return runtime ? runtime->next_thread_id : 0;
}

int sjit_runtime_has_threads(const SRuntime *runtime) {
    return runtime && runtime->thread_count > 0;
}

int sjit_runtime_thread_count(const SRuntime *runtime) {
    return runtime ? runtime->thread_count : 0;
}

uint64_t sjit_runtime_script_invocation_count(const SRuntime *runtime, int script_id) {
    if (!runtime) {
        return 0;
    }
    uint64_t total = 0;
    for (int i = 0; i < runtime->script_count; ++i) {
        const SScriptRegistration *registration = &runtime->scripts[i];
        if (registration->script_id != script_id) {
            continue;
        }
        if (UINT64_MAX - total < registration->invocation_count) {
            return UINT64_MAX;
        }
        total += registration->invocation_count;
    }
    return total;
}

void sjit_runtime_remove_done_threads(SRuntime *runtime) {
    if (!runtime) {
        return;
    }
    int write = 0;
    for (int read = 0; read < runtime->thread_count; ++read) {
        SThread *thread = runtime->threads[read];
        if (!sjit_thread_is_alive(thread)) {
            sjit_thread_destroy(thread);
            continue;
        }
        runtime->threads[write++] = thread;
    }
    runtime->thread_count = write;
}

SVariable *sjit_runtime_lookup_or_create_variable(
    SRuntime *runtime,
    int current_target_id,
    const char *name,
    int type) {
    return sjit_runtime_lookup_or_create_variable_by_scratch_id(
        runtime, current_target_id, NULL, name, type);
}

SVariable *sjit_runtime_lookup_variable_by_scratch_id(
    SRuntime *runtime,
    int current_target_id,
    const char *scratch_id,
    const char *name,
    int type) {
    if (!runtime) {
        return NULL;
    }

    SSprite *current = sjit_runtime_get_sprite(runtime, current_target_id);
    if (current) {
        SVariable *variable = sjit_target_lookup_variable_by_scratch_id(
            &current->base, scratch_id, name, type);
        if (variable) {
            return variable;
        }
    }

    for (int i = 0; i < runtime->target_count; ++i) {
        SSprite *target = runtime->targets[i];
        if (target->base.is_stage) {
            SVariable *variable = sjit_target_lookup_variable_by_scratch_id(
                &target->base, scratch_id, name, type);
            if (variable) {
                return variable;
            }
        }
    }

    return NULL;
}

SVariable *sjit_runtime_lookup_or_create_variable_by_scratch_id(
    SRuntime *runtime,
    int current_target_id,
    const char *scratch_id,
    const char *name,
    int type) {
    SVariable *variable = sjit_runtime_lookup_variable_by_scratch_id(
        runtime, current_target_id, scratch_id, name, type);
    if (variable) {
        return variable;
    }

    if (!runtime) {
        return NULL;
    }
    SSprite *current = sjit_runtime_get_sprite(runtime, current_target_id);
    if (current) {
        return sjit_target_lookup_or_create_variable_by_scratch_id(
            &current->base, scratch_id, name, type);
    }
    if (runtime->target_count > 0) {
        return sjit_target_lookup_or_create_variable_by_scratch_id(
            &runtime->targets[0]->base, scratch_id, name, type);
    }
    return NULL;
}

SVariableMonitor *sjit_runtime_lookup_variable_monitor(SRuntime *runtime, const char *id) {
    if (!runtime || !id || id[0] == '\0') {
        return NULL;
    }
    for (int i = 0; i < runtime->variable_monitor_count; ++i) {
        SVariableMonitor *monitor = &runtime->variable_monitors[i];
        if (strcmp(sjit_string_cstr(monitor->id), id) == 0) {
            return monitor;
        }
    }
    return NULL;
}

SVariableMonitor *sjit_runtime_register_variable_monitor(
    SRuntime *runtime,
    const char *id,
    const char *label,
    int target_id,
    int variable_index,
    int variable_type) {
    if (!runtime || !id || id[0] == '\0' || target_id <= 0 || variable_index < 0) {
        return NULL;
    }
    SVariableMonitor *monitor = sjit_runtime_lookup_variable_monitor(runtime, id);
    if (!monitor) {
        if (!ensure_variable_monitor_capacity(runtime, runtime->variable_monitor_count + 1)) {
            return NULL;
        }
        monitor = &runtime->variable_monitors[runtime->variable_monitor_count++];
        memset(monitor, 0, sizeof(*monitor));
        monitor->id = sjit_string_new(id);
        monitor->label = sjit_string_new(label ? label : "");
        monitor->mode = variable_type == SJIT_VAR_LIST ?
            SJIT_MONITOR_MODE_LIST : SJIT_MONITOR_MODE_DEFAULT;
        monitor->slider_min = 0.0;
        monitor->slider_max = 100.0;
        monitor->is_discrete = 1;
    } else if (strcmp(sjit_string_cstr(monitor->label), label ? label : "") != 0) {
        sjit_string_destroy(monitor->label);
        monitor->label = sjit_string_new(label ? label : "");
    }
    monitor->target_id = target_id;
    monitor->variable_index = variable_index;
    monitor->variable_type = variable_type;
    return monitor;
}

int sjit_runtime_configure_variable_monitor(
    SRuntime *runtime,
    const char *id,
    int visible,
    int mode,
    double x,
    double y,
    double width,
    double height,
    double slider_min,
    double slider_max,
    int is_discrete) {
    SVariableMonitor *monitor = sjit_runtime_lookup_variable_monitor(runtime, id);
    if (!monitor) {
        return 0;
    }
    monitor->visible = visible ? 1 : 0;
    monitor->mode = mode;
    monitor->x = x;
    monitor->y = y;
    monitor->width = width;
    monitor->height = height;
    monitor->slider_min = slider_min;
    monitor->slider_max = slider_max;
    monitor->is_discrete = is_discrete ? 1 : 0;
    return 1;
}

int sjit_runtime_set_variable_monitor_visible(SRuntime *runtime, const char *id, int visible) {
    SVariableMonitor *monitor = sjit_runtime_lookup_variable_monitor(runtime, id);
    if (!monitor) {
        return 0;
    }
    const int next_visible = visible ? 1 : 0;
    if (monitor->visible != next_visible) {
        monitor->visible = next_visible;
        sjit_runtime_request_redraw(runtime);
    }
    return 1;
}

int sjit_runtime_variable_monitor_count(const SRuntime *runtime) {
    return runtime ? runtime->variable_monitor_count : 0;
}

const SVariableMonitor *sjit_runtime_variable_monitor_at(const SRuntime *runtime, int index) {
    if (!runtime || index < 0 || index >= runtime->variable_monitor_count) {
        return NULL;
    }
    return &runtime->variable_monitors[index];
}

SVariable *sjit_runtime_variable_for_monitor(SRuntime *runtime, const SVariableMonitor *monitor) {
    if (!runtime || !monitor) {
        return NULL;
    }
    SSprite *target = sjit_runtime_get_sprite(runtime, monitor->target_id);
    if (!target || monitor->variable_index < 0 ||
        monitor->variable_index >= target->base.variable_count) {
        return NULL;
    }
    SVariable *variable = &target->base.variables[monitor->variable_index];
    return variable->type == monitor->variable_type ? variable : NULL;
}
