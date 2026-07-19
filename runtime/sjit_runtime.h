#ifndef SJIT_RUNTIME_H
#define SJIT_RUNTIME_H

#include "sjit_abi.h"
#include "sjit_sprite.h"
#include "sjit_thread.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int target_id;
    int script_id;
    int opcode_id;
    SString *match_value;
    int restart_existing_threads;
    int edge_activated;
    SScriptEntryFn entry;
    void *script_data;
    /* Scheduler-owned, non-atomic hotness metadata.  It is updated once when
       a script thread starts or restarts, never on JIT scheduler re-entry. */
    uint64_t invocation_count;
    /* Populated only by the compiled-script ownership pass.  A zero
       parallel_safe value is the fail-closed default for registrations made
       through the generic C API. */
    int ownership_analyzed;
    int parallel_safe;
    int parallel_owner_target_id;
    uint64_t ownership_reject_flags;
    const struct SCompiledScript *ownership_script;
    /* Exact entry snapshot captured by the interpreter analyzer or by an
       attesting JitEngine.  Scheduler validation rejects later pointer swaps. */
    SScriptEntryFn ownership_entry;
    SProcedureEntryFn *ownership_procedure_entries;
    int ownership_procedure_count;
    /* Stable append-only indices of the owner-local scalars reached by the
       immutable certified AST.  Scheduler revalidation is O(this count). */
    int *ownership_variable_indices;
    int ownership_variable_count;
} SScriptRegistration;

typedef enum {
    SJIT_MONITOR_MODE_DEFAULT = 0,
    SJIT_MONITOR_MODE_LARGE = 1,
    SJIT_MONITOR_MODE_SLIDER = 2,
    SJIT_MONITOR_MODE_LIST = 3
} SVariableMonitorMode;

typedef enum {
    SJIT_COMPATIBILITY_MODE_SCRATCH = 0,
    SJIT_COMPATIBILITY_MODE_TURBOWARP = 1
} SJITCompatibilityMode;

typedef struct {
    SString *id;
    SString *label;
    int target_id;
    int variable_index;
    int variable_type;
    int visible;
    int mode;
    double x;
    double y;
    double width;
    double height;
    double slider_min;
    double slider_max;
    int is_discrete;
} SVariableMonitor;

#ifdef SJIT_PROFILE_RUNTIME
typedef enum {
    SJIT_PROFILE_LIST_NUMBER_AT_NUMBER = 0,
    SJIT_PROFILE_LIST_NUMBER_AT_VALUE,
    SJIT_PROFILE_LIST_ITEM_VALUE,
    SJIT_PROFILE_LIST_ITEM_NUMBER_SEARCH,
    SJIT_PROFILE_LIST_CONTAINS,
    SJIT_PROFILE_VARIABLE_NUMBER,
    SJIT_PROFILE_VARIABLE_VALUE,
    SJIT_PROFILE_VALUE_COMPARE,
    SJIT_PROFILE_MATHOP,
    SJIT_PROFILE_MATHOP_ABS,
    SJIT_PROFILE_MATHOP_FLOOR,
    SJIT_PROFILE_MATHOP_CEIL,
    SJIT_PROFILE_MATHOP_SQRT,
    SJIT_PROFILE_MATHOP_SIN,
    SJIT_PROFILE_MATHOP_COS,
    SJIT_PROFILE_MATHOP_TAN,
    SJIT_PROFILE_MATHOP_ASIN,
    SJIT_PROFILE_MATHOP_ACOS,
    SJIT_PROFILE_MATHOP_ATAN,
    SJIT_PROFILE_MATHOP_LN,
    SJIT_PROFILE_MATHOP_LOG10,
    SJIT_PROFILE_MATHOP_EXP,
    SJIT_PROFILE_MATHOP_POW10,
    SJIT_PROFILE_VARIABLE_SET_FROM_VALUE,
    SJIT_PROFILE_LIST_REPLACE_NUMBER,
    SJIT_PROFILE_PEN_RENDER_PIXEL,
    SJIT_PROFILE_PEN_COLOR_LIST,
    SJIT_PROFILE_PEN_COLOR_VALUE,
    SJIT_PROFILE_PEN_STAMP,
    SJIT_PROFILE_VALUE_COMPARE_NUMBER_NUMBER,
    SJIT_PROFILE_LIST_VALUE_CACHED_NUMBER,
    SJIT_PROFILE_VARIABLE_SET_NUMBER_SOURCE,
    SJIT_PROFILE_LIST_VALUE_INDEX_NUMBER,
    SJIT_PROFILE_SET_FROM_VARIABLE,
    SJIT_PROFILE_SET_FROM_ARGUMENT,
    SJIT_PROFILE_SET_FROM_LIST_ITEM,
    SJIT_PROFILE_VALUE_COMPARE_NUMBER_STRING,
    SJIT_PROFILE_VALUE_COMPARE_STRING_NUMBER,
    SJIT_PROFILE_VALUE_COMPARE_STRING_STRING,
    SJIT_PROFILE_VALUE_COMPARE_NUMBER_OTHER,
    SJIT_PROFILE_VALUE_COMPARE_OTHER_NUMBER,
    SJIT_PROFILE_VALUE_COMPARE_OTHER_OTHER,
    SJIT_PROFILE_VALUE_COMPARE_LT,
    SJIT_PROFILE_VALUE_COMPARE_EQ,
    SJIT_PROFILE_VALUE_COMPARE_GT,
    SJIT_PROFILE_VARIABLE_SET_STRING_SOURCE,
    SJIT_PROFILE_VARIABLE_SET_BOOL_SOURCE,
    SJIT_PROFILE_VARIABLE_SET_NULL_SOURCE,
    SJIT_PROFILE_VARIABLE_SET_LIST_SOURCE,
    SJIT_PROFILE_VARIABLE_SET_SAME_STRING,
    SJIT_PROFILE_VALUE_COMPARE_STRING_STRING_NUMERIC,
    SJIT_PROFILE_LIST_NUMBER_NUMERIC_CACHE,
    SJIT_PROFILE_LIST_NUMBER_ITEM_NUMBER,
    SJIT_PROFILE_LIST_NUMBER_ITEM_STRING_CACHE,
    SJIT_PROFILE_LIST_NUMBER_ITEM_FALLBACK,
    SJIT_PROFILE_COUNT
} SRuntimeProfileCounter;

#define SJIT_PROFILE_INC(runtime, counter) \
    do { \
        if ((runtime) != NULL) { \
            ++(runtime)->profile_counts[(counter)]; \
        } \
    } while (0)
#else
#define SJIT_PROFILE_INC(runtime, counter) ((void)0)
#endif

struct SRuntime {
    SHostInputSnapshot input;
    double now_ms;
    double delta_ms;
    SSprite **targets;
    int target_count;
    int target_capacity;
    int next_target_id;
    int next_drawable_id;
    SThread **threads;
    int thread_count;
    int thread_capacity;
    int next_thread_id;
    SScriptRegistration *scripts;
    int script_count;
    int script_capacity;
    SDrawCommandBuffer draw;
    SPenPathBuffer pen;
    double timer_start_ms;
    double current_step_time_ms;
    int turbo_mode;
    int redraw_requested;
    int stopped;
    /* Monotonic identity for guards on code specialized for one runtime.
       Unlike the allocation address, this does not alias when malloc reuses a
       destroyed SRuntime's storage.  Zero disables specialization if the
       process-wide 64-bit identity space is ever exhausted. */
    uint64_t instance_id;
    int draw_pen_revision;
    int draw_pen_length;
    void *pen_color_cache;
    SDrawCommand *draw_owned_items;
    int draw_owned_capacity;
    SVariableMonitor *variable_monitors;
    int variable_monitor_count;
    int variable_monitor_capacity;
    /* Optional fixed-size sidecar for the guarded 480x360 pen raster path.
       It is appended after the JIT-visible SRuntime prefix so existing
       generated field offsets remain stable. */
    SPenRasterTile pen_raster_tile;
    SDrawCommand *pen_materialized_items;
    int pen_materialized_capacity;
    int pen_materialized_valid;
#ifdef SJIT_PROFILE_RUNTIME
    uint64_t profile_counts[SJIT_PROFILE_COUNT];
#endif
    /* Host-scheduler-only fields appended after the JIT-visible prefix. */
    void *thread_pool;
    int thread_pool_initialized;
    int thread_pool_parallelism;
    void *parallel_steps;
    int parallel_step_capacity;
    uint64_t parallel_batch_count;
    uint64_t parallel_task_count;
    /* Project-level Scratch renderer option. Kept after the JIT-visible
       prefix so generated code does not depend on this host setting. */
    int fencing;
    /* Compatibility settings are host/runtime configuration and are kept
       after the JIT-visible prefix. */
    int compatibility_mode;
    int list_item_limit;
};

SRuntime *sjit_runtime_create(void);
void sjit_runtime_destroy(SRuntime *runtime);
int sjit_runtime_load_project(SRuntime *runtime, const SProject *project);
void sjit_runtime_green_flag(SRuntime *runtime);
void sjit_runtime_stop_all(SRuntime *runtime);
SRuntimeStatus sjit_runtime_tick(SRuntime *runtime);
void sjit_runtime_set_input(SRuntime *runtime, const SHostInputSnapshot *input);
void sjit_runtime_set_time(SRuntime *runtime, double now_ms, double delta_ms);
void sjit_runtime_set_turbo_mode(SRuntime *runtime, int enabled);
void sjit_runtime_set_fencing(SRuntime *runtime, int enabled);
int sjit_runtime_set_compatibility_mode(SRuntime *runtime, int mode);
int sjit_runtime_compatibility_mode(const SRuntime *runtime);
int sjit_runtime_set_list_item_limit(SRuntime *runtime, int item_limit);
int sjit_runtime_list_item_limit(const SRuntime *runtime);
void sjit_runtime_set_current_step_time(SRuntime *runtime, double step_time_ms);
const SDrawCommandBuffer *sjit_runtime_get_draw_commands(SRuntime *runtime);
void sjit_runtime_clear_draw_commands(SRuntime *runtime);
/* The returned path pointer remains valid until the runtime next mutates the pen path. */
const SDrawCommand *sjit_runtime_pen_path_data(const SRuntime *runtime);
int sjit_runtime_pen_path_count(const SRuntime *runtime);
int sjit_runtime_pen_path_revision(const SRuntime *runtime);
void sjit_runtime_request_redraw(SRuntime *runtime);

SSprite *sjit_runtime_create_sprite(SRuntime *runtime, const char *name, int is_stage);
int sjit_runtime_add_sprite(SRuntime *runtime, SSprite *sprite);
SSprite *sjit_runtime_get_sprite(SRuntime *runtime, int target_id);
SSprite *sjit_runtime_get_sprite_by_name(SRuntime *runtime, const char *name);
int sjit_runtime_register_script(
    SRuntime *runtime,
    int target_id,
    int script_id,
    int opcode_id,
    const char *match_value,
    int restart_existing_threads,
    int edge_activated,
    SScriptEntryFn entry);
int sjit_runtime_register_script_with_data(
    SRuntime *runtime,
    int target_id,
    int script_id,
    int opcode_id,
    const char *match_value,
    int restart_existing_threads,
    int edge_activated,
    SScriptEntryFn entry,
    void *script_data);
struct SCompiledScript;
/* Analyze a registered compiled script.  Returns 1 only when the complete
   reachable script/procedure tree has exclusive ownership and no shared
   effects; every unproven case remains sequential. */
int sjit_runtime_analyze_script_ownership(
    SRuntime *runtime,
    int script_id,
    const struct SCompiledScript *script);
int sjit_runtime_script_parallel_safe(const SRuntime *runtime, int script_id);
uint64_t sjit_runtime_script_ownership_reject_flags(
    const SRuntime *runtime,
    int script_id);
int sjit_runtime_thread_pool_parallelism(const SRuntime *runtime);
uint64_t sjit_runtime_parallel_batch_count(const SRuntime *runtime);
uint64_t sjit_runtime_parallel_task_count(const SRuntime *runtime);
int sjit_runtime_start_hats(SRuntime *runtime, int opcode_id, const char *match_value);
int sjit_runtime_count_threads_in_id_range(SRuntime *runtime, int begin_id, int count);
int sjit_runtime_next_thread_id(const SRuntime *runtime);
int sjit_runtime_has_threads(const SRuntime *runtime);
int sjit_runtime_thread_count(const SRuntime *runtime);
uint64_t sjit_runtime_script_invocation_count(const SRuntime *runtime, int script_id);
void sjit_runtime_remove_done_threads(SRuntime *runtime);
SVariable *sjit_runtime_lookup_or_create_variable(
    SRuntime *runtime,
    int current_target_id,
    const char *name,
    int type);
SVariable *sjit_runtime_lookup_or_create_variable_by_scratch_id(
    SRuntime *runtime,
    int current_target_id,
    const char *scratch_id,
    const char *name,
    int type);
SVariable *sjit_runtime_lookup_variable_by_scratch_id(
    SRuntime *runtime,
    int current_target_id,
    const char *scratch_id,
    const char *name,
    int type);
SVariableMonitor *sjit_runtime_register_variable_monitor(
    SRuntime *runtime,
    const char *id,
    const char *label,
    int target_id,
    int variable_index,
    int variable_type);
SVariableMonitor *sjit_runtime_lookup_variable_monitor(SRuntime *runtime, const char *id);
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
    int is_discrete);
int sjit_runtime_set_variable_monitor_visible(SRuntime *runtime, const char *id, int visible);
int sjit_runtime_variable_monitor_count(const SRuntime *runtime);
const SVariableMonitor *sjit_runtime_variable_monitor_at(const SRuntime *runtime, int index);
SVariable *sjit_runtime_variable_for_monitor(SRuntime *runtime, const SVariableMonitor *monitor);

#ifdef __cplusplus
}
#endif

#endif
