#include "sjit_script.h"

#include "sjit_compare.h"
#include "sjit_clone.h"
#include "sjit_control.h"
#include "sjit_event.h"
#include "sjit_frame.h"
#include "sjit_looks.h"
#include "sjit_list.h"
#include "sjit_motion.h"
#include "sjit_number.h"
#include "sjit_opcode_effects.h"
#include "sjit_operator.h"
#include "sjit_pen.h"
#include "sjit_sensing.h"
#include "sjit_sound.h"
#include "sjit_string.h"
#include "sjit_scheduler.h"
#include "sjit_value.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

enum {
    SJIT_INITIAL_PROCEDURE_CALL_CAPACITY = 64,
    SJIT_VARIABLE_CACHE_SIZE = 64,
    SJIT_INLINE_ARGUMENT_BINDINGS = 4,
    SJIT_LOOP_BRANCH_BATCH = 16384
};

typedef struct {
    const char *name;
    SValue value;
} SArgumentBinding;

typedef struct {
    SArgumentBinding *bindings;
    SArgumentBinding inline_bindings[SJIT_INLINE_ARGUMENT_BINDINGS];
    int binding_count;
    int uses_inline_bindings;
} SArgumentFrame;

typedef struct {
    int current_target_id;
    SSprite *owner;
    int owner_target_id;
    int variable_index;
    int type;
    const char *id;
    const char *name;
    const SString *identity;
} SVariableCacheEntry;

typedef struct {
    SCompiledScript *script;
    SThread *thread;
    SArgumentFrame *frames;
    SVariableCacheEntry variable_cache[SJIT_VARIABLE_CACHE_SIZE];
    SSprite *sprite_cache;
    int sprite_cache_target_id;
    int frame_count;
    int frame_capacity;
} SExecContext;

static void init_exec_context(SExecContext *context, SCompiledScript *script, SThread *thread) {
    if (!context) {
        return;
    }
    context->script = script;
    context->thread = thread;
    context->frames = NULL;
    context->sprite_cache = NULL;
    context->sprite_cache_target_id = -1;
    context->frame_count = 0;
    context->frame_capacity = 0;
    memset(context->variable_cache, 0, sizeof(context->variable_cache));
}

static SValue eval_expr(SRuntime *runtime, int target_id, SExecContext *context, SExpr *expr);
static int eval_expr_number(SRuntime *runtime, int target_id, SExecContext *context, SExpr *expr, double *out);

static int variable_reference_matches(
    const SVariable *variable,
    const char *scratch_id,
    const char *name,
    int type) {
    if (!variable || variable->type != type) {
        return 0;
    }
    if (scratch_id && scratch_id[0] != '\0') {
        return variable->scratch_id &&
            strcmp(sjit_string_cstr(variable->scratch_id), scratch_id) == 0;
    }
    return sjit_string_equals_ignore_case(variable->name, name ? name : "");
}

static const SString *variable_reference_identity(
    const SVariable *variable,
    const char *scratch_id) {
    if (!variable) {
        return NULL;
    }
    return scratch_id && scratch_id[0] != 0 ?
        variable->scratch_id : variable->name;
}

static int variable_cache_matches(
    const SVariable *variable,
    const SString *identity,
    const char *scratch_id,
    const char *name,
    int type) {
    if (!variable || variable->type != type) {
        return 0;
    }
    if (identity) {
        return variable_reference_identity(variable, scratch_id) == identity;
    }
    return variable_reference_matches(variable, scratch_id, name, type);
}

static __attribute__((noinline, cold)) int eval_expr_number_cold(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SExpr *expr,
    double *out);
static __attribute__((noinline)) int eval_expr_number_mathop(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SExpr *expr,
    double *out);
static __attribute__((noinline)) int eval_expr_number_list_item(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SExpr *expr,
    double *out);
static double eval_expr_number_coerced_slow(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SExpr *expr);
static int eval_expr_compare_bool(SRuntime *runtime, int target_id, SExecContext *context, SExpr *expr, int *out);
static __attribute__((noinline, cold)) int eval_expr_compare_bool_generic(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SExpr *expr,
    int *out);
static int eval_expr_bool(SRuntime *runtime, int target_id, SExecContext *context, SExpr *expr, int *out);
static int eval_expr_bool_coerced(SRuntime *runtime, int target_id, SExecContext *context, SExpr *expr);
static SRuntimeStatus execute_statement(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SFrame *frame,
    SStatement *statement);
static SRuntimeStatus execute_statements_from(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SFrame *frame,
    SStatement *statements,
    int count,
    int start,
    int *resume_index);
static SLoopState *statement_loop_state(
    SFrame *frame,
    SExecContext *context,
    SStatement *statement,
    int create);
static void statement_loop_reset(SFrame *frame, SExecContext *context, SStatement *statement);

static inline __attribute__((always_inline)) SSprite *script_sprite(
    SRuntime *runtime,
    int target_id,
    SExecContext *context) {
    if (context && context->sprite_cache && context->sprite_cache_target_id == target_id) {
        return context->sprite_cache;
    }
    SSprite *sprite = sjit_runtime_get_sprite(runtime, target_id);
    if (context) {
        context->sprite_cache = sprite;
        context->sprite_cache_target_id = target_id;
    }
    return sprite;
}

static int execute_adjacent_pen_stamp(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SStatement *statements,
    int count,
    int index) {
    if (!statements || index < 0 || index + 1 >= count ||
        statements[index].opcode != SJIT_STMT_PEN_DOWN ||
        statements[index + 1].opcode != SJIT_STMT_PEN_UP) {
        return 0;
    }
    sjit_pen_stamp(runtime, script_sprite(runtime, target_id, context));
    return 1;
}

static __attribute__((noinline)) int execute_adjacent_pen_color_change_slow(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SStatement *statements,
    int count,
    int index) {
    if (!statements || index < 0 || index + 1 >= count ||
        statements[index].opcode != SJIT_STMT_PEN_SET_COLOR ||
        statements[index + 1].opcode != SJIT_STMT_PEN_CHANGE_COLOR_PARAM) {
        return 0;
    }
    SStatement *color_statement = &statements[index];
    SStatement *change_statement = &statements[index + 1];
    if (!change_statement->index ||
        change_statement->index->opcode != SJIT_EXPR_LITERAL ||
        change_statement->index->literal.tag != SJIT_VALUE_STRING) {
        return 0;
    }
    if (!change_statement->pen_color_param_cache_valid) {
        change_statement->pen_color_param_cache = sjit_pen_color_param_id(
            sjit_string_cstr((const SString *)change_statement->index->literal.ptr));
        change_statement->pen_color_param_cache_valid = 1;
    }
    if (change_statement->pen_color_param_cache == 0) {
        return 0;
    }

    SSprite *sprite = script_sprite(runtime, target_id, context);
    double color_number = 0.0;
    double delta_number = 0.0;
    SValue color = sjit_make_null_fast();
    /* Hex colors are valid Scratch color strings, but the generic numeric
       evaluator treats every literal as numeric and coerces "#rrggbb" to
       zero. Preserve the string so sjit_pen_set_color_value can parse it. */
    const int is_hex_color_literal =
        color_statement->value &&
        color_statement->value->opcode == SJIT_EXPR_LITERAL &&
        color_statement->value->literal.tag == SJIT_VALUE_STRING &&
        color_statement->value->literal.ptr &&
        sjit_string_cstr((const SString *)color_statement->value->literal.ptr)[0] == '#';
    if (!is_hex_color_literal &&
        eval_expr_number(runtime, target_id, context, color_statement->value, &color_number)) {
        color = sjit_make_number_fast(color_number);
    } else {
        color = eval_expr(runtime, target_id, context, color_statement->value);
    }

    if (!eval_expr_number(runtime, target_id, context, change_statement->value, &delta_number)) {
        SValue delta = eval_expr(runtime, target_id, context, change_statement->value);
        delta_number = sjit_to_number_fast(runtime, delta);
        sjit_value_destroy_fast(delta);
    }
    sjit_pen_set_color_value_and_change_param_number(
        runtime,
        sprite,
        color,
        change_statement->pen_color_param_cache,
        delta_number);
    sjit_value_destroy_fast(color);
    return 1;
}

static inline __attribute__((always_inline)) int execute_adjacent_pen_color_change(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SStatement *statements,
    int count,
    int index) {
    if (!statements || index < 0 || index + 1 >= count ||
        statements[index].opcode != SJIT_STMT_PEN_SET_COLOR ||
        statements[index + 1].opcode != SJIT_STMT_PEN_CHANGE_COLOR_PARAM) {
        return 0;
    }
    return execute_adjacent_pen_color_change_slow(
        runtime,
        target_id,
        context,
        statements,
        count,
        index);
}

static __attribute__((noinline)) SVariable *lookup_cached_variable_slow(
    SRuntime *runtime,
    SExecContext *context,
    int target_id,
    const char *id,
    const char *name,
    int type,
    size_t cache_slot) {
    if (!runtime) {
        return NULL;
    }
    SVariable *variable = sjit_runtime_lookup_or_create_variable_by_scratch_id(
        runtime, target_id, id, name, type);
    if (!variable || !context) {
        return variable;
    }
    for (int target_index = 0; target_index < runtime->target_count; ++target_index) {
        SSprite *owner = runtime->targets[target_index];
        for (int variable_index = 0; owner && variable_index < owner->base.variable_count; ++variable_index) {
            if (&owner->base.variables[variable_index] == variable) {
                SVariableCacheEntry *entry = &context->variable_cache[cache_slot];
                entry->current_target_id = target_id;
                entry->owner = owner;
                entry->owner_target_id = owner->base.id;
                entry->variable_index = variable_index;
                entry->type = type;
                entry->id = id;
                entry->name = name;
                entry->identity = variable_reference_identity(variable, id);
                return variable;
            }
        }
    }
    return variable;
}

static inline __attribute__((always_inline)) SVariable *lookup_cached_variable(
    SRuntime *runtime,
    SExecContext *context,
    int target_id,
    const char *id,
    const char *name,
    int type) {
    if (!runtime) {
        return NULL;
    }
    const size_t cache_slot = ((((size_t)id >> 4) ^ ((size_t)name >> 4) ^
                                ((size_t)target_id * 33u) ^ ((size_t)type * 131u)) %
                               SJIT_VARIABLE_CACHE_SIZE);
    if (context) {
        SVariableCacheEntry *entry = &context->variable_cache[cache_slot];
        if (entry->id == id && entry->name == name &&
            entry->current_target_id == target_id && entry->type == type) {
            if (entry->owner && entry->variable_index >= 0 && entry->variable_index < entry->owner->base.variable_count) {
                SVariable *variable = &entry->owner->base.variables[entry->variable_index];
                if (variable_cache_matches(variable, entry->identity, id, name, type)) {
                    return variable;
                }
            }
            entry->name = NULL;
            entry->id = NULL;
            entry->identity = NULL;
            entry->owner = NULL;
        }
    }
    return lookup_cached_variable_slow(runtime, context, target_id, id, name, type, cache_slot);
}

static __attribute__((noinline)) SVariable *lookup_statement_variable_slow(
    SRuntime *runtime,
    int target_id,
    SStatement *statement,
    int type) {
    if (!runtime || !statement) {
        return NULL;
    }
    statement->variable_cache_owner = NULL;
    statement->variable_cache_owner_target_id = 0;
    statement->variable_cache_index = 0;
    statement->variable_cache_runtime = NULL;
    statement->variable_cache_runtime_instance_id = 0;
    statement->variable_cache_identity = NULL;

    const char *id = sjit_string_cstr(statement->variable_id);
    const char *name = sjit_string_cstr(statement->variable_name);
    SVariable *variable = sjit_runtime_lookup_or_create_variable_by_scratch_id(
        runtime, target_id, id, name, type);
    if (!variable) {
        return NULL;
    }
    for (int target_index = 0; target_index < runtime->target_count; ++target_index) {
        SSprite *owner = runtime->targets[target_index];
        for (int variable_index = 0; owner && variable_index < owner->base.variable_count; ++variable_index) {
            if (&owner->base.variables[variable_index] == variable) {
                statement->variable_cache_target_id = target_id;
                statement->variable_cache_owner = NULL;
                statement->variable_cache_owner_target_id = owner->base.id;
                statement->variable_cache_index = variable_index;
                statement->variable_cache_type = type;
                statement->variable_cache_owner_is_original = owner->base.is_original;
                statement->variable_cache_runtime = runtime;
                statement->variable_cache_runtime_instance_id = runtime->instance_id;
                statement->variable_cache_identity = variable_reference_identity(variable, id);
                return variable;
            }
        }
    }
    return variable;
}

static inline __attribute__((always_inline)) SVariable *lookup_statement_variable(
    SRuntime *runtime,
    int target_id,
    SStatement *statement,
    int type) {
    if (!runtime || !statement) {
        return NULL;
    }
    if (statement->variable_cache_target_id == target_id &&
        statement->variable_cache_type == type &&
        statement->variable_cache_runtime == runtime &&
        runtime->instance_id != 0 &&
        statement->variable_cache_runtime_instance_id == runtime->instance_id &&
        statement->variable_cache_owner_target_id > 0 &&
        statement->variable_cache_index >= 0 &&
        runtime) {
        SSprite *owner = sjit_runtime_get_sprite(
            runtime, statement->variable_cache_owner_target_id);
        if (owner && statement->variable_cache_index < owner->base.variable_count) {
            SVariable *variable = &owner->base.variables[statement->variable_cache_index];
            if (variable_cache_matches(
                    variable,
                    statement->variable_cache_identity,
                    sjit_string_cstr(statement->variable_id),
                    sjit_string_cstr(statement->variable_name),
                    type)) {
                return variable;
            }
        }
    }
    return lookup_statement_variable_slow(runtime, target_id, statement, type);
}

static inline __attribute__((always_inline)) SList *script_list(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    const char *id,
    const char *name) {
    SVariable *variable = lookup_cached_variable(
        runtime, context, target_id, id, name, SJIT_VAR_LIST);
    if (!variable || variable->value.tag != SJIT_VALUE_LIST || !variable->value.ptr) {
        return NULL;
    }
    return (SList *)variable->value.ptr;
}

static __attribute__((noinline)) SVariable *lookup_expr_variable_slow(
    SRuntime *runtime,
    int target_id,
    SExpr *expr,
    int type) {
    if (!runtime || !expr) {
        return NULL;
    }
    expr->variable_cache_owner = NULL;
    expr->variable_cache_owner_target_id = 0;
    expr->variable_cache_index = 0;
    expr->variable_cache_runtime = NULL;
    expr->variable_cache_runtime_instance_id = 0;
    expr->variable_cache_identity = NULL;

    const char *id = sjit_string_cstr(expr->variable_id);
    const char *name = sjit_string_cstr((const SString *)expr->literal.ptr);
    SVariable *variable = sjit_runtime_lookup_or_create_variable_by_scratch_id(
        runtime, target_id, id, name, type);
    if (!variable) {
        return NULL;
    }
    for (int target_index = 0; target_index < runtime->target_count; ++target_index) {
        SSprite *owner = runtime->targets[target_index];
        for (int variable_index = 0; owner && variable_index < owner->base.variable_count; ++variable_index) {
            if (&owner->base.variables[variable_index] == variable) {
                expr->variable_cache_target_id = target_id;
                expr->variable_cache_owner = NULL;
                expr->variable_cache_owner_target_id = owner->base.id;
                expr->variable_cache_index = variable_index;
                expr->variable_cache_type = type;
                expr->variable_cache_owner_is_original = owner->base.is_original;
                expr->variable_cache_runtime = runtime;
                expr->variable_cache_runtime_instance_id = runtime->instance_id;
                expr->variable_cache_identity = variable_reference_identity(variable, id);
                return variable;
            }
        }
    }
    return variable;
}

static inline __attribute__((always_inline)) SVariable *lookup_expr_variable(
    SRuntime *runtime,
    int target_id,
    SExpr *expr,
    int type) {
    if (!runtime || !expr) {
        return NULL;
    }
    if (expr->variable_cache_target_id == target_id &&
        expr->variable_cache_type == type &&
        expr->variable_cache_runtime == runtime &&
        runtime->instance_id != 0 &&
        expr->variable_cache_runtime_instance_id == runtime->instance_id &&
        expr->variable_cache_owner_target_id > 0 &&
        expr->variable_cache_index >= 0 &&
        runtime) {
        SSprite *owner = sjit_runtime_get_sprite(runtime, expr->variable_cache_owner_target_id);
        if (owner && expr->variable_cache_index < owner->base.variable_count) {
            SVariable *variable = &owner->base.variables[expr->variable_cache_index];
            if (variable_cache_matches(
                    variable,
                    expr->variable_cache_identity,
                    sjit_string_cstr(expr->variable_id),
                    sjit_string_cstr((const SString *)expr->literal.ptr),
                    type)) {
                return variable;
            }
        }
    }
    return lookup_expr_variable_slow(runtime, target_id, expr, type);
}

static inline __attribute__((always_inline)) SList *expr_list(SRuntime *runtime, int target_id, SExpr *expr) {
    SVariable *variable = lookup_expr_variable(runtime, target_id, expr, SJIT_VAR_LIST);
    if (!variable || variable->value.tag != SJIT_VALUE_LIST || !variable->value.ptr) {
        return NULL;
    }
    return (SList *)variable->value.ptr;
}

static int literal_number_expr(SRuntime *runtime, SExpr *expr, double *out) {
    if (!expr || expr->opcode != SJIT_EXPR_LITERAL || expr->literal.tag == SJIT_VALUE_LIST) {
        return 0;
    }
    if (!expr->number_cache_valid) {
        expr->number_cache = sjit_to_number_fast(runtime, expr->literal);
        expr->number_cache_valid = 1;
    }
    if (out) {
        *out = expr->number_cache;
    }
    return 1;
}


SCompiledScript *sjit_compiled_script_create(int target_id, int statement_count) {
    SCompiledScript *script = (SCompiledScript *)calloc(1, sizeof(SCompiledScript));
    if (!script) {
        return NULL;
    }
    script->target_id = target_id;
    script->statement_count = statement_count;
    if (statement_count > 0) {
        script->statements = (SStatement *)calloc((size_t)statement_count, sizeof(SStatement));
        if (!script->statements) {
            free(script);
            return NULL;
        }
    }
    return script;
}

void sjit_compiled_script_destroy(SCompiledScript *script) {
    if (!script) {
        return;
    }
    for (int i = 0; i < script->statement_count; ++i) {
        sjit_statement_destroy(&script->statements[i]);
    }
    for (int i = 0; i < script->procedure_count; ++i) {
        sjit_compiled_procedure_destroy(&script->procedures[i]);
    }
    sjit_expr_destroy(script->hat_edge_value);
    script->hat_edge_value = NULL;
    free(script->statements);
    free(script->procedures);
    free(script);
}

void sjit_compiled_procedure_destroy(SCompiledProcedure *procedure) {
    if (!procedure) {
        return;
    }
    sjit_string_destroy(procedure->name);
    for (int i = 0; i < procedure->argument_count; ++i) {
        sjit_string_destroy(procedure->argument_names[i]);
    }
    free(procedure->argument_names);
    for (int i = 0; i < procedure->statement_count; ++i) {
        sjit_statement_destroy(&procedure->statements[i]);
    }
    free(procedure->statements);
    procedure->name = NULL;
    procedure->argument_names = NULL;
    procedure->argument_count = 0;
    procedure->statements = NULL;
    procedure->statement_count = 0;
}

SExpr *sjit_expr_create_literal(SValue value) {
    SExpr *expr = (SExpr *)calloc(1, sizeof(SExpr));
    if (!expr) {
        return NULL;
    }
    expr->opcode = SJIT_EXPR_LITERAL;
    if (value.tag == SJIT_VALUE_STRING) {
        /* Literal expressions own an independent string.  Parallel scripts
           may populate the string's numeric cache and adjust its refcount, so
           retaining a caller-shared SString here would invalidate ownership
           isolation even when the AST roots themselves are distinct. */
        const SString *source = (const SString *)value.ptr;
        expr->literal = sjit_make_string_len(
            sjit_string_cstr(source),
            source ? source->length : 0);
    } else {
        expr->literal = sjit_value_clone_fast(value);
    }
    return expr;
}

SExpr *sjit_expr_create_binary(int opcode, SExpr *left, SExpr *right) {
    SExpr *expr = (SExpr *)calloc(1, sizeof(SExpr));
    if (!expr) {
        sjit_expr_destroy(left);
        sjit_expr_destroy(right);
        return NULL;
    }
    expr->opcode = opcode;
    expr->literal = sjit_make_null_fast();
    expr->left = left;
    expr->right = right;
    return expr;
}

SExpr *sjit_expr_create_unary(int opcode, SExpr *operand) {
    SExpr *expr = (SExpr *)calloc(1, sizeof(SExpr));
    if (!expr) {
        sjit_expr_destroy(operand);
        return NULL;
    }
    expr->opcode = opcode;
    expr->literal = sjit_make_null_fast();
    expr->left = operand;
    return expr;
}

SExpr *sjit_expr_create_mathop(const char *operator_name, SExpr *operand) {
    SExpr *expr = (SExpr *)calloc(1, sizeof(SExpr));
    if (!expr) {
        sjit_expr_destroy(operand);
        return NULL;
    }
    expr->opcode = SJIT_EXPR_MATHOP;
    expr->literal = sjit_make_string(operator_name ? operator_name : "");
    expr->left = operand;
    return expr;
}

SExpr *sjit_expr_create_variable_with_id(
    const char *variable_id,
    const char *variable_name) {
    SExpr *expr = (SExpr *)calloc(1, sizeof(SExpr));
    if (!expr) {
        return NULL;
    }
    expr->opcode = SJIT_EXPR_VARIABLE;
    expr->literal = sjit_make_string(variable_name ? variable_name : "");
    if (variable_id && variable_id[0] != '\0') {
        expr->variable_id = sjit_string_new(variable_id);
    }
    return expr;
}

SExpr *sjit_expr_create_variable(const char *variable_name) {
    return sjit_expr_create_variable_with_id(NULL, variable_name);
}

SExpr *sjit_expr_create_argument(const char *argument_name) {
    SExpr *expr = (SExpr *)calloc(1, sizeof(SExpr));
    if (!expr) {
        return NULL;
    }
    expr->opcode = SJIT_EXPR_ARGUMENT;
    expr->literal = sjit_make_string(argument_name ? argument_name : "");
    expr->variable_cache_index = -1;
    return expr;
}

static SExpr *sjit_expr_create_list_expr_with_id(
    int opcode,
    const char *list_id,
    const char *list_name,
    SExpr *operand) {
    SExpr *expr = (SExpr *)calloc(1, sizeof(SExpr));
    if (!expr) {
        sjit_expr_destroy(operand);
        return NULL;
    }
    expr->opcode = opcode;
    expr->literal = sjit_make_string(list_name ? list_name : "");
    if (list_id && list_id[0] != '\0') {
        expr->variable_id = sjit_string_new(list_id);
    }
    expr->left = operand;
    return expr;
}

static SExpr *sjit_expr_create_list_expr(int opcode, const char *list_name, SExpr *operand) {
    return sjit_expr_create_list_expr_with_id(opcode, NULL, list_name, operand);
}

SExpr *sjit_expr_create_list_variable_with_id(
    const char *list_id,
    const char *list_name) {
    return sjit_expr_create_list_expr_with_id(
        SJIT_EXPR_LIST_VARIABLE, list_id, list_name, NULL);
}

SExpr *sjit_expr_create_list_variable(const char *list_name) {
    return sjit_expr_create_list_expr(SJIT_EXPR_LIST_VARIABLE, list_name, NULL);
}

SExpr *sjit_expr_create_list_item_with_id(
    const char *list_id,
    const char *list_name,
    SExpr *index) {
    return sjit_expr_create_list_expr_with_id(
        SJIT_EXPR_LIST_ITEM, list_id, list_name, index);
}

SExpr *sjit_expr_create_list_item(const char *list_name, SExpr *index) {
    return sjit_expr_create_list_expr(SJIT_EXPR_LIST_ITEM, list_name, index);
}

SExpr *sjit_expr_create_list_item_number_with_id(
    const char *list_id,
    const char *list_name,
    SExpr *item) {
    return sjit_expr_create_list_expr_with_id(
        SJIT_EXPR_LIST_ITEM_NUMBER, list_id, list_name, item);
}

SExpr *sjit_expr_create_list_item_number(const char *list_name, SExpr *item) {
    return sjit_expr_create_list_expr(SJIT_EXPR_LIST_ITEM_NUMBER, list_name, item);
}

SExpr *sjit_expr_create_list_length_with_id(
    const char *list_id,
    const char *list_name) {
    return sjit_expr_create_list_expr_with_id(
        SJIT_EXPR_LIST_LENGTH, list_id, list_name, NULL);
}

SExpr *sjit_expr_create_list_length(const char *list_name) {
    return sjit_expr_create_list_expr(SJIT_EXPR_LIST_LENGTH, list_name, NULL);
}

SExpr *sjit_expr_create_list_contains_with_id(
    const char *list_id,
    const char *list_name,
    SExpr *item) {
    return sjit_expr_create_list_expr_with_id(
        SJIT_EXPR_LIST_CONTAINS, list_id, list_name, item);
}

SExpr *sjit_expr_create_list_contains(const char *list_name, SExpr *item) {
    return sjit_expr_create_list_expr(SJIT_EXPR_LIST_CONTAINS, list_name, item);
}

SExpr *sjit_expr_create_timer(void) {
    SExpr *expr = (SExpr *)calloc(1, sizeof(SExpr));
    if (!expr) {
        return NULL;
    }
    expr->opcode = SJIT_EXPR_TIMER;
    expr->literal = sjit_make_null_fast();
    return expr;
}

static SExpr *sjit_expr_create_nullary(int opcode) {
    SExpr *expr = (SExpr *)calloc(1, sizeof(SExpr));
    if (!expr) {
        return NULL;
    }
    expr->opcode = opcode;
    expr->literal = sjit_make_null_fast();
    return expr;
}

SExpr *sjit_expr_create_days_since_2000(void) {
    return sjit_expr_create_nullary(SJIT_EXPR_DAYS_SINCE_2000);
}

SExpr *sjit_expr_create_mouse_x(void) {
    return sjit_expr_create_nullary(SJIT_EXPR_MOUSE_X);
}

SExpr *sjit_expr_create_mouse_y(void) {
    return sjit_expr_create_nullary(SJIT_EXPR_MOUSE_Y);
}

SExpr *sjit_expr_create_x_position(void) {
    return sjit_expr_create_nullary(SJIT_EXPR_X_POSITION);
}

SExpr *sjit_expr_create_y_position(void) {
    return sjit_expr_create_nullary(SJIT_EXPR_Y_POSITION);
}

SExpr *sjit_expr_create_direction(void) {
    return sjit_expr_create_nullary(SJIT_EXPR_DIRECTION);
}

SExpr *sjit_expr_create_costume_number_name(int number_name) {
    SExpr *expr = sjit_expr_create_nullary(SJIT_EXPR_COSTUME_NUMBER_NAME);
    if (expr) {
        expr->literal = sjit_make_bool_fast(number_name);
    }
    return expr;
}

SExpr *sjit_expr_create_size(void) {
    return sjit_expr_create_nullary(SJIT_EXPR_SIZE);
}

SExpr *sjit_expr_create_backdrop_number_name(int number_name) {
    SExpr *expr = sjit_expr_create_nullary(SJIT_EXPR_BACKDROP_NUMBER_NAME);
    if (expr) {
        expr->literal = sjit_make_bool_fast(number_name);
    }
    return expr;
}

SExpr *sjit_expr_create_list_contents_with_id(
    const char *list_id,
    const char *list_name) {
    return sjit_expr_create_list_expr_with_id(
        SJIT_EXPR_LIST_CONTENTS,
        list_id,
        list_name,
        NULL);
}

SExpr *sjit_expr_create_list_contents(const char *list_name) {
    return sjit_expr_create_list_contents_with_id(NULL, list_name);
}

SExpr *sjit_expr_create_touching_object(SExpr *object) {
    return sjit_expr_create_unary(SJIT_EXPR_TOUCHING_OBJECT, object);
}

SExpr *sjit_expr_create_touching_color(SExpr *color) {
    return sjit_expr_create_unary(SJIT_EXPR_TOUCHING_COLOR, color);
}

SExpr *sjit_expr_create_color_touching_color(SExpr *color, SExpr *color2) {
    return sjit_expr_create_binary(SJIT_EXPR_COLOR_TOUCHING_COLOR, color, color2);
}

SExpr *sjit_expr_create_distance_to(SExpr *target) {
    return sjit_expr_create_unary(SJIT_EXPR_DISTANCE_TO, target);
}

SExpr *sjit_expr_create_sensing_of(SExpr *attribute, SExpr *object) {
    return sjit_expr_create_binary(SJIT_EXPR_SENSING_OF, attribute, object);
}

SExpr *sjit_expr_create_current(SExpr *menu) {
    return sjit_expr_create_unary(SJIT_EXPR_CURRENT, menu);
}

SExpr *sjit_expr_create_answer(void) {
    return sjit_expr_create_nullary(SJIT_EXPR_ANSWER);
}

SExpr *sjit_expr_create_loudness(void) {
    return sjit_expr_create_nullary(SJIT_EXPR_LOUDNESS);
}

SExpr *sjit_expr_create_loud(void) {
    return sjit_expr_create_nullary(SJIT_EXPR_LOUD);
}

SExpr *sjit_expr_create_online(void) {
    return sjit_expr_create_nullary(SJIT_EXPR_ONLINE);
}

SExpr *sjit_expr_create_username(void) {
    return sjit_expr_create_nullary(SJIT_EXPR_USERNAME);
}

SExpr *sjit_expr_create_sound_volume(void) {
    return sjit_expr_create_nullary(SJIT_EXPR_SOUND_VOLUME);
}

SExpr *sjit_expr_create_counter(void) {
    return sjit_expr_create_nullary(SJIT_EXPR_COUNTER);
}

SExpr *sjit_expr_create_mouse_down(void) {
    return sjit_expr_create_nullary(SJIT_EXPR_MOUSE_DOWN);
}

SExpr *sjit_expr_create_key_pressed(SExpr *key_name) {
    SExpr *expr = (SExpr *)calloc(1, sizeof(SExpr));
    if (!expr) {
        sjit_expr_destroy(key_name);
        return NULL;
    }
    expr->opcode = SJIT_EXPR_KEY_PRESSED;
    expr->literal = sjit_make_null_fast();
    expr->left = key_name;
    return expr;
}

void sjit_expr_destroy(SExpr *expr) {
    if (!expr) {
        return;
    }
    sjit_expr_destroy(expr->left);
    sjit_expr_destroy(expr->right);
    sjit_value_destroy_fast(expr->literal);
    sjit_string_destroy(expr->variable_id);
    free(expr);
}

void sjit_statement_destroy(SStatement *statement) {
    if (!statement) {
        return;
    }
    sjit_string_destroy(statement->variable_name);
    sjit_string_destroy(statement->procedure_name);
    sjit_string_destroy(statement->variable_id);
    sjit_expr_destroy(statement->value);
    sjit_expr_destroy(statement->index);
    sjit_expr_destroy(statement->condition);
    sjit_expr_destroy(statement->times);
    for (int i = 0; i < statement->argument_count; ++i) {
        sjit_string_destroy(statement->arguments[i].name);
        sjit_expr_destroy(statement->arguments[i].value);
    }
    free(statement->arguments);
    for (int i = 0; i < statement->substack_count; ++i) {
        sjit_statement_destroy(&statement->substack[i]);
    }
    free(statement->substack);
    for (int i = 0; i < statement->substack2_count; ++i) {
        sjit_statement_destroy(&statement->substack2[i]);
    }
    free(statement->substack2);
    statement->variable_name = NULL;
    statement->procedure_name = NULL;
    statement->variable_id = NULL;
    statement->value = NULL;
    statement->index = NULL;
    statement->condition = NULL;
    statement->times = NULL;
    statement->arguments = NULL;
    statement->argument_count = 0;
    statement->substack = NULL;
    statement->substack_count = 0;
    statement->substack2 = NULL;
    statement->substack2_count = 0;
    statement->procedure_cache_index = 0;
    statement->procedure_cache_valid = 0;
    statement->loop_state_cache_index = -1;
    statement->loop_state_cache_scope_depth = 0;
    statement->substack_sync_cache = 0;
    statement->substack2_sync_cache = 0;
}

static SArgumentBinding *lookup_argument_binding(SExecContext *context, SExpr *expr) {
    if (!context || !expr || context->frame_count <= 0) {
        return NULL;
    }
    const char *name = sjit_string_cstr((const SString *)expr->literal.ptr);
    SArgumentFrame *top_frame = &context->frames[context->frame_count - 1];
    const int cached_index = expr->variable_cache_index;
    if (cached_index >= 0 && cached_index < top_frame->binding_count) {
        SArgumentBinding *binding = &top_frame->bindings[cached_index];
        if (strcmp(binding->name ? binding->name : "", name) == 0) {
            return binding;
        }
    }
    for (int frame_index = context->frame_count - 1; frame_index >= 0; --frame_index) {
        SArgumentFrame *frame = &context->frames[frame_index];
        for (int i = 0; i < frame->binding_count; ++i) {
            SArgumentBinding *binding = &frame->bindings[i];
            if (strcmp(binding->name ? binding->name : "", name) == 0) {
                if (frame_index == context->frame_count - 1) {
                    expr->variable_cache_index = i;
                }
                return binding;
            }
        }
    }
    return NULL;
}

static SValue lookup_argument_expr(SExecContext *context, SExpr *expr) {
    SArgumentBinding *binding = lookup_argument_binding(context, expr);
    if (binding) {
        return sjit_value_clone_fast(binding->value);
    }
    return sjit_make_string("");
}

static int key_index_for_name(const char *name) {
    if (!name || name[0] == '\0') {
        return -1;
    }
    if (sjit_cstr_equals_ignore_case(name, "up arrow")) {
        return SJIT_KEY_UP_ARROW;
    }
    if (sjit_cstr_equals_ignore_case(name, "down arrow")) {
        return SJIT_KEY_DOWN_ARROW;
    }
    if (sjit_cstr_equals_ignore_case(name, "right arrow")) {
        return SJIT_KEY_RIGHT_ARROW;
    }
    if (sjit_cstr_equals_ignore_case(name, "left arrow")) {
        return SJIT_KEY_LEFT_ARROW;
    }
    if (sjit_cstr_equals_ignore_case(name, "space")) {
        return ' ';
    }
    if (sjit_cstr_equals_ignore_case(name, "enter")) {
        return SJIT_KEY_ENTER;
    }
    if (sjit_cstr_equals_ignore_case(name, "backspace")) {
        return SJIT_KEY_BACKSPACE;
    }
    if (sjit_cstr_equals_ignore_case(name, "delete")) {
        return SJIT_KEY_DELETE;
    }
    if (sjit_cstr_equals_ignore_case(name, "shift")) {
        return SJIT_KEY_SHIFT;
    }
    if (sjit_cstr_equals_ignore_case(name, "caps lock")) {
        return SJIT_KEY_CAPS_LOCK;
    }
    if (sjit_cstr_equals_ignore_case(name, "scroll lock")) {
        return SJIT_KEY_SCROLL_LOCK;
    }
    if (sjit_cstr_equals_ignore_case(name, "control")) {
        return SJIT_KEY_CONTROL;
    }
    if (sjit_cstr_equals_ignore_case(name, "escape")) {
        return SJIT_KEY_ESCAPE;
    }
    if (sjit_cstr_equals_ignore_case(name, "insert")) {
        return SJIT_KEY_INSERT;
    }
    if (sjit_cstr_equals_ignore_case(name, "home")) {
        return SJIT_KEY_HOME;
    }
    if (sjit_cstr_equals_ignore_case(name, "end")) {
        return SJIT_KEY_END;
    }
    if (sjit_cstr_equals_ignore_case(name, "page up")) {
        return SJIT_KEY_PAGE_UP;
    }
    if (sjit_cstr_equals_ignore_case(name, "page down")) {
        return SJIT_KEY_PAGE_DOWN;
    }
    if (name[1] == '\0') {
        const unsigned char character = (unsigned char)name[0];
        return character >= 'a' && character <= 'z' ?
            character - ('a' - 'A') : character;
    }
    return -1;
}

static int key_index_for_number(double number) {
    if (!isfinite(number) || floor(number) != number) {
        return -1;
    }
    if (number >= 48.0 && number <= 90.0) {
        return (int)number >= 'a' && (int)number <= 'z' ?
            (int)number - ('a' - 'A') : (int)number;
    }
    if (number == 32.0) {
        return ' ';
    }
    switch ((int)number) {
    case 37:
        return SJIT_KEY_LEFT_ARROW;
    case 38:
        return SJIT_KEY_UP_ARROW;
    case 39:
        return SJIT_KEY_RIGHT_ARROW;
    case 40:
        return SJIT_KEY_DOWN_ARROW;
    default:
        return -1;
    }
}

static SValue string_length_value(SRuntime *runtime, SValue value) {
    SValue text = sjit_to_string(runtime, value);
    const SString *string = (const SString *)text.ptr;
    SValue out = sjit_make_number_fast((double)sjit_string_utf16_length(string));
    sjit_value_destroy_fast(text);
    return out;
}

static SValue letter_of_value(SRuntime *runtime, SValue index_value, SValue text_value) {
    const int index = (int)floor(sjit_to_number_fast(runtime, index_value));
    SValue text = sjit_to_string(runtime, text_value);
    const SString *string = (const SString *)text.ptr;
    SValue out;
    out = sjit_make_null_fast();
    out.tag = SJIT_VALUE_STRING;
    out.ptr = sjit_string_utf16_char_at(string, index - 1);
    sjit_value_destroy_fast(text);
    return out;
}

static SValue random_value(SRuntime *runtime, SValue from_value, SValue to_value) {
    double from = sjit_to_number_fast(runtime, from_value);
    double to = sjit_to_number_fast(runtime, to_value);
    if (to < from) {
        const double tmp = from;
        from = to;
        to = tmp;
    }
    if (floor(from) == from && floor(to) == to) {
        const int min = (int)from;
        const int max = (int)to;
        return sjit_make_number_fast((double)(min + (rand() % (max - min + 1))));
    }
    const double unit = (double)rand() / (double)RAND_MAX;
    return sjit_make_number_fast(from + ((to - from) * unit));
}

static double days_since_2000(void) {
    struct timeval now;
    if (gettimeofday(&now, NULL) != 0) {
        return 0.0;
    }
    const double unix_ms = ((double)now.tv_sec * 1000.0) + ((double)now.tv_usec / 1000.0);
    return (unix_ms - 946684800000.0) / 86400000.0;
}

static SValue eval_expr(SRuntime *runtime, int target_id, SExecContext *context, SExpr *expr) {
    if (!expr) {
        return sjit_make_null_fast();
    }
    switch (expr->opcode) {
    case SJIT_EXPR_LITERAL:
        return sjit_value_clone_fast(expr->literal);
    case SJIT_EXPR_TIMER:
        return sjit_make_number_fast((runtime->now_ms - runtime->timer_start_ms) / 1000.0);
    case SJIT_EXPR_DAYS_SINCE_2000:
        return sjit_make_number_fast(days_since_2000());
    case SJIT_EXPR_MOUSE_X:
        return sjit_make_number_fast(runtime->input.mouse_x);
    case SJIT_EXPR_MOUSE_Y:
        return sjit_make_number_fast(runtime->input.mouse_y);
    case SJIT_EXPR_X_POSITION: {
        SSprite *sprite = script_sprite(runtime, target_id, context);
        return sjit_make_number_fast(sprite ? sprite->x : 0.0);
    }
    case SJIT_EXPR_Y_POSITION: {
        SSprite *sprite = script_sprite(runtime, target_id, context);
        return sjit_make_number_fast(sprite ? sprite->y : 0.0);
    }
    case SJIT_EXPR_DIRECTION: {
        SSprite *sprite = script_sprite(runtime, target_id, context);
        return sjit_make_number_fast(sprite ? sprite->direction : 90.0);
    }
    case SJIT_EXPR_COSTUME_NUMBER_NAME: {
        SSprite *sprite = script_sprite(runtime, target_id, context);
        if (expr->literal.number != 0.0) {
            return sjit_make_number_fast(
                sprite && sprite->costume_count > 0 ?
                    (double)sprite->current_costume + 1.0 : 0.0);
        }
        return sjit_make_string(
            sjit_sprite_current_costume_name(sprite));
    }
    case SJIT_EXPR_SIZE: {
        SSprite *sprite = script_sprite(runtime, target_id, context);
        return sjit_make_number_fast(sprite ? floor(sprite->size + 0.5) : 0.0);
    }
    case SJIT_EXPR_BACKDROP_NUMBER_NAME:
        return sjit_looks_backdrop_number_name(
            runtime,
            expr->literal.tag == SJIT_VALUE_BOOL ? expr->literal.number != 0.0 :
                expr->literal.number != 0.0);
    case SJIT_EXPR_LIST_CONTENTS: {
        SList *list = expr_list(runtime, target_id, expr);
        return sjit_list_contents(list);
    }
    case SJIT_EXPR_TOUCHING_OBJECT: {
        SValue object = eval_expr(runtime, target_id, context, expr->left);
        const int touching = sjit_sensing_touching_object(
            runtime,
            script_sprite(runtime, target_id, context),
            object);
        sjit_value_destroy_fast(object);
        return sjit_make_bool_fast(touching);
    }
    case SJIT_EXPR_TOUCHING_COLOR: {
        SValue color = eval_expr(runtime, target_id, context, expr->left);
        const int touching = sjit_sensing_touching_color(
            runtime,
            script_sprite(runtime, target_id, context),
            color);
        sjit_value_destroy_fast(color);
        return sjit_make_bool_fast(touching);
    }
    case SJIT_EXPR_COLOR_TOUCHING_COLOR: {
        SValue color = eval_expr(runtime, target_id, context, expr->left);
        SValue color2 = eval_expr(runtime, target_id, context, expr->right);
        const int touching = sjit_sensing_color_touching_color(
            runtime,
            script_sprite(runtime, target_id, context),
            color,
            color2);
        sjit_value_destroy_fast(color);
        sjit_value_destroy_fast(color2);
        return sjit_make_bool_fast(touching);
    }
    case SJIT_EXPR_DISTANCE_TO: {
        SValue target = eval_expr(runtime, target_id, context, expr->left);
        const double distance = sjit_sensing_distance_to(
            runtime,
            script_sprite(runtime, target_id, context),
            target);
        sjit_value_destroy_fast(target);
        return sjit_make_number_fast(distance);
    }
    case SJIT_EXPR_SENSING_OF: {
        SValue attribute = eval_expr(runtime, target_id, context, expr->left);
        SValue object = eval_expr(runtime, target_id, context, expr->right);
        SValue result = sjit_sensing_attribute_of(
            runtime,
            script_sprite(runtime, target_id, context),
            attribute,
            object);
        sjit_value_destroy_fast(attribute);
        sjit_value_destroy_fast(object);
        return result;
    }
    case SJIT_EXPR_CURRENT: {
        SValue menu = eval_expr(runtime, target_id, context, expr->left);
        SValue text = sjit_to_string(runtime, menu);
        SValue result = sjit_sensing_current(sjit_string_cstr((const SString *)text.ptr));
        sjit_value_destroy_fast(menu);
        sjit_value_destroy_fast(text);
        return result;
    }
    case SJIT_EXPR_ANSWER:
        return sjit_runtime_get_answer(runtime);
    case SJIT_EXPR_LOUDNESS:
        return sjit_make_number_fast(runtime ? runtime->loudness : 0.0);
    case SJIT_EXPR_LOUD:
        return sjit_make_bool_fast(runtime && runtime->loudness > 10.0);
    case SJIT_EXPR_ONLINE:
        return sjit_make_bool_fast(runtime && runtime->online);
    case SJIT_EXPR_USERNAME:
        return sjit_make_string(runtime && runtime->username ?
            sjit_string_cstr(runtime->username) : "");
    case SJIT_EXPR_SOUND_VOLUME: {
        SSprite *sprite = script_sprite(runtime, target_id, context);
        return sjit_make_number_fast(sjit_sound_volume(sprite));
    }
    case SJIT_EXPR_COUNTER:
        return sjit_make_number_fast(runtime ? (double)runtime->counter : 0.0);
    case SJIT_EXPR_MOUSE_DOWN:
        return sjit_make_bool_fast(runtime->input.mouse_down || runtime->input.mouse_pressed_edge);
    case SJIT_EXPR_ARGUMENT:
        return lookup_argument_expr(context, expr);
    case SJIT_EXPR_VARIABLE: {
        (void)context;
        SVariable *variable = lookup_expr_variable(runtime, target_id, expr, SJIT_VAR_SCALAR);
        return variable ? sjit_value_clone_fast(variable->value) : sjit_make_number_fast(0.0);
    }
    case SJIT_EXPR_LIST_VARIABLE: {
        (void)context;
        SVariable *variable = lookup_expr_variable(runtime, target_id, expr, SJIT_VAR_LIST);
        if (!variable || variable->value.tag != SJIT_VALUE_LIST || !variable->value.ptr) {
            return sjit_make_string("");
        }
        return sjit_value_clone_fast(variable->value);
    }
    case SJIT_EXPR_LT: {
        int fast_result = 0;
        if (eval_expr_compare_bool(runtime, target_id, context, expr, &fast_result)) {
            return sjit_make_bool_fast(fast_result);
        }
        SValue left = eval_expr(runtime, target_id, context, expr->left);
        SValue right = eval_expr(runtime, target_id, context, expr->right);
        SValue result = sjit_make_bool_fast(sjit_lt(runtime, left, right));
        sjit_value_destroy_fast(left);
        sjit_value_destroy_fast(right);
        return result;
    }
    case SJIT_EXPR_EQ: {
        int fast_result = 0;
        if (eval_expr_compare_bool(runtime, target_id, context, expr, &fast_result)) {
            return sjit_make_bool_fast(fast_result);
        }
        SValue left = eval_expr(runtime, target_id, context, expr->left);
        SValue right = eval_expr(runtime, target_id, context, expr->right);
        SValue result = sjit_make_bool_fast(sjit_eq(runtime, left, right));
        sjit_value_destroy_fast(left);
        sjit_value_destroy_fast(right);
        return result;
    }
    case SJIT_EXPR_GT: {
        int fast_result = 0;
        if (eval_expr_compare_bool(runtime, target_id, context, expr, &fast_result)) {
            return sjit_make_bool_fast(fast_result);
        }
        SValue left = eval_expr(runtime, target_id, context, expr->left);
        SValue right = eval_expr(runtime, target_id, context, expr->right);
        SValue result = sjit_make_bool_fast(sjit_op_gt(runtime, left, right));
        sjit_value_destroy_fast(left);
        sjit_value_destroy_fast(right);
        return result;
    }
    case SJIT_EXPR_AND: {
        SValue left = eval_expr(runtime, target_id, context, expr->left);
        SValue right = eval_expr(runtime, target_id, context, expr->right);
        SValue result = sjit_make_bool_fast(sjit_op_and(runtime, left, right));
        sjit_value_destroy_fast(left);
        sjit_value_destroy_fast(right);
        return result;
    }
    case SJIT_EXPR_OR: {
        SValue left = eval_expr(runtime, target_id, context, expr->left);
        SValue right = eval_expr(runtime, target_id, context, expr->right);
        SValue result = sjit_make_bool_fast(sjit_op_or(runtime, left, right));
        sjit_value_destroy_fast(left);
        sjit_value_destroy_fast(right);
        return result;
    }
    case SJIT_EXPR_NOT: {
        SValue operand = eval_expr(runtime, target_id, context, expr->left);
        SValue result = sjit_make_bool_fast(sjit_op_not(runtime, operand));
        sjit_value_destroy_fast(operand);
        return result;
    }
    case SJIT_EXPR_LIST_ITEM: {
        SList *list = expr_list(runtime, target_id, expr);
        if (!list) {
            return sjit_make_string("");
        }
        SValue index_value = eval_expr(runtime, target_id, context, expr->left);
        const int index = sjit_list_to_index(runtime, index_value, sjit_list_length(list), 0);
        sjit_value_destroy_fast(index_value);
        if (index == SJIT_LIST_INDEX_INVALID) {
            return sjit_make_string("");
        }
        int fast_ok = 0;
        SValue fast_item = sjit_list_get_fast_number(list, index, &fast_ok);
        if (fast_ok) {
            return fast_item;
        }
        SValue item = sjit_list_get(list, index);
        if (item.tag == SJIT_VALUE_NULL) {
            sjit_value_destroy_fast(item);
            return sjit_make_string("");
        }
        return item;
    }
    case SJIT_EXPR_LIST_ITEM_NUMBER: {
        SList *list = expr_list(runtime, target_id, expr);
        SValue item = eval_expr(runtime, target_id, context, expr->left);
        const int index = list ? sjit_list_item_number(runtime, list, item) : 0;
        sjit_value_destroy_fast(item);
        return sjit_make_number_fast((double)index);
    }
    case SJIT_EXPR_LIST_LENGTH: {
        SList *list = expr_list(runtime, target_id, expr);
        return sjit_make_number_fast((double)sjit_list_length(list));
    }
    case SJIT_EXPR_LIST_CONTAINS: {
        SList *list = expr_list(runtime, target_id, expr);
        SValue item = eval_expr(runtime, target_id, context, expr->left);
        const int contains = list ? sjit_list_contains(runtime, list, item) : 0;
        sjit_value_destroy_fast(item);
        return sjit_make_bool_fast(contains);
    }
    case SJIT_EXPR_MATHOP: {
        SValue operand = eval_expr(runtime, target_id, context, expr->left);
        if (!expr->mathop_cache_valid) {
            expr->mathop_cache = sjit_op_mathop_id(
                sjit_string_cstr((const SString *)expr->literal.ptr));
            expr->mathop_cache_valid = 1;
        }
        SValue result = sjit_make_number_fast(sjit_op_mathop_number_id(
            expr->mathop_cache,
            sjit_to_number_fast(runtime, operand)));
        sjit_value_destroy_fast(operand);
        return result;
    }
    case SJIT_EXPR_ADD:
    case SJIT_EXPR_SUB:
    case SJIT_EXPR_MUL:
    case SJIT_EXPR_DIV: {
        SValue left = eval_expr(runtime, target_id, context, expr->left);
        SValue right = eval_expr(runtime, target_id, context, expr->right);
        SValue result = sjit_make_null_fast();
        if (expr->opcode == SJIT_EXPR_ADD) {
            result = sjit_op_add(runtime, left, right);
        } else if (expr->opcode == SJIT_EXPR_SUB) {
            result = sjit_op_sub(runtime, left, right);
        } else if (expr->opcode == SJIT_EXPR_MUL) {
            result = sjit_op_mul(runtime, left, right);
        } else {
            result = sjit_op_div(runtime, left, right);
        }
        sjit_value_destroy_fast(left);
        sjit_value_destroy_fast(right);
        return result;
    }
    case SJIT_EXPR_MOD: {
        SValue left = eval_expr(runtime, target_id, context, expr->left);
        SValue right = eval_expr(runtime, target_id, context, expr->right);
        SValue result = sjit_op_mod(runtime, left, right);
        sjit_value_destroy_fast(left);
        sjit_value_destroy_fast(right);
        return result;
    }
    case SJIT_EXPR_ROUND: {
        SValue value = eval_expr(runtime, target_id, context, expr->left);
        SValue result = sjit_op_round(runtime, value);
        sjit_value_destroy_fast(value);
        return result;
    }
    case SJIT_EXPR_JOIN: {
        SValue left = eval_expr(runtime, target_id, context, expr->left);
        SValue right = eval_expr(runtime, target_id, context, expr->right);
        SValue result = sjit_op_join(runtime, left, right);
        sjit_value_destroy_fast(left);
        sjit_value_destroy_fast(right);
        return result;
    }
    case SJIT_EXPR_CONTAINS: {
        SValue text = eval_expr(runtime, target_id, context, expr->left);
        SValue substring = eval_expr(runtime, target_id, context, expr->right);
        SValue result = sjit_make_bool_fast(sjit_op_contains(runtime, text, substring));
        sjit_value_destroy_fast(text);
        sjit_value_destroy_fast(substring);
        return result;
    }
    case SJIT_EXPR_LENGTH: {
        SValue value = eval_expr(runtime, target_id, context, expr->left);
        SValue result = string_length_value(runtime, value);
        sjit_value_destroy_fast(value);
        return result;
    }
    case SJIT_EXPR_LETTER_OF: {
        SValue index = eval_expr(runtime, target_id, context, expr->left);
        SValue text = eval_expr(runtime, target_id, context, expr->right);
        SValue result = letter_of_value(runtime, index, text);
        sjit_value_destroy_fast(index);
        sjit_value_destroy_fast(text);
        return result;
    }
    case SJIT_EXPR_RANDOM: {
        SValue left = eval_expr(runtime, target_id, context, expr->left);
        SValue right = eval_expr(runtime, target_id, context, expr->right);
        SValue result = random_value(runtime, left, right);
        sjit_value_destroy_fast(left);
        sjit_value_destroy_fast(right);
        return result;
    }
    case SJIT_EXPR_KEY_PRESSED: {
        SValue key = eval_expr(runtime, target_id, context, expr->left);
        SValue text = sjit_to_string(runtime, key);
        const char *key_name = sjit_string_cstr((const SString *)text.ptr);
        int pressed = 0;
        if (sjit_cstr_equals_ignore_case(key_name, "any")) {
            for (int index = 0; index < 256; ++index) {
                if (runtime->input.key_down[index]) {
                    pressed = 1;
                    break;
                }
            }
        } else {
            const int index = key.tag == SJIT_VALUE_NUMBER ?
                key_index_for_number(key.number) : key_index_for_name(key_name);
            pressed = index >= 0 && index < 256 ? runtime->input.key_down[index] : 0;
        }
        sjit_value_destroy_fast(key);
        sjit_value_destroy_fast(text);
        return sjit_make_bool_fast(pressed);
    }
    default:
        return sjit_make_null_fast();
    }
}

static inline __attribute__((always_inline)) double eval_expr_number_coerced(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SExpr *expr) {
    if (!expr) {
        return 0.0;
    }
    if (expr->opcode == SJIT_EXPR_LITERAL) {
        if (expr->literal.tag == SJIT_VALUE_LIST) {
            return sjit_to_number_fast(runtime, expr->literal);
        }
        if (!expr->number_cache_valid) {
            expr->number_cache = sjit_to_number_fast(runtime, expr->literal);
            expr->number_cache_valid = 1;
        }
        return isnan(expr->number_cache) ? 0.0 : expr->number_cache;
    }
    if (expr->opcode == SJIT_EXPR_VARIABLE) {
        SVariable *variable = lookup_expr_variable(runtime, target_id, expr, SJIT_VAR_SCALAR);
        return variable ? sjit_to_number_fast(runtime, variable->value) : 0.0;
    }
    if (expr->opcode == SJIT_EXPR_ARGUMENT) {
        SArgumentBinding *binding = lookup_argument_binding(context, expr);
        return binding ? sjit_to_number_fast(runtime, binding->value) : 0.0;
    }
    return eval_expr_number_coerced_slow(runtime, target_id, context, expr);
}

static int eval_expr_number(SRuntime *runtime, int target_id, SExecContext *context, SExpr *expr, double *out) {
    if (!expr || !out) {
        return 0;
    }
    switch (expr->opcode) {
    case SJIT_EXPR_LITERAL:
        return literal_number_expr(runtime, expr, out);
    case SJIT_EXPR_VARIABLE: {
        (void)context;
        SVariable *variable = lookup_expr_variable(runtime, target_id, expr, SJIT_VAR_SCALAR);
        if (!variable) {
            return 0;
        }
        if (variable->value.tag == SJIT_VALUE_NUMBER) {
            *out = isnan(variable->value.number) ? 0.0 : variable->value.number;
            return 1;
        }
        if (variable->scalar_kind != SJIT_SCALAR_NUMBER) {
            return 0;
        }
        *out = sjit_variable_number(runtime, variable);
        return 1;
    }
    case SJIT_EXPR_ADD:
    case SJIT_EXPR_SUB:
    case SJIT_EXPR_MUL:
    case SJIT_EXPR_DIV:
    case SJIT_EXPR_MOD: {
        const double left = eval_expr_number_coerced(runtime, target_id, context, expr->left);
        const double right = eval_expr_number_coerced(runtime, target_id, context, expr->right);
        if (expr->opcode == SJIT_EXPR_ADD) {
            *out = left + right;
        } else if (expr->opcode == SJIT_EXPR_SUB) {
            *out = left - right;
        } else if (expr->opcode == SJIT_EXPR_MUL) {
            *out = left * right;
        } else if (expr->opcode == SJIT_EXPR_DIV) {
            *out = left / right;
        } else if (expr->opcode == SJIT_EXPR_MOD) {
            *out = fmod(left, right);
            if (*out / right < 0.0) {
                *out += right;
            }
        }
        return 1;
    }
    case SJIT_EXPR_LIST_ITEM:
        return eval_expr_number_list_item(runtime, target_id, context, expr, out);
    case SJIT_EXPR_MATHOP:
        return eval_expr_number_mathop(runtime, target_id, context, expr, out);
    default:
        return eval_expr_number_cold(runtime, target_id, context, expr, out);
    }
}

static __attribute__((noinline)) int eval_expr_number_mathop(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SExpr *expr,
    double *out) {
    const double value = eval_expr_number_coerced(runtime, target_id, context, expr->left);
    if (!expr->mathop_cache_valid) {
        expr->mathop_cache = sjit_op_mathop_id(
            sjit_string_cstr((const SString *)expr->literal.ptr));
        expr->mathop_cache_valid = 1;
    }
    *out = sjit_op_mathop_number_id(expr->mathop_cache, value);
    return 1;
}

static __attribute__((noinline)) int eval_expr_number_list_item(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SExpr *expr,
    double *out) {
    SList *list = expr_list(runtime, target_id, expr);
    if (!list) {
        return 0;
    }
    double index_number = 0.0;
    if (!eval_expr_number(runtime, target_id, context, expr->left, &index_number)) {
        return 0;
    }
    const int index = (int)floor(index_number);
    return sjit_list_get_number(list, index, out);
}

static __attribute__((noinline, cold)) int eval_expr_number_cold(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SExpr *expr,
    double *out) {
    switch (expr->opcode) {
    case SJIT_EXPR_TIMER:
        *out = (runtime->now_ms - runtime->timer_start_ms) / 1000.0;
        return 1;
    case SJIT_EXPR_DAYS_SINCE_2000:
        *out = days_since_2000();
        return 1;
    case SJIT_EXPR_MOUSE_X:
        *out = runtime->input.mouse_x;
        return 1;
    case SJIT_EXPR_MOUSE_Y:
        *out = runtime->input.mouse_y;
        return 1;
    case SJIT_EXPR_X_POSITION: {
        SSprite *sprite = script_sprite(runtime, target_id, context);
        *out = sprite ? sprite->x : 0.0;
        return 1;
    }
    case SJIT_EXPR_Y_POSITION: {
        SSprite *sprite = script_sprite(runtime, target_id, context);
        *out = sprite ? sprite->y : 0.0;
        return 1;
    }
    case SJIT_EXPR_DIRECTION: {
        SSprite *sprite = script_sprite(runtime, target_id, context);
        *out = sprite ? sprite->direction : 90.0;
        return 1;
    }
    case SJIT_EXPR_COSTUME_NUMBER_NAME: {
        SValue value = eval_expr(runtime, target_id, context, expr);
        *out = sjit_to_number_fast(runtime, value);
        sjit_value_destroy_fast(value);
        return 1;
    }
    case SJIT_EXPR_ARGUMENT: {
        SArgumentBinding *binding = lookup_argument_binding(context, expr);
        return binding ? sjit_value_try_number_for_set(binding->value, out) : 0;
    }
    case SJIT_EXPR_RANDOM: {
        double from = eval_expr_number_coerced(runtime, target_id, context, expr->left);
        double to = eval_expr_number_coerced(runtime, target_id, context, expr->right);
        if (to < from) {
            const double tmp = from;
            from = to;
            to = tmp;
        }
        if (floor(from) == from && floor(to) == to) {
            const int min = (int)from;
            const int max = (int)to;
            *out = (double)(min + (rand() % (max - min + 1)));
        } else {
            const double unit = (double)rand() / (double)RAND_MAX;
            *out = from + ((to - from) * unit);
        }
        return 1;
    }
    case SJIT_EXPR_ROUND: {
        const double value = eval_expr_number_coerced(runtime, target_id, context, expr->left);
        *out = sjit_op_round_number(value);
        return 1;
    }
    case SJIT_EXPR_LIST_LENGTH: {
        SList *list = expr_list(runtime, target_id, expr);
        *out = (double)sjit_list_length(list);
        return 1;
    }
    case SJIT_EXPR_LIST_ITEM_NUMBER:
    case SJIT_EXPR_LENGTH: {
        SValue value = eval_expr(runtime, target_id, context, expr);
        *out = sjit_to_number_fast(runtime, value);
        sjit_value_destroy_fast(value);
        return 1;
    }
    default:
        return 0;
    }
}

static __attribute__((noinline)) double eval_expr_number_coerced_slow(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SExpr *expr) {
    double number = 0.0;
    if (eval_expr_number(runtime, target_id, context, expr, &number)) {
        return isnan(number) ? 0.0 : number;
    }
    if (expr && expr->opcode == SJIT_EXPR_LIST_ITEM) {
        SList *list = expr_list(runtime, target_id, expr);
        if (!list) {
            return 0.0;
        }
        SValue index_value = eval_expr(runtime, target_id, context, expr->left);
        const int index = sjit_list_to_index(runtime, index_value, sjit_list_length(list), 0);
        sjit_value_destroy_fast(index_value);
        return sjit_list_get_coerced_number(runtime, list, index);
    }
    SValue value = eval_expr(runtime, target_id, context, expr);
    number = sjit_to_number_fast(runtime, value);
    sjit_value_destroy_fast(value);
    return number;
}

typedef struct {
    SValue value;
    int owned;
} SCompareOperand;

static inline __attribute__((always_inline)) int expr_is_nonnumeric_string_literal(SExpr *expr) {
    if (!expr || expr->opcode != SJIT_EXPR_LITERAL || expr->literal.tag != SJIT_VALUE_STRING) {
        return 0;
    }
    double number = 0.0;
    int whitespace = 0;
    return !sjit_parse_number_for_compare_fast(expr->literal, &number, &whitespace) ||
        whitespace || isnan(number);
}

static inline __attribute__((always_inline)) SCompareOperand eval_compare_operand(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SExpr *expr) {
    SCompareOperand operand;
    operand.value = sjit_make_null_fast();
    operand.owned = 0;
    if (expr) {
        if (expr->opcode == SJIT_EXPR_LIST_ITEM) {
            SList *list = expr_list(runtime, target_id, expr);
            if (list) {
                SValue index_value = eval_expr(runtime, target_id, context, expr->left);
                const int index = sjit_list_to_index(runtime, index_value, sjit_list_length(list), 0);
                sjit_value_destroy_fast(index_value);
                const SValue *item = sjit_list_get_borrowed(list, index);
                if (item) {
                    operand.value = *item;
                    return operand;
                }
            }
        } else if (expr->opcode == SJIT_EXPR_LITERAL) {
            operand.value = expr->literal;
            return operand;
        } else if (expr->opcode == SJIT_EXPR_ARGUMENT) {
            SArgumentBinding *binding = lookup_argument_binding(context, expr);
            if (binding) {
                operand.value = binding->value;
                return operand;
            }
        } else if (expr->opcode == SJIT_EXPR_VARIABLE) {
            SVariable *variable = lookup_expr_variable(runtime, target_id, expr, SJIT_VAR_SCALAR);
            if (variable) {
                operand.value = variable->value;
                return operand;
            }
        }
    }
    double number = 0.0;
    if (eval_expr_number(runtime, target_id, context, expr, &number)) {
        operand.value = sjit_make_number_fast(number);
        return operand;
    }
    operand.value = eval_expr(runtime, target_id, context, expr);
    operand.owned = 1;
    return operand;
}

static int eval_expr_compare_bool(SRuntime *runtime, int target_id, SExecContext *context, SExpr *expr, int *out) {
    if (!expr || !out ||
        (expr->opcode != SJIT_EXPR_LT && expr->opcode != SJIT_EXPR_EQ && expr->opcode != SJIT_EXPR_GT)) {
        return 0;
    }
    if (expr->opcode == SJIT_EXPR_EQ) {
        SExpr *literal = NULL;
        SExpr *other = NULL;
        if (expr_is_nonnumeric_string_literal(expr->left)) {
            literal = expr->left;
            other = expr->right;
        } else if (expr_is_nonnumeric_string_literal(expr->right)) {
            literal = expr->right;
            other = expr->left;
        }
        if (literal) {
            SCompareOperand operand = eval_compare_operand(runtime, target_id, context, other);
            if (operand.value.tag == SJIT_VALUE_NUMBER && isfinite(operand.value.number)) {
                *out = 0;
            } else if (operand.value.tag == SJIT_VALUE_STRING) {
                *out = sjit_string_equals_ignore_case(
                    (const SString *)operand.value.ptr,
                    sjit_string_cstr((const SString *)literal->literal.ptr));
            } else {
                *out = sjit_eq(runtime, operand.value, literal->literal);
            }
            if (operand.owned) {
                sjit_value_destroy_fast(operand.value);
            }
            return 1;
        }
    }
    return eval_expr_compare_bool_generic(runtime, target_id, context, expr, out);
}

static __attribute__((noinline, cold)) int eval_expr_compare_bool_generic(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SExpr *expr,
    int *out) {
    SCompareOperand left_operand = eval_compare_operand(
        runtime,
        target_id,
        context,
        expr->left);
    SCompareOperand right_operand = eval_compare_operand(
        runtime,
        target_id,
        context,
        expr->right);
    if (expr->opcode == SJIT_EXPR_LT) {
        *out = sjit_lt(runtime, left_operand.value, right_operand.value);
    } else if (expr->opcode == SJIT_EXPR_EQ) {
        *out = sjit_eq(runtime, left_operand.value, right_operand.value);
    } else {
        *out = sjit_op_gt(runtime, left_operand.value, right_operand.value);
    }
    if (left_operand.owned) {
        sjit_value_destroy_fast(left_operand.value);
    }
    if (right_operand.owned) {
        sjit_value_destroy_fast(right_operand.value);
    }
    return 1;
}

static inline __attribute__((always_inline)) int eval_expr_number_for_set(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SExpr *expr,
    double *out) {
    if (!expr) {
        return 0;
    }
    if (expr->opcode == SJIT_EXPR_LITERAL &&
        (expr->literal.tag == SJIT_VALUE_STRING || expr->literal.tag == SJIT_VALUE_LIST)) {
        return 0;
    }
    return eval_expr_number(runtime, target_id, context, expr, out);
}

static int eval_expr_bool(SRuntime *runtime, int target_id, SExecContext *context, SExpr *expr, int *out) {
    if (!expr || !out) {
        return 0;
    }
    switch (expr->opcode) {
    case SJIT_EXPR_LITERAL:
        *out = sjit_to_bool(runtime, expr->literal);
        return 1;
    case SJIT_EXPR_MOUSE_DOWN:
        *out = (runtime->input.mouse_down || runtime->input.mouse_pressed_edge) ? 1 : 0;
        return 1;
    case SJIT_EXPR_ARGUMENT: {
        SArgumentBinding *binding = lookup_argument_binding(context, expr);
        if (!binding) {
            return 0;
        }
        *out = sjit_to_bool(runtime, binding->value);
        return 1;
    }
    case SJIT_EXPR_VARIABLE: {
        (void)context;
        SVariable *variable = lookup_expr_variable(runtime, target_id, expr, SJIT_VAR_SCALAR);
        if (!variable) {
            return 0;
        }
        *out = sjit_to_bool(runtime, variable->value);
        return 1;
    }
    case SJIT_EXPR_AND: {
        const int left = eval_expr_bool_coerced(runtime, target_id, context, expr->left);
        const int right = eval_expr_bool_coerced(runtime, target_id, context, expr->right);
        *out = left && right;
        return 1;
    }
    case SJIT_EXPR_OR: {
        const int left = eval_expr_bool_coerced(runtime, target_id, context, expr->left);
        const int right = eval_expr_bool_coerced(runtime, target_id, context, expr->right);
        *out = left || right;
        return 1;
    }
    case SJIT_EXPR_NOT: {
        const int value = eval_expr_bool_coerced(runtime, target_id, context, expr->left);
        *out = !value;
        return 1;
    }
    case SJIT_EXPR_LT:
    case SJIT_EXPR_EQ:
    case SJIT_EXPR_GT:
        return eval_expr_compare_bool(runtime, target_id, context, expr, out);
    case SJIT_EXPR_LIST_CONTAINS:
    case SJIT_EXPR_CONTAINS:
    case SJIT_EXPR_KEY_PRESSED: {
        SValue value = eval_expr(runtime, target_id, context, expr);
        *out = sjit_to_bool(runtime, value);
        sjit_value_destroy_fast(value);
        return 1;
    }
    default:
        return 0;
    }
}

static int eval_expr_bool_coerced(SRuntime *runtime, int target_id, SExecContext *context, SExpr *expr) {
    int truthy = 0;
    if (eval_expr_bool(runtime, target_id, context, expr, &truthy)) {
        return truthy;
    }
    SValue value = eval_expr(runtime, target_id, context, expr);
    truthy = sjit_to_bool(runtime, value);
    sjit_value_destroy_fast(value);
    return truthy;
}

static SCompiledProcedure *find_statement_procedure(SExecContext *context, SStatement *statement) {
    if (!context || !context->script || !statement) {
        return NULL;
    }
    if (statement->procedure_cache_valid &&
        statement->procedure_cache_index >= 0 &&
        statement->procedure_cache_index < context->script->procedure_count) {
        SCompiledProcedure *cached =
            &context->script->procedures[statement->procedure_cache_index];
        if (cached->name && statement->procedure_name &&
            strcmp(
                sjit_string_cstr(cached->name),
                sjit_string_cstr(statement->procedure_name)) == 0) {
            return cached;
        }
    }
    statement->procedure_cache_valid = 0;
    statement->procedure_cache_index = -1;
    const char *name = sjit_string_cstr(statement->procedure_name);
    for (int i = 0; i < context->script->procedure_count; ++i) {
        SCompiledProcedure *procedure = &context->script->procedures[i];
        if (strcmp(sjit_string_cstr(procedure->name), name) == 0) {
            statement->procedure_cache_index = i;
            statement->procedure_cache_valid = 1;
            return procedure;
        }
    }
    return NULL;
}

static void destroy_argument_frame(SArgumentFrame *frame) {
    if (!frame) {
        return;
    }
    for (int i = 0; i < frame->binding_count; ++i) {
        sjit_value_destroy_fast(frame->bindings[i].value);
    }
    if (!frame->uses_inline_bindings) {
        free(frame->bindings);
    }
    frame->bindings = NULL;
    frame->binding_count = 0;
    frame->uses_inline_bindings = 0;
}

static void destroy_exec_context(SExecContext *context) {
    if (!context) {
        return;
    }
    for (int i = 0; i < context->frame_count; ++i) {
        destroy_argument_frame(&context->frames[i]);
    }
    free(context->frames);
    context->frames = NULL;
    context->frame_count = 0;
    context->frame_capacity = 0;
}

static int ensure_argument_frame_capacity(SExecContext *context) {
    if (!context) {
        return 0;
    }
    if (context->frame_count >= SJIT_MAX_PROCEDURE_CALL_DEPTH) {
        return 0;
    }
    if (context->frame_count < context->frame_capacity) {
        return 1;
    }

    int next_capacity = context->frame_capacity > 0 ?
        context->frame_capacity * 2 :
        SJIT_INITIAL_PROCEDURE_CALL_CAPACITY;
    if (next_capacity <= context->frame_capacity) {
        return 0;
    }
    if (next_capacity > SJIT_MAX_PROCEDURE_CALL_DEPTH) {
        next_capacity = SJIT_MAX_PROCEDURE_CALL_DEPTH;
    }

    SArgumentFrame *frames = (SArgumentFrame *)realloc(
        context->frames,
        (size_t)next_capacity * sizeof(SArgumentFrame));
    if (!frames) {
        return 0;
    }
    for (int i = 0; i < context->frame_count; ++i) {
        if (frames[i].uses_inline_bindings) {
            frames[i].bindings = frames[i].inline_bindings;
        }
    }
    memset(
        frames + context->frame_capacity,
        0,
        (size_t)(next_capacity - context->frame_capacity) * sizeof(SArgumentFrame));
    context->frames = frames;
    context->frame_capacity = next_capacity;
    return 1;
}

static SRuntimeStatus execute_procedure_call(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SFrame *frame,
    SStatement *statement) {
    if (!statement || !context) {
        return SJIT_STATUS_OK;
    }
    if (!ensure_argument_frame_capacity(context)) {
        fprintf(stderr, "procedure call depth exceeded while calling %s\n", sjit_string_cstr(statement->procedure_name));
        return SJIT_STATUS_ERROR;
    }
    SCompiledProcedure *procedure = find_statement_procedure(context, statement);
    if (!procedure) {
        return SJIT_STATUS_OK;
    }
    SLoopState *call_state = NULL;
    int resume_index = 0;
    if (frame) {
        call_state = statement_loop_state(frame, context, statement, 1);
        if (!call_state) {
            return SJIT_STATUS_ERROR;
        }
        if (!call_state->branch_active) {
            call_state->branch_active = 1;
            call_state->sub_pc = 0;
        }
        resume_index = call_state->sub_pc;
    }
    /* A compiled procedure is specialized for the script's original target.
       Clone threads deliberately stay on the interpreter path until the JIT
       uses thread-relative target handles throughout the procedure body. */
    if (procedure->jit_entry && context->thread &&
        context->thread->target_id == context->script->target_id &&
        (context->script->jit_runtime_instance_id == 0 ||
         context->script->jit_runtime_instance_id == runtime->instance_id) &&
        frame && statement->argument_count >= 0) {
        const int argument_count = statement->argument_count;
        double inline_numeric_args[SJIT_MAX_PARAMS > 0 ? SJIT_MAX_PARAMS : 1];
        SValue inline_value_args[SJIT_MAX_PARAMS > 0 ? SJIT_MAX_PARAMS : 1];
        double *numeric_args = inline_numeric_args;
        SValue *value_args = inline_value_args;
        if (argument_count > SJIT_MAX_PARAMS) {
            numeric_args = (double *)calloc((size_t)argument_count, sizeof(double));
            value_args = (SValue *)calloc((size_t)argument_count, sizeof(SValue));
            if (!numeric_args || !value_args) {
                free(numeric_args);
                free(value_args);
                return SJIT_STATUS_ERROR;
            }
        }
        for (int i = 0; i < argument_count; ++i) {
            value_args[i] = eval_expr(runtime, target_id, context, statement->arguments[i].value);
            numeric_args[i] = sjit_to_number_fast(runtime, value_args[i]);
        }
        const int previous_warp_mode = frame->warp_mode;
        if (procedure->warp_mode) {
            frame->warp_mode = 1;
        }
        const SRuntimeStatus status = procedure->jit_entry(
            runtime,
            context->thread,
            frame,
            context->script,
            context->frame_count + 1,
            numeric_args,
            value_args);
        frame->warp_mode = previous_warp_mode;
        for (int i = 0; i < argument_count; ++i) {
            sjit_value_destroy_fast(value_args[i]);
        }
        if (argument_count > SJIT_MAX_PARAMS) {
            free(numeric_args);
            free(value_args);
        }
        const SRuntimeStatus normalized_status = status == SJIT_STATUS_DONE ? SJIT_STATUS_OK : status;
        if (normalized_status == SJIT_STATUS_OK || normalized_status == SJIT_STATUS_ERROR) {
            statement_loop_reset(frame, context, statement);
        }
        return normalized_status;
    }

    const int argument_frame_index = context->frame_count;
    SArgumentFrame *argument_frame = &context->frames[argument_frame_index];
    argument_frame->binding_count = statement->argument_count;
    argument_frame->bindings = NULL;
    argument_frame->uses_inline_bindings = 0;
    if (argument_frame->binding_count > 0) {
        if (argument_frame->binding_count <= SJIT_INLINE_ARGUMENT_BINDINGS) {
            argument_frame->bindings = argument_frame->inline_bindings;
            argument_frame->uses_inline_bindings = 1;
            memset(
                argument_frame->bindings,
                0,
                (size_t)argument_frame->binding_count * sizeof(SArgumentBinding));
        } else {
            argument_frame->bindings = (SArgumentBinding *)calloc((size_t)argument_frame->binding_count, sizeof(SArgumentBinding));
            if (!argument_frame->bindings) {
                argument_frame->binding_count = 0;
                return SJIT_STATUS_ERROR;
            }
        }
    }
    for (int i = 0; i < argument_frame->binding_count; ++i) {
        argument_frame->bindings[i].name = sjit_string_cstr(statement->arguments[i].name);
        argument_frame->bindings[i].value = eval_expr(runtime, target_id, context, statement->arguments[i].value);
    }

    const int previous_warp_mode = frame ? frame->warp_mode : 0;
    if (frame && procedure->warp_mode) {
        frame->warp_mode = 1;
    }
    ++context->frame_count;
    int next_resume_index = resume_index;
    const SRuntimeStatus status = execute_statements_from(
        runtime,
        target_id,
        context,
        frame,
        procedure->statements,
        procedure->statement_count,
        resume_index,
        &next_resume_index);
    if (status == SJIT_STATUS_OK && frame) {
        sjit_control_loop_reset_from_depth(frame, context->frame_count);
    }
    --context->frame_count;
    if (frame) {
        frame->warp_mode = previous_warp_mode;
    }
    argument_frame = &context->frames[argument_frame_index];
    destroy_argument_frame(argument_frame);
    const SRuntimeStatus normalized_status = status == SJIT_STATUS_DONE ? SJIT_STATUS_OK : status;
    if (normalized_status == SJIT_STATUS_OK || normalized_status == SJIT_STATUS_ERROR) {
        if (frame) {
            sjit_control_loop_reset_from_depth(frame, context->frame_count + 1);
        }
        statement_loop_reset(frame, context, statement);
    } else if (call_state) {
        call_state->sub_pc = next_resume_index;
    }
    return normalized_status;
}

static SRuntimeStatus execute_statements_from(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SFrame *frame,
    SStatement *statements,
    int count,
    int start,
    int *resume_index) {
    int begin = start;
    if (begin < 0 || begin >= count) {
        begin = 0;
    }
    for (int i = begin; i < count; ++i) {
        if (execute_adjacent_pen_stamp(runtime, target_id, context, statements, count, i)) {
            ++i;
            continue;
        }
        if (execute_adjacent_pen_color_change(runtime, target_id, context, statements, count, i)) {
            ++i;
            continue;
        }
        SRuntimeStatus status = execute_statement(runtime, target_id, context, frame, &statements[i]);
        if (status != SJIT_STATUS_OK) {
            if (resume_index) {
                *resume_index = i;
            }
            return status;
        }
    }
    if (resume_index) {
        *resume_index = count;
    }
    return SJIT_STATUS_OK;
}

static SRuntimeStatus execute_control_substack(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SFrame *frame,
    SLoopState *state,
    SStatement *statements,
    int count) {
    if (!state) {
        return SJIT_STATUS_OK;
    }
    if (count == 1 && state->sub_pc == 0) {
        return execute_statement(runtime, target_id, context, frame, &statements[0]);
    }
    int resume_index = state->sub_pc;
    const SRuntimeStatus status = execute_statements_from(
        runtime,
        target_id,
        context,
        frame,
        statements,
        count,
        state->sub_pc,
        &resume_index);
    if (status != SJIT_STATUS_OK) {
        state->sub_pc = resume_index;
        return status;
    }
    state->sub_pc = 0;
    return SJIT_STATUS_OK;
}

static SRuntimeStatus finish_control_branch(SFrame *frame, SLoopState *state, SStatement *statement) {
    if (state) {
        state->branch_active = 0;
        state->sub_pc = 0;
    }
    if (!frame || !frame->warp_mode) {
        return SJIT_STATUS_YIELDED;
    }
    (void)statement;
    return SJIT_STATUS_OK;
}

static SRuntimeStatus finish_batched_loop_branch(
    SRuntime *runtime,
    SFrame *frame,
    SLoopState *state,
    SStatement *statement,
    int branch_count) {
    if (state) {
        state->branch_active = 0;
        state->sub_pc = 0;
    }
    (void)statement;
    // Warp-mode loops still need a preemption point so long-running setup
    // does not monopolize a single scheduler pass.
    if (frame && frame->warp_mode && branch_count >= SJIT_LOOP_BRANCH_BATCH) {
        return SJIT_STATUS_YIELDED;
    }
    if (!runtime) {
        return SJIT_STATUS_YIELDED;
    }
    if ((!state || state->scope_depth == 0) && branch_count >= SJIT_LOOP_BRANCH_BATCH) {
        return SJIT_STATUS_YIELDED;
    }
    // Drawing and sibling threads must not interrupt a warp procedure. The
    // batch boundary above remains its explicit preemption point.
    if (!frame || !frame->warp_mode) {
        if (!runtime->turbo_mode && runtime->redraw_requested) {
            return SJIT_STATUS_YIELDED;
        }
        if (runtime->thread_count > 1) {
            return SJIT_STATUS_YIELDED;
        }
    }
    return SJIT_STATUS_OK;
}

static int control_scope_depth(SExecContext *context) {
    return context ? context->frame_count : 0;
}

static double scratch_repeat_round(double value) {
    if (isnan(value)) {
        return 0.0;
    }
    if (isinf(value)) {
        return value;
    }
    return floor(value + 0.5);
}

static double eval_repeat_count(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SExpr *expr) {
    double times_number = 0.0;
    if (eval_expr_number(runtime, target_id, context, expr, &times_number)) {
        return scratch_repeat_round(times_number);
    }
    SValue times_value = eval_expr(runtime, target_id, context, expr);
    times_number = scratch_repeat_round(sjit_to_number_fast(runtime, times_value));
    sjit_value_destroy_fast(times_value);
    return times_number;
}

static int eval_condition_bool(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SExpr *expr) {
    int truthy = 0;
    if (!eval_expr_bool(runtime, target_id, context, expr, &truthy)) {
        SValue condition = eval_expr(runtime, target_id, context, expr);
        truthy = sjit_to_bool(runtime, condition);
        sjit_value_destroy_fast(condition);
    }
    return truthy;
}

static SLoopState *statement_loop_state(
    SFrame *frame,
    SExecContext *context,
    SStatement *statement,
    int create) {
    if (!frame || !statement) {
        return NULL;
    }
    const int depth = control_scope_depth(context);
    const int cached_index = statement->loop_state_cache_index;
    if (cached_index >= 0 && cached_index < frame->loop_state_count) {
        SLoopState *cached = &frame->loop_states[cached_index];
        if (cached->key == statement && cached->scope_depth == depth) {
            frame->loop_state_cache_index = cached_index;
            return cached;
        }
    }
    SLoopState *state = sjit_control_loop_state_at_depth(frame, statement, depth, create);
    if (state) {
        statement->loop_state_cache_index = (int)(state - frame->loop_states);
        statement->loop_state_cache_scope_depth = depth;
    }
    return state;
}

static int statement_repeat_should_enter(
    SRuntime *runtime,
    SFrame *frame,
    SExecContext *context,
    SStatement *statement,
    SValue times) {
    SLoopState *state = statement_loop_state(frame, context, statement, 0);
    if (!state) {
        state = statement_loop_state(frame, context, statement, 1);
        if (state) {
            state->counter = scratch_repeat_round(sjit_to_number_fast(runtime, times));
        }
    }
    if (!state) {
        return 0;
    }
    state->counter -= 1.0;
    return state->counter >= 0.0;
}

static void shrink_statement_loop_state_count(SFrame *frame) {
    if (!frame) {
        return;
    }
    while (frame->loop_state_count > 0 &&
           !frame->loop_states[frame->loop_state_count - 1].key) {
        --frame->loop_state_count;
    }
}

static void statement_loop_reset(SFrame *frame, SExecContext *context, SStatement *statement) {
    const int depth = control_scope_depth(context);
    if (frame && statement) {
        const int cached_index = statement->loop_state_cache_index;
        if (cached_index >= 0 && cached_index < frame->loop_state_count) {
            SLoopState *state = &frame->loop_states[cached_index];
            if (state->key == statement && state->scope_depth == depth) {
                memset(state, 0, sizeof(*state));
                frame->loop_state_cache_index = -1;
                statement->loop_state_cache_index = -1;
                statement->loop_state_cache_scope_depth = depth;
                shrink_statement_loop_state_count(frame);
                return;
            }
        }
    }
    if (statement) {
        statement->loop_state_cache_index = -1;
        statement->loop_state_cache_scope_depth = depth;
    }
    sjit_control_loop_reset_at_depth(frame, statement, depth);
}

static int statement_is_synchronous(SStatement *statement);

static int statements_are_synchronous(SStatement *statements, int count) {
    if (count <= 0) {
        return 1;
    }
    if (!statements) {
        return 0;
    }
    for (int i = 0; i < count; ++i) {
        if (!statement_is_synchronous(&statements[i])) {
            return 0;
        }
    }
    return 1;
}

static __attribute__((noinline)) int statement_substack_is_synchronous_slow(
    SStatement *statement,
    int second) {
    int *cache = second ? &statement->substack2_sync_cache : &statement->substack_sync_cache;
    const int synchronous = statements_are_synchronous(
        second ? statement->substack2 : statement->substack,
        second ? statement->substack2_count : statement->substack_count);
    *cache = synchronous ? 1 : -1;
    return synchronous;
}

static inline __attribute__((always_inline)) int statement_substack_is_synchronous(
    SStatement *statement,
    int second) {
    if (!statement) {
        return 0;
    }
    const int cached = second ? statement->substack2_sync_cache : statement->substack_sync_cache;
    if (cached != 0) {
        return cached > 0;
    }
    return statement_substack_is_synchronous_slow(statement, second);
}

static int statement_is_synchronous(SStatement *statement) {
    if (!statement) {
        return 0;
    }
    const OpcodeEffects opcode_effects = sjit_statement_opcode_effects(statement->opcode);
    if (opcode_effects.requiresInterpreter ||
        opcode_effects.canYield ||
        opcode_effects.canCallUnknown) {
        return 0;
    }
    switch (statement->opcode) {
    case SJIT_STMT_IF:
        return statement_substack_is_synchronous(statement, 0);
    case SJIT_STMT_IF_ELSE:
        return statement_substack_is_synchronous(statement, 0) &&
            statement_substack_is_synchronous(statement, 1);
    default:
        return 1;
    }
}

static SRuntimeStatus execute_synchronous_substack(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SFrame *frame,
    SStatement *statements,
    int count) {
    int resume_index = 0;
    return execute_statements_from(
        runtime,
        target_id,
        context,
        frame,
        statements,
        count,
        0,
        &resume_index);
}

static SRuntimeStatus execute_statement(
    SRuntime *runtime,
    int target_id,
    SExecContext *context,
    SFrame *frame,
    SStatement *statement) {
    if (!runtime || !statement) {
        return SJIT_STATUS_OK;
    }
    switch (statement->opcode) {
    case SJIT_STMT_BROADCAST: {
        SValue message = eval_expr(runtime, target_id, context, statement->value);
        SValue text = sjit_to_string(runtime, message);
        sjit_event_broadcast(runtime, sjit_string_cstr((const SString *)text.ptr));
        sjit_value_destroy_fast(message);
        sjit_value_destroy_fast(text);
        break;
    }
    case SJIT_STMT_BROADCAST_AND_WAIT: {
        SValue message = eval_expr(runtime, target_id, context, statement->value);
        SValue text = sjit_to_string(runtime, message);
        const SRuntimeStatus status = sjit_event_broadcast_and_wait(
            runtime,
            frame,
            sjit_string_cstr((const SString *)text.ptr),
            frame ? frame->pc : 0);
        sjit_value_destroy_fast(message);
        sjit_value_destroy_fast(text);
        if (status != SJIT_STATUS_OK) {
            return status;
        }
        break;
    }
    case SJIT_STMT_WAIT: {
        SValue duration = eval_expr(runtime, target_id, context, statement->value);
        SRuntimeStatus status = sjit_control_wait(runtime, frame, duration, frame ? frame->pc : 0);
        sjit_value_destroy_fast(duration);
        if (status != SJIT_STATUS_OK) {
            return status;
        }
        break;
    }
    case SJIT_STMT_LOOKS_SAY_FOR_SECS: {
        if (frame && frame->wake_time_ms < 0.0) {
            SValue message = eval_expr(runtime, target_id, context, statement->value);
            SSprite *sprite = script_sprite(runtime, target_id, context);
            sjit_looks_say(runtime, sprite, message, 0);
            const double seconds = eval_expr_number_coerced(
                runtime, target_id, context, statement->index);
            if (sprite) {
                sprite->bubble_until_ms = runtime->now_ms + fmax(0.0, seconds) * 1000.0;
            }
            sjit_value_destroy_fast(message);
        }
        SValue duration = eval_expr(runtime, target_id, context, statement->index);
        SRuntimeStatus status = sjit_control_wait(runtime, frame, duration, frame ? frame->pc : 0);
        sjit_value_destroy_fast(duration);
        if (status != SJIT_STATUS_OK) {
            return status;
        }
        break;
    }
    case SJIT_STMT_WAIT_UNTIL:
        if (!eval_condition_bool(runtime, target_id, context, statement->condition)) {
            sjit_runtime_request_redraw(runtime);
            return SJIT_STATUS_YIELDED;
        }
        break;
    case SJIT_STMT_STOP_OTHER_SCRIPTS:
        sjit_scheduler_stop_for_target(runtime, target_id, context && context->thread ? context->thread->id : -1);
        break;
    case SJIT_STMT_STOP_OTHER_SCRIPTS_IN_STAGE: {
        int stage_id = 0;
        for (int i = 0; i < runtime->target_count; ++i) {
            if (runtime->targets[i] && runtime->targets[i]->base.is_stage) {
                stage_id = runtime->targets[i]->base.id;
                break;
            }
        }
        if (stage_id != 0) {
            sjit_scheduler_stop_for_target(
                runtime,
                stage_id,
                context && context->thread ? context->thread->id : -1);
        }
        break;
    }
    case SJIT_STMT_STOP_ALL:
        sjit_runtime_stop_all(runtime);
        return SJIT_STATUS_DONE;
    case SJIT_STMT_RESET_TIMER:
        runtime->timer_start_ms = runtime->now_ms;
        break;
    case SJIT_STMT_SET_VARIABLE: {
        SVariable *variable = lookup_statement_variable(runtime, target_id, statement, SJIT_VAR_SCALAR);
        if (variable && variable->scalar_kind == SJIT_SCALAR_NUMBER) {
            double number = 0.0;
            if (eval_expr_number_for_set(runtime, target_id, context, statement->value, &number)) {
                sjit_variable_set_number_fast(variable, number);
                break;
            }
        } else if (variable && variable->scalar_kind == SJIT_SCALAR_BOOL) {
            int truthy = 0;
            if (eval_expr_bool(runtime, target_id, context, statement->value, &truthy)) {
                sjit_variable_set_bool(variable, truthy);
                break;
            }
        }
        SValue value = eval_expr(runtime, target_id, context, statement->value);
        if (variable) {
            sjit_variable_set_move(variable, value);
        } else {
            sjit_value_destroy_fast(value);
        }
        break;
    }
    case SJIT_STMT_CHANGE_VARIABLE: {
        SVariable *variable = lookup_statement_variable(runtime, target_id, statement, SJIT_VAR_SCALAR);
        double delta = 0.0;
        if (variable && eval_expr_number(runtime, target_id, context, statement->value, &delta)) {
            if (variable->value.tag == SJIT_VALUE_NUMBER) {
                const double current = isnan(variable->value.number) ? 0.0 : variable->value.number;
                variable->value.number = current + delta;
                variable->value.ptr = NULL;
            } else {
                sjit_variable_change_by_number(runtime, variable, delta);
            }
            break;
        }
        SValue value = eval_expr(runtime, target_id, context, statement->value);
        if (variable) {
            sjit_variable_change_by(runtime, variable, value);
        }
        sjit_value_destroy_fast(value);
        break;
    }
    case SJIT_STMT_LIST_ADD: {
        SList *list = script_list(
            runtime,
            target_id,
            context,
            sjit_string_cstr(statement->variable_id),
            sjit_string_cstr(statement->variable_name));
        double number = 0.0;
        if (list && eval_expr_number_for_set(runtime, target_id, context, statement->value, &number)) {
            sjit_list_push_number(list, number);
            break;
        }
        SValue value = eval_expr(runtime, target_id, context, statement->value);
        if (list) {
            sjit_list_push_move(list, value);
        } else {
            sjit_value_destroy_fast(value);
        }
        break;
    }
    case SJIT_STMT_LIST_DELETE: {
        SList *list = script_list(
            runtime,
            target_id,
            context,
            sjit_string_cstr(statement->variable_id),
            sjit_string_cstr(statement->variable_name));
        SValue index_value = eval_expr(runtime, target_id, context, statement->index);
        const int index = sjit_list_to_index(runtime, index_value, sjit_list_length(list), 1);
        sjit_value_destroy_fast(index_value);
        if (!list || index == SJIT_LIST_INDEX_INVALID) {
            break;
        }
        if (index == SJIT_LIST_INDEX_ALL) {
            sjit_list_clear(list);
        } else {
            sjit_list_delete(list, index);
        }
        break;
    }
    case SJIT_STMT_LIST_DELETE_ALL: {
        SList *list = script_list(
            runtime,
            target_id,
            context,
            sjit_string_cstr(statement->variable_id),
            sjit_string_cstr(statement->variable_name));
        sjit_list_clear(list);
        break;
    }
    case SJIT_STMT_LIST_INSERT: {
        SList *list = script_list(
            runtime,
            target_id,
            context,
            sjit_string_cstr(statement->variable_id),
            sjit_string_cstr(statement->variable_name));
        SValue index_value = eval_expr(runtime, target_id, context, statement->index);
        const int index = sjit_list_to_index(runtime, index_value, sjit_list_length(list) + 1, 0);
        sjit_value_destroy_fast(index_value);
        if (!list || index == SJIT_LIST_INDEX_INVALID ||
            index > sjit_list_item_limit(list)) {
            break;
        }
        SValue value = eval_expr(runtime, target_id, context, statement->value);
        sjit_list_insert_move(list, index, value);
        break;
    }
    case SJIT_STMT_LIST_REPLACE: {
        SList *list = script_list(
            runtime,
            target_id,
            context,
            sjit_string_cstr(statement->variable_id),
            sjit_string_cstr(statement->variable_name));
        SValue index_value = eval_expr(runtime, target_id, context, statement->index);
        const int index = sjit_list_to_index(runtime, index_value, sjit_list_length(list), 0);
        sjit_value_destroy_fast(index_value);
        if (!list || index == SJIT_LIST_INDEX_INVALID) {
            break;
        }
        double number = 0.0;
        if (eval_expr_number_for_set(runtime, target_id, context, statement->value, &number)) {
            sjit_list_replace_number(list, index, number);
            break;
        }
        SValue value = eval_expr(runtime, target_id, context, statement->value);
        sjit_list_replace_move(list, index, value);
        break;
    }
    case SJIT_STMT_REPEAT: {
        int branch_count = 0;
        SLoopState *state = statement_loop_state(frame, context, statement, 0);
        do {
            if (!state || !state->branch_active) {
                if (!state) {
                    SValue times_value = sjit_make_number_fast(eval_repeat_count(runtime, target_id, context, statement->times));
                    if (!statement_repeat_should_enter(runtime, frame, context, statement, times_value)) {
                        sjit_value_destroy_fast(times_value);
                        statement_loop_reset(frame, context, statement);
                        break;
                    }
                    sjit_value_destroy_fast(times_value);
                    state = statement_loop_state(frame, context, statement, 0);
                    if (!state) {
                        break;
                    }
                } else {
                    state->counter -= 1.0;
                    if (state->counter < 0.0) {
                        statement_loop_reset(frame, context, statement);
                        break;
                    }
                }
                state->branch_active = 1;
                state->sub_pc = 0;
            }
            SRuntimeStatus status = execute_control_substack(
                runtime,
                target_id,
                context,
                frame,
                state,
                statement->substack,
                statement->substack_count);
            if (status != SJIT_STATUS_OK) {
                return status;
            }
            status = finish_batched_loop_branch(runtime, frame, state, statement, ++branch_count);
            if (status != SJIT_STATUS_OK) {
                return status;
            }
        } while (1);
        break;
    }
    case SJIT_STMT_REPEAT_UNTIL: {
        for (int i = 0; i < 1000000; ++i) {
            SLoopState *state = statement_loop_state(frame, context, statement, 1);
            if (!state) {
                break;
            }
            if (!state->branch_active) {
                const int truthy = eval_condition_bool(runtime, target_id, context, statement->condition);
                if (truthy) {
                    statement_loop_reset(frame, context, statement);
                    break;
                }
                state->branch_active = 1;
                state->sub_pc = 0;
            }
            SRuntimeStatus status = execute_control_substack(
                runtime,
                target_id,
                context,
                frame,
                state,
                statement->substack,
                statement->substack_count);
            if (status != SJIT_STATUS_OK) {
                return status;
            }
            status = finish_control_branch(frame, state, statement);
            if (status != SJIT_STATUS_OK) {
                return status;
            }
        }
        break;
    }
    case SJIT_STMT_WHILE: {
        int branch_count = 0;
        for (int guard = 0; guard < 1000000; ++guard) {
            SLoopState *state = statement_loop_state(frame, context, statement, 1);
            if (!state) {
                break;
            }
            if (!state->branch_active) {
                const int truthy = eval_condition_bool(runtime, target_id, context, statement->condition);
                if (!truthy) {
                    statement_loop_reset(frame, context, statement);
                    break;
                }
                state->branch_active = 1;
                state->sub_pc = 0;
            }
            SRuntimeStatus status = execute_control_substack(
                runtime,
                target_id,
                context,
                frame,
                state,
                statement->substack,
                statement->substack_count);
            if (status != SJIT_STATUS_OK) {
                return status;
            }
            status = finish_batched_loop_branch(runtime, frame, state, statement, ++branch_count);
            if (status != SJIT_STATUS_OK) {
                return status;
            }
        }
        if (branch_count >= 1000000) {
            return SJIT_STATUS_YIELDED;
        }
        break;
    }
    case SJIT_STMT_IF: {
        if (statement_substack_is_synchronous(statement, 0)) {
            if (eval_condition_bool(runtime, target_id, context, statement->condition)) {
                const SRuntimeStatus status = execute_synchronous_substack(
                    runtime,
                    target_id,
                    context,
                    frame,
                    statement->substack,
                    statement->substack_count);
                if (status != SJIT_STATUS_OK) {
                    return status;
                }
            }
            break;
        }
        SLoopState *state = statement_loop_state(frame, context, statement, 1);
        if (!state) {
            break;
        }
        if (!state->branch_active) {
            const int truthy = eval_condition_bool(runtime, target_id, context, statement->condition);
            if (!truthy) {
                statement_loop_reset(frame, context, statement);
                break;
            }
            state->branch_active = 1;
            state->sub_pc = 0;
        }
        SRuntimeStatus status = execute_control_substack(
            runtime,
            target_id,
            context,
            frame,
            state,
            statement->substack,
            statement->substack_count);
        if (status != SJIT_STATUS_OK) {
            return status;
        }
        statement_loop_reset(frame, context, statement);
        break;
    }
    case SJIT_STMT_IF_ELSE: {
        if (statement_substack_is_synchronous(statement, 0) &&
            statement_substack_is_synchronous(statement, 1)) {
            const int use_first = eval_condition_bool(runtime, target_id, context, statement->condition);
            const SRuntimeStatus status = execute_synchronous_substack(
                runtime,
                target_id,
                context,
                frame,
                use_first ? statement->substack : statement->substack2,
                use_first ? statement->substack_count : statement->substack2_count);
            if (status != SJIT_STATUS_OK) {
                return status;
            }
            break;
        }
        SLoopState *state = statement_loop_state(frame, context, statement, 1);
        if (!state) {
            break;
        }
        if (!state->branch_active) {
            const int truthy = eval_condition_bool(runtime, target_id, context, statement->condition);
            state->counter = truthy ? 1.0 : 2.0;
            state->branch_active = 1;
            state->sub_pc = 0;
        }
        const int use_first = state->counter == 1.0;
        SRuntimeStatus status = execute_control_substack(
            runtime,
            target_id,
            context,
            frame,
            state,
            use_first ? statement->substack : statement->substack2,
            use_first ? statement->substack_count : statement->substack2_count);
        if (status != SJIT_STATUS_OK) {
            return status;
        }
        statement_loop_reset(frame, context, statement);
        break;
    }
    case SJIT_STMT_FOREVER: {
        for (int guard = 0; guard < 1000000; ++guard) {
            SLoopState *state = statement_loop_state(frame, context, statement, 1);
            if (!state) {
                break;
            }
            if (!state->branch_active) {
                state->branch_active = 1;
                state->sub_pc = 0;
            }
            SRuntimeStatus status = execute_control_substack(
                runtime,
                target_id,
                context,
                frame,
                state,
                statement->substack,
                statement->substack_count);
            if (status != SJIT_STATUS_OK) {
                return status;
            }
            status = finish_control_branch(frame, state, statement);
            if (status != SJIT_STATUS_OK) {
                return status;
            }
        }
        return SJIT_STATUS_YIELDED;
    }
    case SJIT_STMT_FOR_EACH: {
        if (!frame) {
            break;
        }
        SVariable *variable = lookup_statement_variable(runtime, target_id, statement, SJIT_VAR_SCALAR);
        for (int guard = 0; guard < 1000000; ++guard) {
            SLoopState *state = statement_loop_state(frame, context, statement, 1);
            if (!state) {
                break;
            }
            if (!state->branch_active) {
                double limit = 0.0;
                if (!eval_expr_number(runtime, target_id, context, statement->times, &limit)) {
                    SValue limit_value = eval_expr(runtime, target_id, context, statement->times);
                    limit = sjit_to_number_fast(runtime, limit_value);
                    sjit_value_destroy_fast(limit_value);
                }
                if (state->counter >= limit) {
                    statement_loop_reset(frame, context, statement);
                    break;
                }
                state->counter += 1.0;
                if (variable) {
                    sjit_variable_set_number_fast(variable, state->counter);
                }
                state->branch_active = 1;
                state->sub_pc = 0;
            }
            SRuntimeStatus status = execute_control_substack(
                runtime,
                target_id,
                context,
                frame,
                state,
                statement->substack,
                statement->substack_count);
            if (status != SJIT_STATUS_OK) {
                return status;
            }
            status = finish_control_branch(frame, state, statement);
            if (status != SJIT_STATUS_OK) {
                return status;
            }
        }
        break;
    }
    case SJIT_STMT_PROCEDURE_CALL: {
        SRuntimeStatus status = execute_procedure_call(runtime, target_id, context, frame, statement);
        if (status != SJIT_STATUS_OK) {
            return status;
        }
        break;
    }
    case SJIT_STMT_SAY: {
        SValue value = eval_expr(runtime, target_id, context, statement->value);
        sjit_looks_say(runtime, script_sprite(runtime, target_id, context), value, 0);
        sjit_value_destroy_fast(value);
        break;
    }
    case SJIT_STMT_LOOKS_THINK: {
        SValue value = eval_expr(runtime, target_id, context, statement->value);
        sjit_looks_say(runtime, script_sprite(runtime, target_id, context), value, 1);
        sjit_value_destroy_fast(value);
        break;
    }
    case SJIT_STMT_LOOKS_THINK_FOR_SECS: {
        if (frame && frame->wake_time_ms < 0.0) {
            SValue value = eval_expr(runtime, target_id, context, statement->value);
            SSprite *sprite = script_sprite(runtime, target_id, context);
            sjit_looks_say(runtime, sprite, value, 1);
            const double seconds = eval_expr_number_coerced(
                runtime, target_id, context, statement->index);
            if (sprite) {
                sprite->bubble_until_ms = runtime->now_ms + fmax(0.0, seconds) * 1000.0;
            }
            sjit_value_destroy_fast(value);
        }
        SValue duration = eval_expr(runtime, target_id, context, statement->index);
        SRuntimeStatus status = sjit_control_wait(runtime, frame, duration, frame ? frame->pc : 0);
        sjit_value_destroy_fast(duration);
        if (status != SJIT_STATUS_OK) {
            return status;
        }
        break;
    }
    case SJIT_STMT_PEN_CLEAR:
        sjit_pen_clear(runtime);
        break;
    case SJIT_STMT_PEN_DOWN:
        sjit_pen_down(runtime, script_sprite(runtime, target_id, context));
        break;
    case SJIT_STMT_PEN_UP:
        sjit_pen_up(runtime, script_sprite(runtime, target_id, context));
        break;
    case SJIT_STMT_PEN_STAMP:
        sjit_pen_stamp(runtime, script_sprite(runtime, target_id, context));
        break;
    case SJIT_STMT_PEN_SET_SIZE: {
        double number = 0.0;
        if (eval_expr_number(runtime, target_id, context, statement->value, &number)) {
            sjit_pen_set_size(runtime, script_sprite(runtime, target_id, context), sjit_make_number_fast(number));
            break;
        }
        SValue value = eval_expr(runtime, target_id, context, statement->value);
        sjit_pen_set_size(runtime, script_sprite(runtime, target_id, context), value);
        sjit_value_destroy_fast(value);
        break;
    }
    case SJIT_STMT_PEN_SET_COLOR: {
        double number = 0.0;
        if (eval_expr_number(runtime, target_id, context, statement->value, &number)) {
            sjit_pen_set_color_value(runtime, script_sprite(runtime, target_id, context), sjit_make_number_fast(number));
            break;
        }
        SValue value = eval_expr(runtime, target_id, context, statement->value);
        sjit_pen_set_color_value(runtime, script_sprite(runtime, target_id, context), value);
        sjit_value_destroy_fast(value);
        break;
    }
    case SJIT_STMT_PEN_CHANGE_COLOR_PARAM: {
        if (statement->index && statement->index->opcode == SJIT_EXPR_LITERAL &&
            statement->index->literal.tag == SJIT_VALUE_STRING) {
            if (!statement->pen_color_param_cache_valid) {
                statement->pen_color_param_cache = sjit_pen_color_param_id(
                    sjit_string_cstr((const SString *)statement->index->literal.ptr));
                statement->pen_color_param_cache_valid = 1;
            }
            double number = 0.0;
            if (statement->pen_color_param_cache != 0 &&
                eval_expr_number(runtime, target_id, context, statement->value, &number)) {
                sjit_pen_change_color_param_number(
                    runtime,
                    script_sprite(runtime, target_id, context),
                    statement->pen_color_param_cache,
                    number);
                break;
            }
        }
        SValue param_value = eval_expr(runtime, target_id, context, statement->index);
        SValue param_text = sjit_to_string(runtime, param_value);
        SValue value = eval_expr(runtime, target_id, context, statement->value);
        sjit_pen_change_color_param(
            runtime,
            script_sprite(runtime, target_id, context),
            sjit_string_cstr((const SString *)param_text.ptr),
            value);
        sjit_value_destroy_fast(param_value);
        sjit_value_destroy_fast(param_text);
        sjit_value_destroy_fast(value);
        break;
    }
    case SJIT_STMT_PEN_SET_COLOR_PARAM: {
        SValue param = eval_expr(runtime, target_id, context, statement->index);
        SValue param_text = sjit_to_string(runtime, param);
        sjit_pen_set_color_param_number(
            runtime,
            script_sprite(runtime, target_id, context),
            sjit_pen_color_param_id(sjit_string_cstr((const SString *)param_text.ptr)),
            eval_expr_number_coerced(runtime, target_id, context, statement->value));
        sjit_value_destroy_fast(param);
        sjit_value_destroy_fast(param_text);
        break;
    }
    case SJIT_STMT_PEN_CHANGE_SIZE:
        sjit_pen_change_size(
            runtime,
            script_sprite(runtime, target_id, context),
            eval_expr_number_coerced(runtime, target_id, context, statement->value));
        break;
    case SJIT_STMT_PEN_SET_SHADE:
        sjit_pen_set_shade_number(
            runtime,
            script_sprite(runtime, target_id, context),
            eval_expr_number_coerced(runtime, target_id, context, statement->value));
        break;
    case SJIT_STMT_PEN_CHANGE_SHADE:
        sjit_pen_change_shade_number(
            runtime,
            script_sprite(runtime, target_id, context),
            eval_expr_number_coerced(runtime, target_id, context, statement->value));
        break;
    case SJIT_STMT_PEN_SET_HUE:
        sjit_pen_set_hue_number(
            runtime,
            script_sprite(runtime, target_id, context),
            eval_expr_number_coerced(runtime, target_id, context, statement->value));
        break;
    case SJIT_STMT_PEN_CHANGE_HUE:
        sjit_pen_change_hue_number(
            runtime,
            script_sprite(runtime, target_id, context),
            eval_expr_number_coerced(runtime, target_id, context, statement->value));
        break;
    case SJIT_STMT_MOTION_GOTO_XY: {
        SSprite *sprite = script_sprite(runtime, target_id, context);
        if (sprite) {
            const double x = eval_expr_number_coerced(runtime, target_id, context, statement->value);
            const double y = eval_expr_number_coerced(runtime, target_id, context, statement->index);
            sjit_sprite_set_xy(runtime, sprite, x, y, 0);
        }
        break;
    }
    case SJIT_STMT_MOTION_SET_X: {
        SSprite *sprite = script_sprite(runtime, target_id, context);
        if (sprite) {
            sjit_sprite_set_xy(
                runtime,
                sprite,
                eval_expr_number_coerced(runtime, target_id, context, statement->value),
                sprite->y,
                0);
        }
        break;
    }
    case SJIT_STMT_MOTION_SET_Y: {
        SSprite *sprite = script_sprite(runtime, target_id, context);
        if (sprite) {
            sjit_sprite_set_xy(
                runtime,
                sprite,
                sprite->x,
                eval_expr_number_coerced(runtime, target_id, context, statement->value),
                0);
        }
        break;
    }
    case SJIT_STMT_MOTION_CHANGE_X: {
        SSprite *sprite = script_sprite(runtime, target_id, context);
        if (sprite) {
            sjit_sprite_set_xy(
                runtime,
                sprite,
                sprite->x + eval_expr_number_coerced(runtime, target_id, context, statement->value),
                sprite->y,
                0);
        }
        break;
    }
    case SJIT_STMT_MOTION_CHANGE_Y: {
        SSprite *sprite = script_sprite(runtime, target_id, context);
        if (sprite) {
            sjit_sprite_set_xy(
                runtime,
                sprite,
                sprite->x,
                sprite->y + eval_expr_number_coerced(runtime, target_id, context, statement->value),
                0);
        }
        break;
    }
    case SJIT_STMT_MOTION_MOVE_STEPS: {
        SValue steps = eval_expr(runtime, target_id, context, statement->value);
        sjit_motion_move_steps(runtime, script_sprite(runtime, target_id, context), steps);
        sjit_value_destroy_fast(steps);
        break;
    }
    case SJIT_STMT_MOTION_GOTO: {
        SValue destination = eval_expr(runtime, target_id, context, statement->value);
        sjit_motion_goto(runtime, script_sprite(runtime, target_id, context), destination);
        sjit_value_destroy_fast(destination);
        break;
    }
    case SJIT_STMT_MOTION_TURN_RIGHT:
    case SJIT_STMT_MOTION_TURN_LEFT: {
        SValue degrees = eval_expr(runtime, target_id, context, statement->value);
        sjit_motion_turn(
            runtime,
            script_sprite(runtime, target_id, context),
            degrees,
            statement->opcode == SJIT_STMT_MOTION_TURN_RIGHT);
        sjit_value_destroy_fast(degrees);
        break;
    }
    case SJIT_STMT_MOTION_POINT_DIRECTION: {
        SValue direction = eval_expr(runtime, target_id, context, statement->value);
        sjit_motion_point_in_direction(
            runtime,
            script_sprite(runtime, target_id, context),
            direction);
        sjit_value_destroy_fast(direction);
        break;
    }
    case SJIT_STMT_MOTION_POINT_TOWARDS: {
        SValue destination = eval_expr(runtime, target_id, context, statement->value);
        sjit_motion_point_towards(
            runtime,
            script_sprite(runtime, target_id, context),
            destination);
        sjit_value_destroy_fast(destination);
        break;
    }
    case SJIT_STMT_MOTION_GLIDE_XY: {
        const double seconds = eval_expr_number_coerced(
            runtime, target_id, context, statement->value);
        const double x = eval_expr_number_coerced(
            runtime, target_id, context, statement->index);
        const double y = eval_expr_number_coerced(
            runtime, target_id, context, statement->condition);
        const SRuntimeStatus status = sjit_motion_glide_to_xy(
            runtime,
            script_sprite(runtime, target_id, context),
            frame,
            statement,
            seconds,
            x,
            y,
            frame ? frame->pc : 0);
        if (status != SJIT_STATUS_OK) {
            return status;
        }
        break;
    }
    case SJIT_STMT_MOTION_GLIDE_TO: {
        const double seconds = eval_expr_number_coerced(
            runtime, target_id, context, statement->value);
        SValue destination = eval_expr(runtime, target_id, context, statement->index);
        const SRuntimeStatus status = sjit_motion_glide_to(
            runtime,
            script_sprite(runtime, target_id, context),
            frame,
            statement,
            seconds,
            destination,
            frame ? frame->pc : 0);
        sjit_value_destroy_fast(destination);
        if (status != SJIT_STATUS_OK) {
            return status;
        }
        break;
    }
    case SJIT_STMT_MOTION_IF_ON_EDGE_BOUNCE:
        sjit_motion_if_on_edge_bounce(runtime, script_sprite(runtime, target_id, context));
        break;
    case SJIT_STMT_MOTION_SET_ROTATION_STYLE: {
        SValue style = eval_expr(runtime, target_id, context, statement->value);
        SValue text = sjit_to_string(runtime, style);
        sjit_motion_set_rotation_style(
            script_sprite(runtime, target_id, context),
            sjit_string_cstr((const SString *)text.ptr));
        sjit_value_destroy_fast(style);
        sjit_value_destroy_fast(text);
        break;
    }
    case SJIT_STMT_LOOKS_SHOW:
        sjit_looks_show(runtime, script_sprite(runtime, target_id, context));
        break;
    case SJIT_STMT_LOOKS_HIDE:
        sjit_looks_hide(runtime, script_sprite(runtime, target_id, context));
        break;
    case SJIT_STMT_LOOKS_SWITCH_COSTUME: {
        SValue costume = eval_expr(
            runtime,
            target_id,
            context,
            statement->value);
        sjit_looks_switch_costume(
            runtime,
            script_sprite(runtime, target_id, context),
            costume);
        sjit_value_destroy_fast(costume);
        break;
    }
    case SJIT_STMT_LOOKS_GO_TO_FRONT_BACK:
        sjit_looks_go_to_front_back(
            runtime,
            script_sprite(runtime, target_id, context),
            statement->layer_front);
        break;
    case SJIT_STMT_LOOKS_SWITCH_BACKDROP: {
        SValue backdrop = eval_expr(
            runtime,
            target_id,
            context,
            statement->value);
        sjit_looks_switch_backdrop(runtime, backdrop);
        sjit_value_destroy_fast(backdrop);
        break;
    }
    case SJIT_STMT_LOOKS_SET_EFFECT:
    case SJIT_STMT_LOOKS_CHANGE_EFFECT: {
        SSprite *sprite = script_sprite(runtime, target_id, context);
        const double value = eval_expr_number_coerced(
            runtime,
            target_id,
            context,
            statement->value);
        if (statement->opcode == SJIT_STMT_LOOKS_SET_EFFECT) {
            sjit_looks_set_effect_number(
                runtime,
                sprite,
                statement->looks_effect_cache_valid ? statement->looks_effect : -1,
                value);
        } else {
            sjit_looks_change_effect_number(
                runtime,
                sprite,
                statement->looks_effect_cache_valid ? statement->looks_effect : -1,
                value);
        }
        break;
    }
    case SJIT_STMT_LOOKS_CLEAR_EFFECTS:
        sjit_looks_clear_effects(
            runtime,
            script_sprite(runtime, target_id, context));
        break;
    case SJIT_STMT_SENSING_SET_DRAG_MODE:
        sjit_sprite_set_draggable(
            script_sprite(runtime, target_id, context),
            statement->drag_mode);
        break;
    case SJIT_STMT_MONITOR_SHOW:
    case SJIT_STMT_MONITOR_HIDE:
        sjit_runtime_set_variable_monitor_visible(
            runtime,
            statement->variable_id ? sjit_string_cstr(statement->variable_id) : "",
            statement->opcode == SJIT_STMT_MONITOR_SHOW);
        break;
    case SJIT_STMT_LOOKS_SET_SIZE: {
        SSprite *sprite = script_sprite(runtime, target_id, context);
        SValue value = eval_expr(runtime, target_id, context, statement->value);
        if (sprite && !sprite->base.is_stage) {
            const double size = sjit_to_number_fast(runtime, value);
            sprite->size = isfinite(size) ? fmax(0.0, size) : 0.0;
            sjit_runtime_request_redraw(runtime);
        }
        sjit_value_destroy_fast(value);
        break;
    }
    case SJIT_STMT_LOOKS_SWITCH_BACKDROP_AND_WAIT: {
        SValue backdrop = eval_expr(runtime, target_id, context, statement->value);
        SRuntimeStatus status = SJIT_STATUS_OK;
        sjit_looks_switch_backdrop_and_wait(
            runtime,
            frame,
            backdrop,
            frame ? frame->pc : 0,
            &status);
        sjit_value_destroy_fast(backdrop);
        if (status != SJIT_STATUS_OK) {
            return status;
        }
        break;
    }
    case SJIT_STMT_LOOKS_NEXT_COSTUME:
        sjit_looks_next_costume(runtime, script_sprite(runtime, target_id, context));
        break;
    case SJIT_STMT_LOOKS_NEXT_BACKDROP:
        sjit_looks_next_backdrop(runtime);
        break;
    case SJIT_STMT_LOOKS_CHANGE_SIZE:
        sjit_looks_change_size(
            runtime,
            script_sprite(runtime, target_id, context),
            eval_expr_number_coerced(runtime, target_id, context, statement->value));
        break;
    case SJIT_STMT_LOOKS_HIDE_ALL_SPRITES:
        sjit_looks_hide_all_sprites(runtime);
        break;
    case SJIT_STMT_LOOKS_GO_FORWARD_BACKWARD_LAYERS: {
        double amount = eval_expr_number_coerced(
            runtime, target_id, context, statement->value);
        if (!isfinite(amount)) {
            amount = 0.0;
        }
        if (amount > (double)INT_MAX) amount = (double)INT_MAX;
        if (amount < (double)INT_MAX * -1.0) amount = (double)INT_MIN;
        int layers = (int)amount;
        if (statement->layer_front < 0) layers = -layers;
        sjit_looks_go_forward_backward_layers(
            runtime,
            script_sprite(runtime, target_id, context),
            layers);
        break;
    }
    case SJIT_STMT_LOOKS_CHANGE_STRETCH:
        sjit_looks_change_stretch(
            runtime,
            script_sprite(runtime, target_id, context),
            eval_expr_number_coerced(runtime, target_id, context, statement->value));
        break;
    case SJIT_STMT_LOOKS_SET_STRETCH:
        sjit_looks_set_stretch(
            runtime,
            script_sprite(runtime, target_id, context),
            eval_expr_number_coerced(runtime, target_id, context, statement->value));
        break;
    case SJIT_STMT_CREATE_CLONE: {
        SValue requested = eval_expr(runtime, target_id, context, statement->value);
        sjit_clone_create_requested(
            runtime,
            script_sprite(runtime, target_id, context),
            requested);
        sjit_value_destroy_fast(requested);
        break;
    }
    case SJIT_STMT_DELETE_CLONE: {
        SSprite *sprite = script_sprite(runtime, target_id, context);
        if (sprite && !sprite->base.is_original) {
            sjit_clone_delete(runtime, sprite);
            return SJIT_STATUS_DONE;
        }
        break;
    }
    case SJIT_STMT_CONTROL_INCR_COUNTER:
        if (runtime->counter < INT_MAX) {
            ++runtime->counter;
        }
        break;
    case SJIT_STMT_CONTROL_CLEAR_COUNTER:
        runtime->counter = 0;
        break;
    case SJIT_STMT_CONTROL_ALL_AT_ONCE: {
        const int previous_warp_mode = frame ? frame->warp_mode : 0;
        if (frame) {
            frame->warp_mode = 1;
        }
        const SRuntimeStatus status = execute_synchronous_substack(
            runtime,
            target_id,
            context,
            frame,
            statement->substack,
            statement->substack_count);
        if (frame) {
            frame->warp_mode = previous_warp_mode;
        }
        if (status != SJIT_STATUS_OK) {
            return status;
        }
        break;
    }
    case SJIT_STMT_SENSING_ASK_AND_WAIT: {
        if (frame && frame->timed_event_kind == 5 && frame->timed_statement == statement) {
            if (!runtime->answer_ready) {
                sjit_runtime_request_redraw(runtime);
                return SJIT_STATUS_YIELDED;
            }
            frame->timed_event_kind = 0;
            frame->timed_statement = NULL;
            sjit_string_destroy(runtime->question);
            runtime->question = NULL;
            break;
        }
        SValue question = eval_expr(runtime, target_id, context, statement->value);
        SValue text = sjit_to_string(runtime, question);
        sjit_string_destroy(runtime->question);
        runtime->question = sjit_string_new(
            sjit_string_cstr((const SString *)text.ptr));
        runtime->answer_ready = runtime->ask_input_enabled ? 0 : 1;
        printf("ask: %s\n", sjit_string_cstr((const SString *)text.ptr));
        sjit_value_destroy_fast(question);
        sjit_value_destroy_fast(text);
        if (frame && runtime->ask_input_enabled) {
            frame->timed_event_kind = 5;
            frame->timed_statement = statement;
            sjit_runtime_request_redraw(runtime);
            return SJIT_STATUS_YIELDED;
        }
        break;
    }
    case SJIT_STMT_SOUND_PLAY:
    case SJIT_STMT_SOUND_PLAY_UNTIL_DONE: {
        if (statement->opcode == SJIT_STMT_SOUND_PLAY_UNTIL_DONE && frame &&
            frame->timed_event_kind == 4 && frame->timed_statement == statement) {
            frame->timed_event_kind = 0;
            frame->timed_statement = NULL;
            break;
        }
        SValue sound = eval_expr(runtime, target_id, context, statement->value);
        sjit_sound_play(
            runtime,
            script_sprite(runtime, target_id, context),
            sound,
            statement->opcode == SJIT_STMT_SOUND_PLAY_UNTIL_DONE);
        sjit_value_destroy_fast(sound);
        if (statement->opcode == SJIT_STMT_SOUND_PLAY_UNTIL_DONE && frame) {
            frame->timed_event_kind = 4;
            frame->timed_statement = statement;
            return SJIT_STATUS_YIELDED;
        }
        break;
    }
    case SJIT_STMT_SOUND_STOP_ALL:
        sjit_sound_stop_all(runtime);
        break;
    case SJIT_STMT_SOUND_SET_EFFECT:
    case SJIT_STMT_SOUND_CHANGE_EFFECT: {
        SValue effect = eval_expr(runtime, target_id, context, statement->index);
        sjit_sound_set_effect(
            runtime,
            script_sprite(runtime, target_id, context),
            effect,
            eval_expr_number_coerced(runtime, target_id, context, statement->value),
            statement->opcode == SJIT_STMT_SOUND_CHANGE_EFFECT);
        sjit_value_destroy_fast(effect);
        break;
    }
    case SJIT_STMT_SOUND_CLEAR_EFFECTS:
        sjit_sound_clear_effects(runtime, script_sprite(runtime, target_id, context));
        break;
    case SJIT_STMT_SOUND_SET_VOLUME:
    case SJIT_STMT_SOUND_CHANGE_VOLUME:
        sjit_sound_set_volume(
            runtime,
            script_sprite(runtime, target_id, context),
            eval_expr_number_coerced(runtime, target_id, context, statement->value),
            statement->opcode == SJIT_STMT_SOUND_CHANGE_VOLUME);
        break;
    case SJIT_STMT_STOP_THIS_SCRIPT:
        return SJIT_STATUS_DONE;
    case SJIT_STMT_NOOP:
    default:
        break;
    }
    return SJIT_STATUS_OK;
}

SRuntimeStatus sjit_script_execute_statement(SRuntime *runtime, SCompiledScript *script, int statement_index) {
    SFrame frame;
    sjit_frame_init(&frame);
    return sjit_script_execute_statement_with_frame(runtime, script, statement_index, &frame);
}

SRuntimeStatus sjit_script_execute_statement_with_frame(
    SRuntime *runtime,
    SCompiledScript *script,
    int statement_index,
    SFrame *frame) {
    if (!runtime || !script || statement_index < 0 || statement_index >= script->statement_count) {
        return SJIT_STATUS_ERROR;
    }
    SExecContext context;
    init_exec_context(&context, script, NULL);
    const SRuntimeStatus status = execute_statement(
        runtime,
        script->target_id,
        &context,
        frame,
        &script->statements[statement_index]);
    destroy_exec_context(&context);
    return status;
}

SRuntimeStatus sjit_script_execute_statement_ptr_with_thread(
    SRuntime *runtime,
    SThread *thread,
    SFrame *frame,
    SCompiledScript *script,
    SStatement *statement) {
    if (!runtime || !script || !statement) {
        return SJIT_STATUS_ERROR;
    }
    SExecContext context;
    init_exec_context(&context, script, thread);
    SRuntimeStatus status = execute_statement(runtime, script->target_id, &context, frame, statement);
    destroy_exec_context(&context);
    return status;
}

SRuntimeStatus sjit_script_execute_procedure_statement(
    SRuntime *runtime,
    SThread *thread,
    SFrame *frame,
    SCompiledScript *script,
    SStatement *statement) {
    if (!runtime || !script || !statement || statement->opcode != SJIT_STMT_PROCEDURE_CALL) {
        return SJIT_STATUS_ERROR;
    }
    SExecContext context;
    init_exec_context(&context, script, thread);
    const SRuntimeStatus status = execute_procedure_call(
        runtime,
        script->target_id,
        &context,
        frame,
        statement);
    destroy_exec_context(&context);
    return status;
}

SValue sjit_script_eval_statement_expr(
    SRuntime *runtime,
    SCompiledScript *script,
    int statement_index,
    int expr_slot) {
    if (!runtime || !script || statement_index < 0 || statement_index >= script->statement_count) {
        return sjit_make_null_fast();
    }
    return sjit_script_eval_statement_expr_ptr(runtime, script, &script->statements[statement_index], expr_slot);
}

static SExpr *statement_expr_for_slot(SStatement *statement, int expr_slot) {
    if (!statement) {
        return NULL;
    }
    SExpr *expr = NULL;
    switch (expr_slot) {
    case SJIT_STMT_EXPR_VALUE:
        expr = statement->value;
        break;
    case SJIT_STMT_EXPR_INDEX:
        expr = statement->index;
        break;
    case SJIT_STMT_EXPR_CONDITION:
        expr = statement->condition;
        break;
    case SJIT_STMT_EXPR_TIMES:
        expr = statement->times;
        break;
    default:
        break;
    }
    return expr;
}

SValue sjit_script_eval_statement_expr_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    int expr_slot) {
    if (!runtime || !script || !statement) {
        return sjit_make_null_fast();
    }
    SExpr *expr = statement_expr_for_slot(statement, expr_slot);
    if (!expr) {
        return sjit_make_null_fast();
    }
    SExecContext context;
    init_exec_context(&context, script, NULL);
    SValue value = eval_expr(runtime, script->target_id, &context, expr);
    destroy_exec_context(&context);
    return value;
}

SValue sjit_script_eval_expr(
    SRuntime *runtime,
    SCompiledScript *script,
    SExpr *expr) {
    if (!runtime || !script || !expr) {
        return sjit_make_null_fast();
    }
    SExecContext context;
    init_exec_context(&context, script, NULL);
    SValue value = eval_expr(runtime, script->target_id, &context, expr);
    destroy_exec_context(&context);
    return value;
}

double sjit_script_eval_statement_number(
    SRuntime *runtime,
    SCompiledScript *script,
    int statement_index,
    int expr_slot) {
    if (runtime && script && statement_index >= 0 && statement_index < script->statement_count) {
        return sjit_script_eval_statement_number_ptr(runtime, script, &script->statements[statement_index], expr_slot);
    }
    SValue value = sjit_script_eval_statement_expr(runtime, script, statement_index, expr_slot);
    double number = sjit_to_number_fast(runtime, value);
    sjit_value_destroy_fast(value);
    return number;
}

double sjit_script_eval_statement_number_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    int expr_slot) {
    if (runtime && script && statement) {
        SExpr *expr = statement_expr_for_slot(statement, expr_slot);
        SExecContext context;
        init_exec_context(&context, script, NULL);
        double number = 0.0;
        const int did_eval = eval_expr_number(runtime, script->target_id, &context, expr, &number);
        destroy_exec_context(&context);
        if (did_eval) {
            return number;
        }
    }
    SValue value = sjit_script_eval_statement_expr_ptr(runtime, script, statement, expr_slot);
    double number = sjit_to_number_fast(runtime, value);
    sjit_value_destroy_fast(value);
    return number;
}

int sjit_script_eval_statement_bool_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    int expr_slot) {
    if (runtime && script && statement) {
        SExpr *expr = statement_expr_for_slot(statement, expr_slot);
        SExecContext context;
        init_exec_context(&context, script, NULL);
        int truthy = 0;
        const int did_eval = eval_expr_bool(runtime, script->target_id, &context, expr, &truthy);
        destroy_exec_context(&context);
        if (did_eval) {
            return truthy;
        }
    }
    SValue value = sjit_script_eval_statement_expr_ptr(runtime, script, statement, expr_slot);
    const int truthy = sjit_to_bool(runtime, value);
    sjit_value_destroy_fast(value);
    return truthy;
}

double sjit_script_value_ptr_to_number(SRuntime *runtime, SValue *value) {
    if (!value) {
        return 0.0;
    }
    return sjit_to_number_fast(runtime, *value);
}

void sjit_script_destroy_value_ptr(SValue *value) {
    if (!value) {
        return;
    }
    sjit_value_destroy_fast(*value);
    *value = sjit_make_null_fast();
}

SRuntimeStatus sjit_script_interpreter_entry(SRuntime *runtime, SThread *thread, SFrame *frame) {
    if (!runtime || !thread || !thread->script_data || !frame) {
        return SJIT_STATUS_ERROR;
    }
    SCompiledScript *script = (SCompiledScript *)thread->script_data;
    SExecContext context;
    init_exec_context(&context, script, thread);
    int start = frame->pc;
    if (start < 0 || start >= script->statement_count) {
        start = 0;
    }
    for (int i = start; i < script->statement_count; ++i) {
        if (execute_adjacent_pen_stamp(
                runtime,
                thread->target_id,
                &context,
                script->statements,
                script->statement_count,
                i)) {
            frame->pc = i + 2;
            ++i;
            continue;
        }
        if (execute_adjacent_pen_color_change(
                runtime,
                thread->target_id,
                &context,
                script->statements,
                script->statement_count,
                i)) {
            frame->pc = i + 2;
            ++i;
            continue;
        }
        SRuntimeStatus status = execute_statement(runtime, thread->target_id, &context, frame, &script->statements[i]);
        if (status == SJIT_STATUS_YIELDED || status == SJIT_STATUS_YIELD_TICK || status == SJIT_STATUS_WAITING) {
            frame->pc = i;
            destroy_exec_context(&context);
            return status;
        }
        if (status != SJIT_STATUS_OK) {
            destroy_exec_context(&context);
            return status;
        }
        frame->pc = i + 1;
    }
    frame->finished = 1;
    destroy_exec_context(&context);
    return SJIT_STATUS_DONE;
}
