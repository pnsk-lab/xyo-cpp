#include "sjit_jit_bridge.h"

#include "sjit_compare.h"
#include "sjit_control.h"
#include "sjit_list.h"
#include "sjit_list_internal.h"
#include "sjit_number.h"
#include "sjit_operator.h"
#include "sjit_pen.h"
#include "sjit_runtime.h"
#include "sjit_string.h"
#include "sjit_thread.h"
#include "sjit_value.h"
#include "sjit_variable.h"

#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

enum {
    SJIT_LOOP_BRANCH_BATCH = 16384
};

static void jit_variable_set_string_borrowed(
    SRuntime *runtime,
    SVariable *destination,
    SString *source) {
    if (!destination || !source) {
        return;
    }
    SValue value;
    value.tag = SJIT_VALUE_STRING;
    value.number = 0.0;
    value.ptr = source;
    if (destination->scalar_kind == SJIT_SCALAR_BOOL) {
        sjit_variable_set_bool(destination, sjit_to_bool(runtime, value));
        return;
    }
    if (destination->value.tag == SJIT_VALUE_STRING &&
        destination->value.ptr == source) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VARIABLE_SET_SAME_STRING);
        return;
    }
    ++source->ref_count;
    const SValue previous = destination->value;
    destination->value = value;
    sjit_value_destroy_fast(previous);
}

static void jit_variable_set_borrowed(
    SRuntime *runtime,
    SVariable *destination,
    SValue source);
static void jit_list_push_borrowed(SList *list, SValue source);

static int jit_key_index_for_name(const char *name) {
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

static int jit_key_index_for_number(double number) {
    if (!isfinite(number) || floor(number) != number) {
        return -1;
    }
    if (number >= 48.0 && number <= 90.0) {
        const int value = (int)number;
        return value >= 'a' && value <= 'z' ? value - ('a' - 'A') : value;
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

static SStatement *jit_statement(SCompiledScript *script, int statement_index) {
    if (!script || statement_index < 0 || statement_index >= script->statement_count) {
        return 0;
    }
    return &script->statements[statement_index];
}

static const char *jit_statement_variable_name(const SStatement *statement) {
    return statement ? sjit_string_cstr(statement->variable_name) : "";
}

static const char *jit_statement_variable_id(const SStatement *statement) {
    return statement ? sjit_string_cstr(statement->variable_id) : "";
}

static int jit_variable_reference_matches(
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

static const SString *jit_variable_reference_identity(
    const SVariable *variable,
    const char *scratch_id) {
    if (!variable) {
        return NULL;
    }
    return scratch_id && scratch_id[0] != '\0' ?
        variable->scratch_id : variable->name;
}

static int jit_variable_cache_matches(
    const SVariable *variable,
    const SString *identity,
    const char *scratch_id,
    const char *name,
    int type) {
    if (!variable || variable->type != type) {
        return 0;
    }
    if (identity) {
        return jit_variable_reference_identity(variable, scratch_id) == identity;
    }
    return jit_variable_reference_matches(variable, scratch_id, name, type);
}

static int expr_allows_number_set_fast_path(const SExpr *expr) {
    if (!expr) {
        return 0;
    }
    switch (expr->opcode) {
    case SJIT_EXPR_LITERAL:
        return expr->literal.tag != SJIT_VALUE_STRING && expr->literal.tag != SJIT_VALUE_LIST;
    case SJIT_EXPR_TIMER:
    case SJIT_EXPR_MOUSE_X:
    case SJIT_EXPR_MOUSE_Y:
    case SJIT_EXPR_ADD:
    case SJIT_EXPR_SUB:
    case SJIT_EXPR_MUL:
    case SJIT_EXPR_DIV:
    case SJIT_EXPR_MOD:
    case SJIT_EXPR_RANDOM:
    case SJIT_EXPR_ROUND:
    case SJIT_EXPR_LENGTH:
    case SJIT_EXPR_LIST_ITEM:
    case SJIT_EXPR_LIST_ITEM_NUMBER:
    case SJIT_EXPR_LIST_LENGTH:
        return 1;
    default:
        return 0;
    }
}

static SSprite *jit_script_sprite(SRuntime *runtime, SCompiledScript *script) {
    if (!script) {
        return 0;
    }
    return script->bound_target ?
        script->bound_target : sjit_runtime_get_sprite(runtime, script->target_id);
}

static SVariable *jit_statement_variable(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    int type) {
    if (!runtime || !script || !statement) {
        return 0;
    }
    if (statement->variable_cache_target_id == script->target_id &&
        statement->variable_cache_type == type &&
        statement->variable_cache_runtime == runtime &&
        runtime->instance_id != 0 &&
        statement->variable_cache_runtime_instance_id == runtime->instance_id &&
        statement->variable_cache_owner_target_id > 0 &&
        statement->variable_cache_index >= 0) {
        SSprite *owner = sjit_runtime_get_sprite(
            runtime, statement->variable_cache_owner_target_id);
        if (owner && statement->variable_cache_index < owner->base.variable_count) {
            SVariable *variable = &owner->base.variables[statement->variable_cache_index];
            if (jit_variable_cache_matches(
                    variable,
                    statement->variable_cache_identity,
                    jit_statement_variable_id(statement),
                    jit_statement_variable_name(statement),
                    type)) {
                return variable;
            }
        }
        statement->variable_cache_owner = NULL;
        statement->variable_cache_owner_target_id = 0;
        statement->variable_cache_index = 0;
        statement->variable_cache_owner_is_original = 0;
        statement->variable_cache_runtime = NULL;
        statement->variable_cache_runtime_instance_id = 0;
        statement->variable_cache_identity = NULL;
    }

    const char *id = jit_statement_variable_id(statement);
    const char *name = jit_statement_variable_name(statement);
    SVariable *variable = sjit_runtime_lookup_or_create_variable_by_scratch_id(
        runtime, script->target_id, id, name, type);
    if (!variable) {
        return 0;
    }
    for (int target_index = 0; target_index < runtime->target_count; ++target_index) {
        SSprite *owner = runtime->targets[target_index];
        for (int variable_index = 0; owner && variable_index < owner->base.variable_count; ++variable_index) {
            if (&owner->base.variables[variable_index] == variable) {
                statement->variable_cache_target_id = script->target_id;
                statement->variable_cache_owner = NULL;
                statement->variable_cache_owner_target_id = owner->base.id;
                statement->variable_cache_index = variable_index;
                statement->variable_cache_type = type;
                statement->variable_cache_owner_is_original = owner->base.is_original;
                statement->variable_cache_runtime = runtime;
                statement->variable_cache_runtime_instance_id = runtime->instance_id;
                statement->variable_cache_identity = jit_variable_reference_identity(variable, id);
                return variable;
            }
        }
    }
    return variable;
}

static SList *jit_script_list(SRuntime *runtime, SCompiledScript *script, SStatement *statement) {
    if (!script || !statement) {
        return 0;
    }
    SVariable *variable = jit_statement_variable(runtime, script, statement, SJIT_VAR_LIST);
    if (!variable || variable->value.tag != SJIT_VALUE_LIST || !variable->value.ptr) {
        return 0;
    }
    return (SList *)variable->value.ptr;
}

static SVariable *jit_expr_named_variable(SRuntime *runtime, int target_id, SExpr *expr, int type) {
    if (!runtime || !expr) {
        return 0;
    }
    if (expr->variable_cache_target_id == target_id &&
        expr->variable_cache_type == type &&
        expr->variable_cache_runtime == runtime &&
        runtime->instance_id != 0 &&
        expr->variable_cache_runtime_instance_id == runtime->instance_id &&
        expr->variable_cache_owner_target_id > 0 &&
        expr->variable_cache_index >= 0) {
        SSprite *owner = sjit_runtime_get_sprite(
            runtime, expr->variable_cache_owner_target_id);
        if (owner && expr->variable_cache_index < owner->base.variable_count) {
            SVariable *variable = &owner->base.variables[expr->variable_cache_index];
            if (jit_variable_cache_matches(
                    variable,
                    expr->variable_cache_identity,
                    sjit_string_cstr(expr->variable_id),
                    sjit_string_cstr((const SString *)expr->literal.ptr),
                    type)) {
                return variable;
            }
        }
        expr->variable_cache_owner = NULL;
        expr->variable_cache_owner_target_id = 0;
        expr->variable_cache_index = 0;
        expr->variable_cache_owner_is_original = 0;
        expr->variable_cache_runtime = NULL;
        expr->variable_cache_runtime_instance_id = 0;
        expr->variable_cache_identity = NULL;
    }

    const char *id = sjit_string_cstr(expr->variable_id);
    const char *name = sjit_string_cstr((const SString *)expr->literal.ptr);
    SVariable *variable = sjit_runtime_lookup_or_create_variable_by_scratch_id(
        runtime, target_id, id, name, type);
    if (!variable) {
        return 0;
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
                expr->variable_cache_identity = jit_variable_reference_identity(variable, id);
                return variable;
            }
        }
    }
    return variable;
}

static SVariable *jit_expr_variable(SRuntime *runtime, int target_id, SExpr *expr, int type) {
    if (!expr || expr->opcode != SJIT_EXPR_VARIABLE) {
        return 0;
    }
    return jit_expr_named_variable(runtime, target_id, expr, type);
}

static SList *jit_expr_list(SRuntime *runtime, int target_id, SExpr *expr) {
    SVariable *variable = jit_expr_named_variable(runtime, target_id, expr, SJIT_VAR_LIST);
    if (!variable || variable->value.tag != SJIT_VALUE_LIST || !variable->value.ptr) {
        return 0;
    }
    return (SList *)variable->value.ptr;
}

static SList *jit_list_from_variable(SVariable *variable) {
    if (!variable || variable->type != SJIT_VAR_LIST ||
        variable->value.tag != SJIT_VALUE_LIST || !variable->value.ptr) {
        return 0;
    }
    return (SList *)variable->value.ptr;
}

void *sjit_jit_thread_script_data(SThread *thread) {
    return thread ? thread->script_data : 0;
}

int sjit_jit_frame_pc(SFrame *frame) {
    return frame ? frame->pc : 0;
}

void sjit_jit_frame_set_pc(SFrame *frame, int pc) {
    if (frame) {
        frame->pc = pc;
    }
}

void sjit_jit_frame_mark_finished(SFrame *frame) {
    if (frame) {
        frame->finished = 1;
    }
}

double sjit_jit_sprite_x(SSprite *sprite) {
    return sprite ? sprite->x : 0.0;
}

double sjit_jit_sprite_y(SSprite *sprite) {
    return sprite ? sprite->y : 0.0;
}

void sjit_jit_pen_set_size_number(SRuntime *runtime, SSprite *sprite, double size) {
    (void)runtime;
    if (!sprite) {
        return;
    }
    sprite->pen_size = size < 1.0 ? 1.0 : size;
}

void sjit_jit_sprite_set_size(SRuntime *runtime, SSprite *sprite, double size) {
    if (!sprite) {
        return;
    }
    sprite->size = size;
    sjit_runtime_request_redraw(runtime);
}

void sjit_jit_reset_timer(SRuntime *runtime) {
    if (runtime) {
        runtime->timer_start_ms = runtime->now_ms;
    }
}

SVariable *sjit_jit_statement_scalar_variable(SRuntime *runtime, SCompiledScript *script, int statement_index) {
    SStatement *statement = jit_statement(script, statement_index);
    return jit_statement_variable(runtime, script, statement, SJIT_VAR_SCALAR);
}

SVariable *sjit_jit_statement_scalar_variable_ptr(SRuntime *runtime, SCompiledScript *script, SStatement *statement) {
    return jit_statement_variable(runtime, script, statement, SJIT_VAR_SCALAR);
}

SVariable *sjit_jit_statement_list_variable_ptr(SRuntime *runtime, SCompiledScript *script, SStatement *statement) {
    return jit_statement_variable(runtime, script, statement, SJIT_VAR_LIST);
}

SVariable *sjit_jit_expr_scalar_variable(SRuntime *runtime, int target_id, SExpr *expr) {
    return jit_expr_variable(runtime, target_id, expr, SJIT_VAR_SCALAR);
}

SVariable *sjit_jit_expr_list_variable(SRuntime *runtime, int target_id, SExpr *expr) {
    return jit_expr_named_variable(runtime, target_id, expr, SJIT_VAR_LIST);
}

int sjit_jit_variable_is_number(SVariable *variable) {
    return variable && variable->scalar_kind == SJIT_SCALAR_NUMBER;
}

double sjit_jit_variable_number(SRuntime *runtime, SVariable *variable) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VARIABLE_NUMBER);
    if (variable) {
        const SValue value = variable->value;
        if (value.tag == SJIT_VALUE_NUMBER) {
            return isnan(value.number) ? 0.0 : value.number;
        }
        if (value.tag == SJIT_VALUE_BOOL) {
            return value.number != 0.0 ? 1.0 : 0.0;
        }
        if (value.tag == SJIT_VALUE_NULL) {
            return 0.0;
        }
        if (value.tag == SJIT_VALUE_STRING) {
            const SString *string = (const SString *)value.ptr;
            if (!string) {
                return 0.0;
            }
            if (string->number_cache_valid) {
                return string->number_cache_ok && !isnan(string->number_cache) ?
                    string->number_cache : 0.0;
            }
        }
    }
    return sjit_variable_number(runtime, variable);
}

void sjit_jit_variable_value(SVariable *variable, SValue *out) {
    if (!out) {
        return;
    }
    *out = variable ?
        sjit_value_clone_fast(variable->value) : sjit_make_number_fast(0.0);
}

int sjit_jit_variable_truthy(SRuntime *runtime, SVariable *variable) {
    return variable ? sjit_to_bool(runtime, variable->value) : 0;
}

void sjit_jit_variable_set_number(SVariable *variable, double number) {
    sjit_variable_set_number_fast(variable, number);
}

void sjit_jit_variable_change_by_number(SRuntime *runtime, SVariable *variable, double delta) {
    sjit_variable_change_by_number(runtime, variable, delta);
}

double sjit_jit_expr_variable_number(SRuntime *runtime, int target_id, SExpr *expr) {
    return sjit_variable_number(runtime, jit_expr_variable(runtime, target_id, expr, SJIT_VAR_SCALAR));
}

double sjit_jit_expr_list_length_number(SRuntime *runtime, int target_id, SExpr *expr) {
    SList *list = jit_expr_list(runtime, target_id, expr);
    return (double)sjit_list_length(list);
}

double sjit_jit_list_variable_length_number(SVariable *list_variable) {
    return (double)sjit_list_length(jit_list_from_variable(list_variable));
}

double sjit_jit_expr_list_item_number_at(SRuntime *runtime, int target_id, SExpr *expr, double index) {
    SList *list = jit_expr_list(runtime, target_id, expr);
    if (!list) {
        return 0.0;
    }
    SValue index_value = sjit_make_number(index);
    const int resolved_index = sjit_list_to_index(runtime, index_value, sjit_list_length(list), 0);
    if (resolved_index == SJIT_LIST_INDEX_INVALID) {
        return 0.0;
    }
    double out = 0.0;
    if (sjit_list_get_number(list, resolved_index, &out)) {
        return out;
    }
    SValue item = sjit_list_get(list, resolved_index);
    out = sjit_to_number_fast(runtime, item);
    sjit_value_destroy(item);
    return out;
}

int sjit_jit_expr_list_contains_literal(SRuntime *runtime, int target_id, SExpr *expr) {
    SList *list = jit_expr_list(runtime, target_id, expr);
    if (!list || !expr || !expr->left || expr->left->opcode != SJIT_EXPR_LITERAL) {
        return 0;
    }
    SValue item = sjit_value_clone(expr->left->literal);
    const int contains = sjit_list_contains(runtime, list, item);
    sjit_value_destroy(item);
    return contains;
}

double sjit_jit_days_since_2000(void) {
    struct timeval now;
    if (gettimeofday(&now, NULL) != 0) {
        return 0.0;
    }
    const double unix_ms = ((double)now.tv_sec * 1000.0) + ((double)now.tv_usec / 1000.0);
    return (unix_ms - 946684800000.0) / 86400000.0;
}

double sjit_jit_round_number(double value) {
    return sjit_op_round_number(value);
}

double sjit_jit_random_number(double from, double to) {
    if (to < from) {
        const double tmp = from;
        from = to;
        to = tmp;
    }
    if (floor(from) == from && floor(to) == to) {
        const int min = (int)from;
        const int max = (int)to;
        return (double)(min + (rand() % (max - min + 1)));
    }
    const double unit = (double)rand() / (double)RAND_MAX;
    return from + ((to - from) * unit);
}

double sjit_jit_mathop_number(SRuntime *runtime, SExpr *expr, double value) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_MATHOP);
    (void)runtime;
    if (!expr) {
        return 0.0;
    }
    if (!expr->mathop_cache_valid) {
        const char *operator_name = expr->literal.tag == SJIT_VALUE_STRING ?
            sjit_string_cstr((const SString *)expr->literal.ptr) :
            "";
        expr->mathop_cache = sjit_op_mathop_id(operator_name);
        expr->mathop_cache_valid = 1;
    }
#ifdef SJIT_PROFILE_RUNTIME
    if (runtime && expr->mathop_cache >= 1 && expr->mathop_cache <= 14) {
        ++runtime->profile_counts[
            SJIT_PROFILE_MATHOP_ABS + expr->mathop_cache - 1];
    }
#endif
    return sjit_op_mathop_number_id(expr->mathop_cache, value);
}

void sjit_jit_value_make_number(double value, SValue *out) {
    if (out) {
        *out = sjit_make_number(value);
    }
}

void sjit_jit_value_make_bool(int value, SValue *out) {
    if (out) {
        *out = sjit_make_bool(value != 0);
    }
}

void sjit_jit_expr_literal_value(SExpr *expr, SValue *out) {
    if (!out) {
        return;
    }
    if (!expr || expr->opcode != SJIT_EXPR_LITERAL) {
        *out = sjit_make_null();
        return;
    }
    *out = sjit_value_clone(expr->literal);
}

double sjit_jit_variable_argument_value(SRuntime *runtime, SVariable *variable, SValue *out) {
    if (!variable) {
        if (out) {
            *out = sjit_make_number_fast(0.0);
        }
        return 0.0;
    }
    if (out) {
        *out = sjit_value_clone_fast(variable->value);
    }
    return sjit_variable_number(runtime, variable);
}

double sjit_jit_procedure_argument_copy(double *numeric_args, SValue *value_args, int index, SValue *out) {
    if (index < 0) {
        if (out) {
            *out = sjit_make_null_fast();
        }
        return 0.0;
    }
    const double number = numeric_args ? numeric_args[index] : 0.0;
    if (out) {
        *out = value_args ? sjit_value_clone_fast(value_args[index]) : sjit_make_number_fast(number);
    }
    return number;
}

static double jit_list_item_argument(
    SRuntime *runtime,
    int target_id,
    SExpr *expr,
    SValue index,
    SValue *out) {
    SList *list = jit_expr_list(runtime, target_id, expr);
    const int resolved_index = list ?
        sjit_list_to_index(runtime, index, sjit_list_length(list), 0) :
        SJIT_LIST_INDEX_INVALID;
    const SValue *item = resolved_index == SJIT_LIST_INDEX_INVALID ?
        NULL : sjit_list_get_borrowed(list, resolved_index);
    if (!item) {
        if (out) {
            *out = sjit_make_string("");
        }
        return 0.0;
    }
    if (out) {
        *out = sjit_value_clone_fast(*item);
    }
    return sjit_to_number_fast(runtime, *item);
}

double sjit_jit_expr_list_item_argument_literal(
    SRuntime *runtime,
    int target_id,
    SExpr *expr,
    SValue *out) {
    const SValue index = expr && expr->left && expr->left->opcode == SJIT_EXPR_LITERAL ?
        expr->left->literal : sjit_make_null_fast();
    return jit_list_item_argument(runtime, target_id, expr, index, out);
}

double sjit_jit_expr_list_item_argument_at_variable(
    SRuntime *runtime,
    int target_id,
    SExpr *expr,
    SVariable *index_variable,
    SValue *out) {
    const SValue index = index_variable ? index_variable->value : sjit_make_null_fast();
    return jit_list_item_argument(runtime, target_id, expr, index, out);
}

void sjit_jit_procedure_argument_value_at(double *numeric_args, SValue *value_args, int index, SValue *out) {
    (void)sjit_jit_procedure_argument_copy(numeric_args, value_args, index, out);
}

void sjit_jit_value_join_ptr(SRuntime *runtime, SValue *left, SValue *right, SValue *out) {
    if (!out) {
        return;
    }
    if (!left || !right) {
        *out = sjit_make_null();
        return;
    }
    *out = sjit_op_join(runtime, *left, *right);
}

void sjit_jit_expr_variable_value(SRuntime *runtime, int target_id, SExpr *expr, SValue *out) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VARIABLE_VALUE);
    if (!out) {
        return;
    }
    SVariable *variable = jit_expr_variable(runtime, target_id, expr, SJIT_VAR_SCALAR);
    *out = variable ? sjit_value_clone(variable->value) : sjit_make_number(0.0);
}

void sjit_jit_expr_list_item_value(SRuntime *runtime, int target_id, SExpr *expr, SValue *index, SValue *out) {
    if (!out) {
        return;
    }
    SList *list = jit_expr_list(runtime, target_id, expr);
    if (!list || !index) {
        *out = sjit_make_string("");
        return;
    }
    const int resolved_index = sjit_list_to_index(runtime, *index, sjit_list_length(list), 0);
    if (resolved_index == SJIT_LIST_INDEX_INVALID) {
        *out = sjit_make_string("");
        return;
    }
    SValue item = sjit_list_get(list, resolved_index);
    if (item.tag == SJIT_VALUE_NULL) {
        sjit_value_destroy(item);
        *out = sjit_make_string("");
        return;
    }
    *out = item;
}

double sjit_jit_expr_list_item_number_value(SRuntime *runtime, int target_id, SExpr *expr, SValue *item) {
    SList *list = jit_expr_list(runtime, target_id, expr);
    return list && item ? (double)sjit_list_item_number(runtime, list, *item) : 0.0;
}

int sjit_jit_expr_list_contains_value(SRuntime *runtime, int target_id, SExpr *expr, SValue *item) {
    SList *list = jit_expr_list(runtime, target_id, expr);
    return list && item ? sjit_list_contains(runtime, list, *item) : 0;
}

double sjit_jit_value_length_number(SRuntime *runtime, SValue *value) {
    if (!value) {
        return 0.0;
    }
    SValue text = sjit_to_string(runtime, *value);
    const SString *string = (const SString *)text.ptr;
    const double length = (double)(string ? string->length : 0);
    sjit_value_destroy(text);
    return length;
}

void sjit_jit_value_letter_of(SRuntime *runtime, SValue *index, SValue *text_value, SValue *out) {
    if (!out) {
        return;
    }
    if (!index || !text_value) {
        *out = sjit_make_string("");
        return;
    }
    const int resolved_index = (int)floor(sjit_to_number_fast(runtime, *index));
    SValue text = sjit_to_string(runtime, *text_value);
    const SString *string = (const SString *)text.ptr;
    if (string && resolved_index >= 1 && resolved_index <= string->length) {
        *out = sjit_make_string_len(string->bytes + resolved_index - 1, 1);
    } else {
        *out = sjit_make_string("");
    }
    sjit_value_destroy(text);
}

int sjit_jit_key_pressed_value(SRuntime *runtime, SValue *key) {
    if (!runtime || !key) {
        return 0;
    }
    SValue text = sjit_to_string(runtime, *key);
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
        const int index = key->tag == SJIT_VALUE_NUMBER ?
            jit_key_index_for_number(key->number) : jit_key_index_for_name(key_name);
        pressed = index >= 0 && index < 256 ? runtime->input.key_down[index] : 0;
    }
    sjit_value_destroy(text);
    return pressed;
}

int sjit_jit_value_truthy(SRuntime *runtime, SValue *value) {
    return value ? sjit_to_bool(runtime, *value) : 0;
}

static inline int jit_cached_comparison_number(
    const SValue *value,
    double *out_number) {
    if (!value || value->tag != SJIT_VALUE_STRING || !value->ptr) {
        return 0;
    }
    const SString *string = (const SString *)value->ptr;
    if (!string->number_cache_valid || !string->number_cache_ok ||
        (string->number_cache == 0.0 && string->number_cache_whitespace) ||
        isnan(string->number_cache)) {
        return 0;
    }
    *out_number = string->number_cache;
    return 1;
}

static inline int jit_compare_numbers(double left, double right, int opcode) {
    switch (opcode) {
    case SJIT_EXPR_LT:
        return left < right;
    case SJIT_EXPR_EQ:
        return left == right;
    case SJIT_EXPR_GT:
        return left > right;
    default:
        return 0;
    }
}

int sjit_jit_value_compare(
    SRuntime *runtime,
    SValue *left,
    SValue *right,
    int opcode) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VALUE_COMPARE);
    if (!left || !right) {
        return 0;
    }
    if (left->tag == SJIT_VALUE_NUMBER && right->tag == SJIT_VALUE_NUMBER) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VALUE_COMPARE_NUMBER_NUMBER);
    } else if (left->tag == SJIT_VALUE_NUMBER && right->tag == SJIT_VALUE_STRING) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VALUE_COMPARE_NUMBER_STRING);
    } else if (left->tag == SJIT_VALUE_STRING && right->tag == SJIT_VALUE_NUMBER) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VALUE_COMPARE_STRING_NUMBER);
    } else if (left->tag == SJIT_VALUE_STRING && right->tag == SJIT_VALUE_STRING) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VALUE_COMPARE_STRING_STRING);
        double left_number = 0.0;
        double right_number = 0.0;
        if (jit_cached_comparison_number(left, &left_number) &&
            jit_cached_comparison_number(right, &right_number)) {
            SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VALUE_COMPARE_STRING_STRING_NUMERIC);
        }
    } else if (left->tag == SJIT_VALUE_NUMBER) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VALUE_COMPARE_NUMBER_OTHER);
    } else if (right->tag == SJIT_VALUE_NUMBER) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VALUE_COMPARE_OTHER_NUMBER);
    } else {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VALUE_COMPARE_OTHER_OTHER);
    }
    if (opcode == SJIT_EXPR_LT) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VALUE_COMPARE_LT);
    } else if (opcode == SJIT_EXPR_EQ) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VALUE_COMPARE_EQ);
    } else if (opcode == SJIT_EXPR_GT) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VALUE_COMPARE_GT);
    }
    /* Numeric text is immutable and caches its Scratch-compatible parse.
       Comparisons in generated numeric projects overwhelmingly hit this
       shape.  Once cached, keep the hot path in this bridge instead of
       copying both SValues through the general comparison stack again. */
    if (left->tag == SJIT_VALUE_NUMBER && !isnan(left->number)) {
        double right_number = 0.0;
        if (right->tag == SJIT_VALUE_NUMBER && !isnan(right->number)) {
            return jit_compare_numbers(left->number, right->number, opcode);
        }
        if (jit_cached_comparison_number(right, &right_number)) {
            return jit_compare_numbers(left->number, right_number, opcode);
        }
    } else if (right->tag == SJIT_VALUE_NUMBER && !isnan(right->number)) {
        double left_number = 0.0;
        if (jit_cached_comparison_number(left, &left_number)) {
            return jit_compare_numbers(left_number, right->number, opcode);
        }
    } else if (left->tag == SJIT_VALUE_STRING &&
               right->tag == SJIT_VALUE_STRING) {
        double left_number = 0.0;
        double right_number = 0.0;
        const int left_is_number =
            jit_cached_comparison_number(left, &left_number);
        const int right_is_number =
            jit_cached_comparison_number(right, &right_number);
        if (left_is_number && right_is_number) {
            return jit_compare_numbers(left_number, right_number, opcode);
        }
        const SString *left_string = (const SString *)left->ptr;
        const SString *right_string = (const SString *)right->ptr;
        if (opcode == SJIT_EXPR_EQ && left_string == right_string) {
            return 1;
        }
        /* Once both immutable strings have populated their numeric caches,
           failure of the numeric pair above means Scratch compares them as
           case-insensitive text. Avoid re-entering the generic conversion
           stack in hot list-marker tests such as item = "n". */
        if (opcode == SJIT_EXPR_EQ && left_string && right_string &&
            left_string->number_cache_valid &&
            right_string->number_cache_valid) {
            if (left_is_number != right_is_number) {
                return 0;
            }
            return sjit_cstr_equals_ignore_case(
                left_string->bytes,
                right_string->bytes);
        }
    }
    switch (opcode) {
    case SJIT_EXPR_LT:
        return sjit_lt(runtime, *left, *right);
    case SJIT_EXPR_EQ:
        return sjit_eq(runtime, *left, *right);
    case SJIT_EXPR_GT:
        return sjit_gt(runtime, *left, *right);
    default:
        return 0;
    }
}

void sjit_jit_pen_set_color_value_and_change_param_number_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SValue *color,
    int param_id,
    double delta) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_PEN_COLOR_VALUE);
    if (!color) {
        return;
    }
    sjit_pen_set_color_value_and_change_param_number(
        runtime,
        jit_script_sprite(runtime, script),
        *color,
        param_id,
        delta);
}

static int jit_list_index_at_value(
    SRuntime *runtime,
    SValue index,
    int length) {
    if (index.tag == SJIT_VALUE_NUMBER || index.tag == SJIT_VALUE_BOOL || index.tag == SJIT_VALUE_NULL) {
        const double converted = sjit_to_number_fast(runtime, index);
        return converted >= 1.0 && converted < (double)length + 1.0 ?
            (int)converted : SJIT_LIST_INDEX_INVALID;
    }
    return sjit_list_to_index(runtime, index, length, 0);
}

static int jit_list_index_at_number(double index, int length) {
    return index >= 1.0 && index < (double)length + 1.0 ?
        (int)index : SJIT_LIST_INDEX_INVALID;
}

static inline __attribute__((always_inline)) int
jit_list_replace_number_fast(SList *list, int one_based_index, double number) {
    struct SListStorage *storage = list ? list->storage : NULL;
    if (storage && storage->ref_count == 1 &&
        one_based_index >= 1 && one_based_index <= storage->length) {
        const int zero = one_based_index - 1;
        SValue *item = &storage->items[zero];
        if (item->tag == SJIT_VALUE_NUMBER &&
            (!storage->numbers_valid || storage->numbers)) {
            item->number = number;
            item->ptr = NULL;
            if (storage->numbers_valid) {
                storage->numbers[zero] = number;
            }
            return 1;
        }
    }
    return sjit_list_replace_number(list, one_based_index, number);
}

static inline __attribute__((always_inline)) void
jit_list_replace_literal_exact(
    SRuntime *runtime,
    SList *list,
    int one_based_index,
    const SExpr *literal) {
    if (!literal || literal->opcode != SJIT_EXPR_LITERAL) {
        return;
    }
    if (literal->literal.tag == SJIT_VALUE_STRING) {
        /* The fused pen loop replaces a private color-list item with the
           same small marker string millions of times.  Keep the normal
           sjit_list_replace fallback for shared/invalid storage, but avoid
           its ownership and bounds-call layers when the usual unique-list
           proof is already true. */
        SListStorage *storage = list ? list->storage : NULL;
        if (storage && storage->ref_count == 1 && storage->items &&
            literal->literal.ptr && one_based_index >= 1 &&
            one_based_index <= storage->length) {
            const int zero = one_based_index - 1;
            SString *replacement = (SString *)literal->literal.ptr;
            ++replacement->ref_count;
            sjit_value_destroy_fast(storage->items[zero]);
            storage->items[zero] = literal->literal;
            if (storage->numbers_valid) {
                free(storage->numbers);
                storage->numbers = NULL;
                storage->numbers_valid = 0;
            }
            return;
        }
        sjit_list_replace(list, one_based_index, literal->literal);
        return;
    }
    if (literal->literal.tag == SJIT_VALUE_LIST) {
        sjit_list_replace(list, one_based_index, literal->literal);
        return;
    }
    jit_list_replace_number_fast(
        list,
        one_based_index,
        sjit_to_number_fast(runtime, literal->literal));
}

static const SValue *jit_list_item_borrowed_from_list_at_value(
    SRuntime *runtime,
    SList *list,
    SValue index) {
    SListStorage *storage = list ? list->storage : NULL;
    if (!storage) {
        return NULL;
    }
    const int resolved_index = jit_list_index_at_value(
        runtime,
        index,
        storage->length);
    if (resolved_index == SJIT_LIST_INDEX_INVALID ||
        !storage->items || resolved_index > storage->length) {
        return NULL;
    }
    const SValue *item = &storage->items[resolved_index - 1];
    return item && item->tag != SJIT_VALUE_NULL ? item : NULL;
}

static const SValue *jit_list_item_borrowed_from_list_at_number(
    SList *list,
    double index) {
    SListStorage *storage = list ? list->storage : NULL;
    if (!storage) {
        return NULL;
    }
    const int converted = jit_list_index_at_number(index, storage->length);
    if (converted == SJIT_LIST_INDEX_INVALID ||
        !storage->items || converted > storage->length) {
        return NULL;
    }
    const SValue *item = &storage->items[converted - 1];
    return item && item->tag != SJIT_VALUE_NULL ? item : NULL;
}

static inline __attribute__((always_inline)) double
jit_list_item_number_at_resolved(
    SRuntime *runtime,
    SList *list,
    int resolved_index) {
    const int zero = resolved_index - 1;
    if (list->storage->numbers_valid && list->storage->numbers) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_LIST_NUMBER_NUMERIC_CACHE);
        const double number = list->storage->numbers[zero];
        return isnan(number) ? 0.0 : number;
    }
    if (!list->storage->items) {
        return 0.0;
    }
    const SValue item = list->storage->items[zero];
    if (item.tag == SJIT_VALUE_NUMBER) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_LIST_NUMBER_ITEM_NUMBER);
        return isnan(item.number) ? 0.0 : item.number;
    }
    if (item.tag == SJIT_VALUE_BOOL) {
        return item.number != 0.0 ? 1.0 : 0.0;
    }
    if (item.tag == SJIT_VALUE_NULL) {
        return 0.0;
    }
    if (item.tag == SJIT_VALUE_STRING) {
        const SString *string = (const SString *)item.ptr;
        if (!string) {
            return 0.0;
        }
        if (string->number_cache_valid) {
            SJIT_PROFILE_INC(runtime, SJIT_PROFILE_LIST_NUMBER_ITEM_STRING_CACHE);
            return string->number_cache_ok && !isnan(string->number_cache) ?
                string->number_cache : 0.0;
        }
    }
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_LIST_NUMBER_ITEM_FALLBACK);
    return sjit_to_number_fast(runtime, item);
}

static double jit_list_item_number_from_list_at_value(
    SRuntime *runtime,
    SList *list,
    SValue index) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_LIST_NUMBER_AT_VALUE);
    if (!list || !list->storage) {
        return 0.0;
    }
    const int resolved_index = jit_list_index_at_value(
        runtime,
        index,
        list->storage->length);
    if (resolved_index == SJIT_LIST_INDEX_INVALID) {
        return 0.0;
    }
    return jit_list_item_number_at_resolved(runtime, list, resolved_index);
}

static double jit_list_item_number_from_list_at_number(
    SRuntime *runtime,
    SList *list,
    double index) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_LIST_NUMBER_AT_NUMBER);
    if (!list || !list->storage) {
        return 0.0;
    }
    const int resolved_index = jit_list_index_at_number(
        index,
        list->storage->length);
    if (resolved_index == SJIT_LIST_INDEX_INVALID) {
        return 0.0;
    }
    return jit_list_item_number_at_resolved(runtime, list, resolved_index);
}

static const SValue *jit_list_item_borrowed_at_number(
    SRuntime *runtime,
    int target_id,
    SExpr *expr,
    double index) {
    SList *list = jit_expr_list(runtime, target_id, expr);
    (void)runtime;
    return jit_list_item_borrowed_from_list_at_number(list, index);
}

static const SValue *jit_list_item_borrowed_at_value(
    SRuntime *runtime,
    int target_id,
    SExpr *expr,
    SValue index) {
    SList *list = jit_expr_list(runtime, target_id, expr);
    return jit_list_item_borrowed_from_list_at_value(runtime, list, index);
}

static void jit_list_variable_item_value_from_resolved(
    SRuntime *runtime,
    SList *list,
    int resolved_index,
    SValue *out) {
    (void)runtime;
    SListStorage *storage = list ? list->storage : NULL;
    if (resolved_index == SJIT_LIST_INDEX_INVALID || !storage ||
        !storage->items || resolved_index > storage->length) {
        *out = sjit_make_string("");
        return;
    }
    const int zero = resolved_index - 1;
    if (storage->numbers_valid && storage->numbers) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_LIST_VALUE_CACHED_NUMBER);
        const double number = storage->numbers[zero];
        *out = sjit_make_number_fast(number);
        return;
    }
    const SValue item = storage->items[zero];
    *out = item.tag == SJIT_VALUE_NULL ? sjit_make_string("") :
        sjit_value_clone_fast(item);
}

double sjit_jit_list_variable_item_number_at(
    SRuntime *runtime,
    SVariable *list_variable,
    double index) {
    return jit_list_item_number_from_list_at_number(
        runtime,
        jit_list_from_variable(list_variable),
        index);
}

double sjit_jit_list_variable_item_number_at_variable(
    SRuntime *runtime,
    SVariable *list_variable,
    SVariable *index_variable) {
    if (!index_variable) {
        return 0.0;
    }
    return jit_list_item_number_from_list_at_value(
        runtime,
        jit_list_from_variable(list_variable),
        index_variable->value);
}

double sjit_jit_list_variable_item_number_at_argument(
    SRuntime *runtime,
    SVariable *list_variable,
    double *numeric_arguments,
    SValue *arguments,
    int argument_index) {
    if (argument_index < 0) {
        return 0.0;
    }
    const SValue index = arguments ?
        arguments[argument_index] :
        sjit_make_number_fast(numeric_arguments ? numeric_arguments[argument_index] : 0.0);
    return jit_list_item_number_from_list_at_value(
        runtime,
        jit_list_from_variable(list_variable),
        index);
}

void sjit_jit_list_variable_item_value(
    SRuntime *runtime,
    SVariable *list_variable,
    SValue *index,
    SValue *out) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_LIST_ITEM_VALUE);
    if (!out) {
        return;
    }
    SList *list = jit_list_from_variable(list_variable);
    if (index && index->tag == SJIT_VALUE_NUMBER) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_LIST_VALUE_INDEX_NUMBER);
    }
    if (index &&
        (index->tag == SJIT_VALUE_NUMBER ||
         index->tag == SJIT_VALUE_BOOL ||
         index->tag == SJIT_VALUE_NULL)) {
        const double numeric_index = sjit_to_number_fast(runtime, *index);
        const int resolved_index = list ? jit_list_index_at_number(
            numeric_index,
            sjit_list_length(list)) : SJIT_LIST_INDEX_INVALID;
        jit_list_variable_item_value_from_resolved(runtime, list, resolved_index, out);
        return;
    }
    const int resolved_index = index && list ? jit_list_index_at_value(
        runtime,
        *index,
        sjit_list_length(list)) : SJIT_LIST_INDEX_INVALID;
    jit_list_variable_item_value_from_resolved(runtime, list, resolved_index, out);
}

void sjit_jit_list_variable_item_value_at_number(
    SRuntime *runtime,
    SVariable *list_variable,
    double index,
    SValue *out) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_LIST_ITEM_VALUE);
    if (!out) {
        return;
    }
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_LIST_VALUE_INDEX_NUMBER);
    SList *list = jit_list_from_variable(list_variable);
    SListStorage *storage = list ? list->storage : NULL;
    const int resolved_index = storage ? jit_list_index_at_number(
        index,
        storage->length) : SJIT_LIST_INDEX_INVALID;
    jit_list_variable_item_value_from_resolved(runtime, list, resolved_index, out);
}

static void jit_variable_set_from_list_item_resolved(
    SRuntime *runtime,
    SVariable *destination,
    SList *list,
    int resolved_index) {
    SListStorage *storage = list ? list->storage : NULL;
    if (resolved_index != SJIT_LIST_INDEX_INVALID && storage &&
        storage->numbers_valid && storage->numbers &&
        resolved_index <= storage->length) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VARIABLE_SET_FROM_VALUE);
        sjit_variable_set_number_fast(
            destination,
            storage->numbers[resolved_index - 1]);
        return;
    }
    const SValue *item = resolved_index == SJIT_LIST_INDEX_INVALID || !storage ||
        !storage->items || resolved_index > storage->length ?
        NULL : &storage->items[resolved_index - 1];
    if (item && item->tag == SJIT_VALUE_NULL) {
        item = NULL;
    }
    SValue empty = sjit_make_null_fast();
    if (!item) {
        empty = sjit_make_string("");
        item = &empty;
    }
    jit_variable_set_borrowed(runtime, destination, *item);
    sjit_value_destroy_fast(empty);
}

double sjit_jit_list_variable_item_number_value(
    SRuntime *runtime,
    SVariable *list_variable,
    SValue *item) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_LIST_ITEM_NUMBER_SEARCH);
    SList *list = jit_list_from_variable(list_variable);
    return list && item ? (double)sjit_list_item_number(runtime, list, *item) : 0.0;
}

int sjit_jit_list_variable_contains_value(
    SRuntime *runtime,
    SVariable *list_variable,
    SValue *item) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_LIST_CONTAINS);
    SList *list = jit_list_from_variable(list_variable);
    return list && item ? sjit_list_contains(runtime, list, *item) : 0;
}

static void jit_variable_set_from_list_item(
    SRuntime *runtime,
    int target_id,
    SVariable *destination,
    SExpr *list_expr,
    SValue index) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_SET_FROM_LIST_ITEM);
    SList *list = jit_expr_list(runtime, target_id, list_expr);
    const int resolved_index = list ? jit_list_index_at_value(
        runtime,
        index,
        sjit_list_length(list)) : SJIT_LIST_INDEX_INVALID;
    jit_variable_set_from_list_item_resolved(
        runtime,
        destination,
        list,
        resolved_index);
}

void sjit_jit_variable_set_from_list_item_at_number(
    SRuntime *runtime,
    int target_id,
    SVariable *destination,
    SExpr *list_expr,
    double index) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_SET_FROM_LIST_ITEM);
    SList *list = jit_expr_list(runtime, target_id, list_expr);
    const int resolved_index = list ? jit_list_index_at_number(
        index,
        sjit_list_length(list)) : SJIT_LIST_INDEX_INVALID;
    jit_variable_set_from_list_item_resolved(
        runtime,
        destination,
        list,
        resolved_index);
}

void sjit_jit_variable_set_from_list_item_literal(
    SRuntime *runtime,
    int target_id,
    SVariable *destination,
    SExpr *list_expr) {
    const SValue index = list_expr && list_expr->left &&
        list_expr->left->opcode == SJIT_EXPR_LITERAL ?
        list_expr->left->literal : sjit_make_null_fast();
    if (index.tag == SJIT_VALUE_NUMBER ||
        index.tag == SJIT_VALUE_BOOL ||
        index.tag == SJIT_VALUE_NULL) {
        sjit_jit_variable_set_from_list_item_at_number(
            runtime,
            target_id,
            destination,
            list_expr,
            sjit_to_number_fast(runtime, index));
        return;
    }
    jit_variable_set_from_list_item(runtime, target_id, destination, list_expr, index);
}

void sjit_jit_variable_set_from_list_item_at_variable(
    SRuntime *runtime,
    int target_id,
    SVariable *destination,
    SExpr *list_expr,
    SVariable *index_variable) {
    if (index_variable &&
        (index_variable->value.tag == SJIT_VALUE_NUMBER ||
         index_variable->value.tag == SJIT_VALUE_BOOL ||
         index_variable->value.tag == SJIT_VALUE_NULL)) {
        sjit_jit_variable_set_from_list_item_at_number(
            runtime,
            target_id,
            destination,
            list_expr,
            sjit_to_number_fast(runtime, index_variable->value));
        return;
    }
    jit_variable_set_from_list_item(
        runtime,
        target_id,
        destination,
        list_expr,
        index_variable ? index_variable->value : sjit_make_null_fast());
}

void sjit_jit_variable_set_from_list_item_at_argument(
    SRuntime *runtime,
    int target_id,
    SVariable *destination,
    SExpr *list_expr,
    double *numeric_arguments,
    SValue *arguments,
    int argument_index) {
    if (argument_index >= 0 &&
        (!arguments ||
         arguments[argument_index].tag == SJIT_VALUE_NUMBER ||
         arguments[argument_index].tag == SJIT_VALUE_BOOL ||
         arguments[argument_index].tag == SJIT_VALUE_NULL)) {
        const double numeric_index = arguments ?
            sjit_to_number_fast(runtime, arguments[argument_index]) :
            (numeric_arguments ? numeric_arguments[argument_index] : 0.0);
        sjit_jit_variable_set_from_list_item_at_number(
            runtime,
            target_id,
            destination,
            list_expr,
            numeric_index);
        return;
    }
    const SValue index = argument_index >= 0 && arguments ?
        arguments[argument_index] :
        sjit_make_number_fast(
            argument_index >= 0 && numeric_arguments ? numeric_arguments[argument_index] : 0.0);
    jit_variable_set_from_list_item(runtime, target_id, destination, list_expr, index);
}

void sjit_jit_statement_list_add_list_item_at_variable(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SExpr *list_expr,
    SVariable *index_variable) {
    SList *destination = jit_script_list(runtime, script, statement);
    if (!destination) {
        return;
    }
    const SValue *item = jit_list_item_borrowed_at_value(
        runtime,
        script ? script->target_id : 0,
        list_expr,
        index_variable ? index_variable->value : sjit_make_null_fast());
    if (item) {
        jit_list_push_borrowed(destination, *item);
        return;
    }
    SValue empty = sjit_make_string("");
    sjit_list_push_move(destination, empty);
}

double sjit_jit_expr_list_item_number_at_variable(
    SRuntime *runtime,
    int target_id,
    SExpr *list_expr,
    SVariable *index_variable) {
    if (!index_variable) {
        return 0.0;
    }
    return jit_list_item_number_from_list_at_value(
        runtime,
        jit_expr_list(runtime, target_id, list_expr),
        index_variable->value);
}

double sjit_jit_expr_list_item_number_at_argument(
    SRuntime *runtime,
    int target_id,
    SExpr *list_expr,
    double *numeric_arguments,
    SValue *arguments,
    int argument_index) {
    if (argument_index < 0) {
        return 0.0;
    }
    const SValue index = arguments ?
        arguments[argument_index] :
        sjit_make_number_fast(numeric_arguments ? numeric_arguments[argument_index] : 0.0);
    return jit_list_item_number_from_list_at_value(
        runtime,
        jit_expr_list(runtime, target_id, list_expr),
        index);
}

int sjit_jit_expr_list_item_compare_literal_at_variable(
    SRuntime *runtime,
    int target_id,
    SExpr *list_expr,
    SVariable *index_variable,
    SExpr *literal_expr,
    int opcode) {
    if (!index_variable || !literal_expr || literal_expr->opcode != SJIT_EXPR_LITERAL) {
        return 0;
    }
    const SValue *item = jit_list_item_borrowed_at_value(
        runtime,
        target_id,
        list_expr,
        index_variable->value);
    SValue empty = sjit_make_null();
    if (!item) {
        empty = sjit_make_string("");
        item = &empty;
    }
    int result = 0;
    if (opcode == SJIT_EXPR_LT) {
        result = sjit_lt(runtime, *item, literal_expr->literal);
    } else if (opcode == SJIT_EXPR_EQ) {
        result = sjit_eq(runtime, *item, literal_expr->literal);
    } else if (opcode == SJIT_EXPR_GT) {
        result = sjit_gt(runtime, *item, literal_expr->literal);
    }
    sjit_value_destroy(empty);
    return result;
}

void sjit_jit_pen_set_color_list_item_and_change_param_number_at_variable(
    SRuntime *runtime,
    SCompiledScript *script,
    SExpr *list_expr,
    SVariable *index_variable,
    int param_id,
    double delta) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_PEN_COLOR_LIST);
    const SValue *item = index_variable ? jit_list_item_borrowed_at_value(
        runtime,
        script ? script->target_id : 0,
        list_expr,
        index_variable->value) : NULL;
    SValue empty = sjit_make_null();
    if (!item) {
        empty = sjit_make_string("");
        item = &empty;
    }
    sjit_pen_set_color_value_and_change_param_number(
        runtime,
        jit_script_sprite(runtime, script),
        *item,
        param_id,
        delta);
    sjit_value_destroy(empty);
}

void sjit_jit_statement_list_replace_literal_at_variable(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SVariable *index_variable) {
    const int has_index = index_variable != NULL;
    const SValue index_value = index_variable ? index_variable->value : sjit_make_null();
    SList *list = jit_script_list(runtime, script, statement);
    if (!list || !statement || !statement->value ||
        statement->value->opcode != SJIT_EXPR_LITERAL || !has_index) {
        return;
    }
    const int resolved_index = jit_list_index_at_value(
        runtime,
        index_value,
        sjit_list_length(list));
    if (resolved_index != SJIT_LIST_INDEX_INVALID) {
        jit_list_replace_literal_exact(
            runtime,
            list,
            resolved_index,
            statement->value);
    }
}

void sjit_jit_statement_list_replace_number_at_variable(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SVariable *index_variable,
    double value) {
    const int has_index = index_variable != NULL;
    const SValue index_value = index_variable ? index_variable->value : sjit_make_null();
    SList *list = jit_script_list(runtime, script, statement);
    if (!list || !has_index) {
        return;
    }
    const int resolved_index = jit_list_index_at_value(
        runtime,
        index_value,
        sjit_list_length(list));
    if (resolved_index != SJIT_LIST_INDEX_INVALID) {
        jit_list_replace_number_fast(list, resolved_index, value);
    }
}

void sjit_jit_statement_list_replace_list_item_at_variables(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SVariable *index_variable,
    SExpr *source_expr,
    SVariable *source_index_variable) {
    const int has_index = index_variable != NULL;
    const int has_source_index = source_index_variable != NULL;
    const SValue index_value = index_variable ? index_variable->value : sjit_make_null();
    const SValue source_index_value = source_index_variable ?
        source_index_variable->value : sjit_make_null();
    SList *destination = jit_script_list(runtime, script, statement);
    if (!destination || !has_index) {
        return;
    }
    const int destination_index = jit_list_index_at_value(
        runtime,
        index_value,
        sjit_list_length(destination));
    if (destination_index == SJIT_LIST_INDEX_INVALID) {
        return;
    }

    const SValue *source_item = has_source_index ? jit_list_item_borrowed_at_value(
        runtime,
        script ? script->target_id : 0,
        source_expr,
        source_index_value) : NULL;
    SValue empty = sjit_make_null();
    if (!source_item) {
        empty = sjit_make_string("");
        source_item = &empty;
    }
    sjit_list_replace(destination, destination_index, *source_item);
    sjit_value_destroy(empty);
}

void sjit_jit_statement_list_replace_list_item_at_variable_number(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SVariable *index_variable,
    SExpr *source_expr,
    double source_index) {
    const int has_index = index_variable != NULL;
    const SValue index_value = index_variable ? index_variable->value : sjit_make_null();
    SList *destination = jit_script_list(runtime, script, statement);
    if (!destination || !has_index) {
        return;
    }
    const int destination_index = jit_list_index_at_value(
        runtime,
        index_value,
        sjit_list_length(destination));
    if (destination_index == SJIT_LIST_INDEX_INVALID) {
        return;
    }

    const SValue *source_item = jit_list_item_borrowed_at_number(
        runtime,
        script ? script->target_id : 0,
        source_expr,
        source_index);
    SValue empty = sjit_make_null();
    if (!source_item) {
        empty = sjit_make_string("");
        source_item = &empty;
    }
    sjit_list_replace(destination, destination_index, *source_item);
    sjit_value_destroy(empty);
}

void sjit_jit_pen_render_list_pixel_at_variable(
    SRuntime *runtime,
    SCompiledScript *script,
    SSprite *sprite,
    SExpr *color_expr,
    SExpr *brightness_expr,
    SVariable *index_variable,
    SStatement *replace_statement,
    int param_id) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_PEN_RENDER_PIXEL);
    const int has_index = index_variable != NULL;
    const SValue index_value = index_variable ? index_variable->value : sjit_make_null();
    const int target_id = script ? script->target_id : 0;
    const SValue *color = has_index ? jit_list_item_borrowed_at_value(
        runtime,
        target_id,
        color_expr,
        index_value) : NULL;
    SValue empty = sjit_make_null();
    if (!color) {
        empty = sjit_make_string("");
        color = &empty;
    }
    const double delta = has_index ? jit_list_item_number_from_list_at_value(
        runtime,
        jit_expr_list(runtime, target_id, brightness_expr),
        index_value) : 0.0;
    sjit_pen_set_color_value_and_change_param_number(
        runtime,
        sprite,
        *color,
        param_id,
        delta);

    SList *replace_list = jit_script_list(runtime, script, replace_statement);
    if (replace_list && replace_statement && replace_statement->value &&
        replace_statement->value->opcode == SJIT_EXPR_LITERAL && has_index) {
        const int resolved_index = jit_list_index_at_value(
            runtime,
            index_value,
            sjit_list_length(replace_list));
        if (resolved_index != SJIT_LIST_INDEX_INVALID) {
            jit_list_replace_literal_exact(
                runtime,
                replace_list,
                resolved_index,
                replace_statement->value);
        }
    }
    sjit_pen_stamp(runtime, sprite);
    sjit_value_destroy(empty);
}

void sjit_jit_pen_render_list_pixel_from_variables(
    SRuntime *runtime,
    SSprite *sprite,
    SVariable *color_list_variable,
    SVariable *brightness_list_variable,
    SVariable *index_variable,
    SExpr *replacement_literal,
    int param_id) {
    /* The native procedure prologue has already resolved these invocation-
       local handles after its runtime/script/target identity guards.  Keep
       Scratch's original read, color-change, replace, then stamp order here;
       only the repeated name/cache lookups are removed. */
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_PEN_RENDER_PIXEL);
    const int has_index = index_variable != NULL;
    const SValue index_value = index_variable ?
        index_variable->value : sjit_make_null();
    SList *color_list = jit_list_from_variable(color_list_variable);
    SList *brightness_list = jit_list_from_variable(brightness_list_variable);
    const SValue *color = has_index ?
        jit_list_item_borrowed_from_list_at_value(
            runtime,
            color_list,
            index_value) : NULL;
    SValue empty = sjit_make_null();
    if (!color) {
        empty = sjit_make_string("");
        color = &empty;
    }
    const double delta = has_index ?
        jit_list_item_number_from_list_at_value(
            runtime,
            brightness_list,
            index_value) : 0.0;
    sjit_pen_set_color_value_and_change_param_number(
        runtime,
        sprite,
        *color,
        param_id,
        delta);

    if (color_list && replacement_literal &&
        replacement_literal->opcode == SJIT_EXPR_LITERAL && has_index) {
        const int resolved_index = jit_list_index_at_value(
            runtime,
            index_value,
            sjit_list_length(color_list));
        if (resolved_index != SJIT_LIST_INDEX_INVALID) {
            jit_list_replace_literal_exact(
                runtime,
                color_list,
                resolved_index,
                replacement_literal);
        }
    }
    sjit_pen_stamp(runtime, sprite);
    sjit_value_destroy(empty);
}

static double jit_set_col_clamp(double value) {
    if (value > 255.0) {
        return 255.0;
    }
    if (value < 0.0) {
        return 0.0;
    }
    return value;
}

void sjit_jit_set_col_from_numbers(
    SRuntime *runtime,
    SSprite *sprite,
    SVariable *color_list_variable,
    SVariable *clamp_variable,
    double red,
    double green,
    double blue) {
    SList *color_list = jit_list_from_variable(color_list_variable);
    if (color_list) {
        sjit_list_clear(color_list);
    }

    const double channels[3] = {red, green, blue};
    double clamped_channels[3];
    for (int i = 0; i < 3; ++i) {
        const double value = jit_set_col_clamp(channels[i]);
        clamped_channels[i] = value;
        sjit_variable_set_number_fast(clamp_variable, value);
        if (color_list && i < 2) {
            sjit_list_push_number(color_list, value);
        }
    }

    const double packed_color =
        clamped_channels[0] * 65536.0 +
        clamped_channels[1] * 256.0 +
        clamped_channels[2];
    sjit_pen_set_color_value(runtime, sprite, sjit_make_number_fast(packed_color));
}

static int jit_pen_path_reserve_additional(
    SPenPathBuffer *path,
    int additional) {
    if (!path || additional < 0 || path->length < 0 || path->capacity < 0 ||
        path->length > INT_MAX - additional) {
        return 0;
    }
    const int wanted = path->length + additional;
    if (wanted <= path->capacity) {
        return path->items != NULL || wanted == 0;
    }
    int next = path->capacity > 0 ? path->capacity : SJIT_INITIAL_CAPACITY;
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
        path->items,
        sizeof(SDrawCommand) * (size_t)next);
    if (!items) {
        return 0;
    }
    path->items = items;
    path->capacity = next;
    return 1;
}

static uint32_t jit_pen_color_to_js_uint32(double number) {
    if (!isfinite(number) || number == 0.0) {
        return 0u;
    }
    double bits = fmod(trunc(number), 4294967296.0);
    if (bits < 0.0) {
        bits += 4294967296.0;
    }
    return (uint32_t)bits;
}

static int jit_pen_tile_row_has_only_opaque_numeric_colors(
    SRuntime *runtime,
    const struct SListStorage *color_storage,
    int first_one_based_index,
    int columns) {
    if (!color_storage || !color_storage->items || columns < 0) {
        return 0;
    }
    for (int offset = 0; offset < columns; ++offset) {
        const SValue color =
            color_storage->items[first_one_based_index - 1 + offset];
        const SString *string = color.tag == SJIT_VALUE_STRING ?
            (const SString *)color.ptr : NULL;
        if (string && string->bytes &&
            (string->bytes[0] == 'n' || string->bytes[0] == 'N') &&
            string->bytes[1] == '\0') {
            continue;
        }
        double number = 0.0;
        if (color.tag == SJIT_VALUE_STRING) {
            if (!string || !string->bytes || string->bytes[0] == '#') {
                return 0;
            }
            number = string->number_cache_valid ?
                (string->number_cache_ok && !isnan(string->number_cache) ?
                    string->number_cache : 0.0) :
                sjit_to_number_fast(runtime, color);
        } else if (color.tag == SJIT_VALUE_NUMBER ||
                   color.tag == SJIT_VALUE_BOOL ||
                   color.tag == SJIT_VALUE_NULL) {
            number = sjit_to_number_fast(runtime, color);
        } else {
            return 0;
        }
        const uint32_t bits = jit_pen_color_to_js_uint32(number);
        const uint32_t alpha = (bits >> 24) & 0xffu;
        if (alpha != 0u && alpha != 0xffu) {
            return 0;
        }
    }
    return 1;
}

static int jit_pen_width_is_single_dense_pixel(double width) {
    if (!isfinite(width) || width <= 0.0) {
        return 0;
    }
    const double normalized = fmax(1.0, width);
    if (normalized > (double)LONG_MAX) {
        return 0;
    }
    return lround(normalized) / 2 == 0;
}

static int jit_pen_prepare_raster_tile_row(
    SRuntime *runtime,
    SSprite *sprite,
    const struct SListStorage *color_storage,
    int first_one_based_index,
    int columns,
    double row,
    double column_start,
    double x_step,
    int param_id) {
    if (!runtime || !sprite) {
        return 0;
    }
    const int opaque_colors =
        jit_pen_tile_row_has_only_opaque_numeric_colors(
            runtime,
            color_storage,
            first_one_based_index,
            columns);
    if (getenv("SJIT_DISABLE_COMPACT_PEN_TILE") != NULL ||
        columns != SJIT_PEN_RASTER_TILE_WIDTH || row < 0.0 ||
        row >= (double)SJIT_PEN_RASTER_TILE_HEIGHT ||
        column_start != 0.0 || x_step != 1.0 || param_id != 3 ||
        sprite->x != -239.5 || sprite->y != -179.5 + row ||
        !jit_pen_width_is_single_dense_pixel(sprite->pen_size) ||
        runtime->pen.revision <= 0 ||
        !opaque_colors) {
        if (getenv("SJIT_LOG_COMPACT_PEN_TILE_REJECT") != NULL) {
            static int logged = 0;
            if (!logged) {
                fprintf(
                    stderr,
                    "sjit: compact pen tile rejected columns=%d row=%.17g "
                    "column=%.17g step=%.17g param=%d x=%.17g y=%.17g "
                    "size=%.17g revision=%d opaque=%d\n",
                    columns,
                    row,
                    column_start,
                    x_step,
                    param_id,
                    sprite->x,
                    sprite->y,
                    sprite->pen_size,
                    runtime->pen.revision,
                    opaque_colors);
                logged = 1;
            }
        }
        return 0;
    }

    const int row_index = (int)row;
    SPenRasterTile *tile = &runtime->pen_raster_tile;
    if (row_index > 0) {
        if (!tile->active || runtime->pen.length != 0 ||
            tile->width != SJIT_PEN_RASTER_TILE_WIDTH ||
            tile->height != SJIT_PEN_RASTER_TILE_HEIGHT ||
            tile->stride != SJIT_PEN_RASTER_TILE_WIDTH * 4 ||
            tile->rows_filled != row_index ||
            tile->target_id != sprite->base.id ||
            tile->revision != runtime->pen.revision ||
            tile->origin_x != -239.5 || tile->origin_y != -179.5 ||
            tile->step != 1.0 || tile->pen_width != sprite->pen_size) {
            return 0;
        }
        runtime->pen_materialized_valid = 0;
        return 1;
    }
    if (runtime->pen.length != 0 || tile->active) {
        return 0;
    }

    unsigned char *pixels = tile->pixels;
    unsigned char *active_bits = tile->active_bits;
    const int allocated_pixels = pixels == NULL;
    const int allocated_bits = active_bits == NULL;
    if (!pixels) {
        pixels = (unsigned char *)malloc(SJIT_PEN_RASTER_TILE_PIXEL_BYTES);
    }
    if (!active_bits) {
        active_bits = (unsigned char *)malloc(SJIT_PEN_RASTER_TILE_MASK_BYTES);
    }
    if (!pixels || !active_bits) {
        if (allocated_pixels) {
            free(pixels);
        }
        if (allocated_bits) {
            free(active_bits);
        }
        return 0;
    }
    tile->pixels = pixels;
    tile->active_bits = active_bits;
    memset(tile->pixels, 0, SJIT_PEN_RASTER_TILE_PIXEL_BYTES);
    memset(tile->active_bits, 0, SJIT_PEN_RASTER_TILE_MASK_BYTES);
    tile->width = SJIT_PEN_RASTER_TILE_WIDTH;
    tile->height = SJIT_PEN_RASTER_TILE_HEIGHT;
    tile->stride = SJIT_PEN_RASTER_TILE_WIDTH * 4;
    tile->rows_filled = 0;
    tile->command_count = 0;
    tile->target_id = sprite->base.id;
    tile->revision = runtime->pen.revision;
    tile->active = 1;
    tile->origin_x = -239.5;
    tile->origin_y = -179.5;
    tile->step = 1.0;
    tile->pen_width = sprite->pen_size;
    runtime->pen_materialized_valid = 0;
    if (getenv("SJIT_LOG_COMPACT_PEN_TILE") != NULL) {
        static int logged = 0;
        if (!logged) {
            fprintf(stderr, "sjit: compact 480x360 pen raster tile active\n");
            logged = 1;
        }
    }
    return 1;
}

static inline __attribute__((always_inline)) void
jit_pen_store_raster_tile_pixel(
    SRuntime *runtime,
    int row,
    int column,
    const SSprite *sprite) {
    SPenRasterTile *tile = &runtime->pen_raster_tile;
    const int logical_index = row * tile->width + column;
    tile->active_bits[logical_index >> 3] |=
        (unsigned char)(1u << (logical_index & 7));
    const int pixel_y = tile->height - 1 - row;
    unsigned char *pixel = tile->pixels +
        (size_t)pixel_y * (size_t)tile->stride +
        (size_t)column * 4u;
    pixel[0] = (unsigned char)sprite->pen_r;
    pixel[1] = (unsigned char)sprite->pen_g;
    pixel[2] = (unsigned char)sprite->pen_b;
    pixel[3] = (unsigned char)sprite->pen_a;
    tile->command_count += 1;
}

static inline __attribute__((always_inline)) void
jit_pen_append_reserved_stamp(
    SRuntime *runtime,
    SSprite *sprite,
    double x,
    double y) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_PEN_STAMP);
    SDrawCommand command = {0};
    command.kind = SJIT_DRAW_PEN_STROKE;
    command.target_id = sprite->base.id;
    command.x = x;
    command.y = y;
    command.x2 = x;
    command.y2 = y;
    command.pen_width = sprite->pen_size;
    command.r = sprite->pen_r;
    command.g = sprite->pen_g;
    command.b = sprite->pen_b;
    command.a = sprite->pen_a;
    command.visible = 1;
    runtime->pen.items[runtime->pen.length++] = command;
    sprite->pen_down = 0;
}

static int jit_native_pen_row_reject(const char *reason) {
    if (getenv("SJIT_LOG_NATIVE_PEN_ROW") != NULL) {
        static int logged = 0;
        if (!logged) {
            fprintf(stderr, "sjit: native pen-row kernel rejected: %s\n", reason);
            logged = 1;
        }
    }
    return 0;
}

int sjit_jit_pen_render_row_from_variables(
    SRuntime *runtime,
    SSprite *sprite,
    SVariable *color_list_variable,
    SVariable *brightness_list_variable,
    SVariable *row_variable,
    SVariable *column_variable,
    SVariable *index_variable,
    double column_count,
    double x_step,
    SExpr *replacement_literal,
    int param_id) {
    if (!runtime || !sprite || sprite->pen_down ||
        !row_variable || !column_variable || !index_variable ||
        row_variable->type != SJIT_VAR_SCALAR ||
        column_variable->type != SJIT_VAR_SCALAR ||
        index_variable->type != SJIT_VAR_SCALAR ||
        row_variable == column_variable || row_variable == index_variable ||
        column_variable == index_variable ||
        row_variable->value.tag == SJIT_VALUE_LIST ||
        column_variable->value.tag == SJIT_VALUE_LIST ||
        index_variable->value.tag == SJIT_VALUE_LIST ||
        column_variable->scalar_kind == SJIT_SCALAR_BOOL ||
        index_variable->scalar_kind == SJIT_SCALAR_BOOL ||
        !isfinite(column_count) || column_count < 0.0 ||
        column_count > (double)INT_MAX ||
        floor(column_count) != column_count ||
        !isfinite(x_step) || !isfinite(sprite->x) ||
        !replacement_literal ||
        replacement_literal->opcode != SJIT_EXPR_LITERAL ||
        replacement_literal->literal.tag != SJIT_VALUE_STRING ||
        !replacement_literal->literal.ptr ||
        param_id == 0) {
        return jit_native_pen_row_reject("entry/type guard");
    }

    const double row = sjit_to_number_fast(runtime, row_variable->value);
    const double column_start = sjit_to_number_fast(
        runtime,
        column_variable->value);
    if (!isfinite(row) || row < 0.0 || row > (double)INT_MAX ||
        floor(row) != row || !isfinite(column_start) ||
        column_start < 0.0 || column_start > (double)INT_MAX ||
        floor(column_start) != column_start) {
        return jit_native_pen_row_reject("row/column state guard");
    }
    const int columns = (int)column_count;
    const SString *marker = (const SString *)replacement_literal->literal.ptr;
    if (!marker->bytes || marker->length != 1 ||
        (marker->bytes[0] != 'n' && marker->bytes[0] != 'N')) {
        return jit_native_pen_row_reject("marker specialization guard");
    }
    const int64_t first_index =
        (int64_t)row * (int64_t)columns + (int64_t)column_start + 1;
    const int64_t last_index = first_index + (int64_t)columns - 1;
    if (first_index > INT_MAX ||
        (columns > 0 && (first_index < 1 || last_index > INT_MAX))) {
        return jit_native_pen_row_reject("index range guard");
    }

    SList *color_list = jit_list_from_variable(color_list_variable);
    SList *brightness_list = jit_list_from_variable(
        brightness_list_variable);
    struct SListStorage *color_storage = color_list ? color_list->storage : NULL;
    struct SListStorage *brightness_storage = brightness_list ?
        brightness_list->storage : NULL;
    if (!color_storage || !brightness_storage || color_storage->ref_count != 1 ||
        (columns > 0 &&
         (!color_storage->items || !brightness_storage->items ||
          last_index > color_storage->length ||
          last_index > brightness_storage->length))) {
        return jit_native_pen_row_reject("list storage guard");
    }

    const int use_raster_tile = jit_pen_prepare_raster_tile_row(
        runtime,
        sprite,
        color_storage,
        (int)first_index,
        columns,
        row,
        column_start,
        x_step,
        param_id);

    /* Reserve either the bounded tile sidecar or the maximum ordinary command
       count before the first Scratch-visible mutation. Allocation failure
       therefore leaves the ordinary JIT loop free to replay the region. */
    if (!use_raster_tile && !jit_pen_path_reserve_additional(
            &runtime->pen,
            columns)) {
        return jit_native_pen_row_reject("pen reserve");
    }

    double column = column_start;
    double x = sprite->x;
    double y = sprite->y;
    double final_index = index_variable->value.number;
    for (int column_offset = 0; column_offset < columns; ++column_offset) {
        const double index_number =
            (row * (double)columns + column) + 1.0;
        const int one_based_index = (int)index_number;
        final_index = index_number;
        const SValue color = color_storage->items[one_based_index - 1];
        const SString *color_string = color.tag == SJIT_VALUE_STRING ?
            (const SString *)color.ptr : NULL;
        const int is_marker = color_string && color_string->bytes &&
            (color_string->bytes[0] == 'n' || color_string->bytes[0] == 'N') &&
            color_string->bytes[1] == '\0';
        const int list_is_marker = color.tag == SJIT_VALUE_LIST &&
            sjit_eq(runtime, color, replacement_literal->literal);
        if (!is_marker && !list_is_marker) {
            SJIT_PROFILE_INC(runtime, SJIT_PROFILE_PEN_RENDER_PIXEL);
            SJIT_PROFILE_INC(runtime, SJIT_PROFILE_LIST_NUMBER_AT_VALUE);
            const double brightness = jit_list_item_number_at_resolved(
                runtime,
                brightness_list,
                one_based_index);
            SValue native_color = color;
            int native_color_is_number = color.tag == SJIT_VALUE_NUMBER;
            double native_color_number = native_color_is_number ? color.number : 0.0;
            if (color_string && color_string->bytes &&
                color_string->bytes[0] != '#') {
                const double color_number = color_string->number_cache_valid ?
                    (color_string->number_cache_ok &&
                     !isnan(color_string->number_cache) ?
                         color_string->number_cache : 0.0) :
                    sjit_to_number_fast(runtime, color);
                native_color = sjit_make_number_fast(
                    color_number);
                native_color_is_number = 1;
                native_color_number = color_number;
            }
            if (param_id == 3 && native_color_is_number) {
                sjit_pen_set_number_color_and_change_brightness(
                    runtime, sprite, native_color_number, brightness);
            } else {
                sjit_pen_set_color_value_and_change_param_number(
                    runtime,
                    sprite,
                    native_color,
                    param_id,
                    brightness);
            }
            const int zero_based_index = one_based_index - 1;
            SValue replacement = sjit_value_clone_fast(
                replacement_literal->literal);
            sjit_value_destroy_fast(color_storage->items[zero_based_index]);
            color_storage->items[zero_based_index] = replacement;
            if (color_storage->numbers_valid) {
                free(color_storage->numbers);
                color_storage->numbers = NULL;
                color_storage->numbers_valid = 0;
            }
            if (use_raster_tile) {
                SJIT_PROFILE_INC(runtime, SJIT_PROFILE_PEN_STAMP);
                jit_pen_store_raster_tile_pixel(
                    runtime,
                    (int)row,
                    column_offset,
                    sprite);
                sprite->pen_down = 0;
            } else {
                jit_pen_append_reserved_stamp(runtime, sprite, x, y);
                runtime->pen_materialized_valid = 0;
            }
        }
        x += x_step;
        column += 1.0;
    }

    if (use_raster_tile) {
        runtime->pen_raster_tile.rows_filled = (int)row + 1;
        runtime->pen_materialized_valid = 0;
    }
    if (columns > 0) {
        sprite->x = x;
        runtime->redraw_requested = 1;
        sjit_variable_set_number_fast(column_variable, column);
        sjit_variable_set_number_fast(index_variable, final_index);
    }
    if (getenv("SJIT_LOG_NATIVE_PEN_ROW") != NULL) {
        static int logged = 0;
        if (!logged) {
            fprintf(
                stderr,
                "sjit: native pen-row kernel active (%d columns)\n",
                columns);
            logged = 1;
        }
    }
    return 1;
}

double sjit_jit_statement_number(SRuntime *runtime, SCompiledScript *script, SStatement *statement, int expr_slot) {
    return sjit_script_eval_statement_number_ptr(runtime, script, statement, expr_slot);
}

int sjit_jit_statement_bool(SRuntime *runtime, SCompiledScript *script, SStatement *statement, int expr_slot) {
    return sjit_script_eval_statement_bool_ptr(runtime, script, statement, expr_slot);
}

void sjit_jit_statement_set_monitor_visibility(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement) {
    (void)script;
    if (!runtime || !statement || !statement->variable_id) {
        return;
    }
    sjit_runtime_set_variable_monitor_visible(
        runtime,
        sjit_string_cstr(statement->variable_id),
        statement->opcode == SJIT_STMT_MONITOR_SHOW);
}

void sjit_jit_statement_set_variable(SRuntime *runtime, SCompiledScript *script, SStatement *statement) {
    if (!runtime || !script || !statement) {
        return;
    }
    SVariable *variable = jit_statement_variable(runtime, script, statement, SJIT_VAR_SCALAR);
    if (variable &&
        variable->scalar_kind == SJIT_SCALAR_NUMBER &&
        expr_allows_number_set_fast_path(statement->value)) {
        sjit_variable_set_number_fast(
            variable,
            sjit_script_eval_statement_number_ptr(runtime, script, statement, SJIT_STMT_EXPR_VALUE));
        return;
    }
    if (variable && variable->scalar_kind == SJIT_SCALAR_BOOL) {
        SValue value = sjit_script_eval_statement_expr_ptr(runtime, script, statement, SJIT_STMT_EXPR_VALUE);
        sjit_variable_set_bool(variable, sjit_to_bool(runtime, value));
        sjit_value_destroy(value);
        return;
    }
    SValue value = sjit_script_eval_statement_expr_ptr(runtime, script, statement, SJIT_STMT_EXPR_VALUE);
    if (variable) {
        sjit_variable_set_move(variable, value);
    } else {
        sjit_value_destroy(value);
    }
}

void sjit_jit_script_set_variable(SRuntime *runtime, SCompiledScript *script, int statement_index) {
    sjit_jit_statement_set_variable(runtime, script, jit_statement(script, statement_index));
}

void sjit_jit_statement_change_variable(SRuntime *runtime, SCompiledScript *script, SStatement *statement) {
    if (!runtime || !script || !statement) {
        return;
    }
    SVariable *variable = jit_statement_variable(runtime, script, statement, SJIT_VAR_SCALAR);
    if (variable && variable->scalar_kind == SJIT_SCALAR_NUMBER) {
        sjit_variable_change_by_number(
            runtime,
            variable,
            sjit_script_eval_statement_number_ptr(runtime, script, statement, SJIT_STMT_EXPR_VALUE));
        return;
    }
    SValue value = sjit_script_eval_statement_expr_ptr(runtime, script, statement, SJIT_STMT_EXPR_VALUE);
    if (variable) {
        sjit_variable_change_by(runtime, variable, value);
    }
    sjit_value_destroy(value);
}

void sjit_jit_statement_set_variable_number(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    double value) {
    if (!runtime || !script || !statement) {
        return;
    }
    sjit_variable_set_number_fast(
        jit_statement_variable(runtime, script, statement, SJIT_VAR_SCALAR),
        value);
}

void sjit_jit_statement_change_variable_number(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    double value) {
    if (!runtime || !script || !statement) {
        return;
    }
    sjit_variable_change_by_number(
        runtime,
        jit_statement_variable(runtime, script, statement, SJIT_VAR_SCALAR),
        value);
}

void sjit_jit_script_change_variable(SRuntime *runtime, SCompiledScript *script, int statement_index) {
    sjit_jit_statement_change_variable(runtime, script, jit_statement(script, statement_index));
}

void sjit_jit_statement_list_add(SRuntime *runtime, SCompiledScript *script, SStatement *statement) {
    SList *list = jit_script_list(runtime, script, statement);
    if (list && expr_allows_number_set_fast_path(statement ? statement->value : 0)) {
        sjit_list_push_number(
            list,
            sjit_script_eval_statement_number_ptr(runtime, script, statement, SJIT_STMT_EXPR_VALUE));
        return;
    }
    SValue value = sjit_script_eval_statement_expr_ptr(runtime, script, statement, SJIT_STMT_EXPR_VALUE);
    if (list) {
        sjit_list_push_move(list, value);
    } else {
        sjit_value_destroy(value);
    }
}

void sjit_jit_script_list_add(SRuntime *runtime, SCompiledScript *script, int statement_index) {
    sjit_jit_statement_list_add(runtime, script, jit_statement(script, statement_index));
}

void sjit_jit_statement_list_delete(SRuntime *runtime, SCompiledScript *script, SStatement *statement) {
    SList *list = jit_script_list(runtime, script, statement);
    SValue index_value = sjit_script_eval_statement_expr_ptr(runtime, script, statement, SJIT_STMT_EXPR_INDEX);
    const int index = sjit_list_to_index(runtime, index_value, sjit_list_length(list), 1);
    sjit_value_destroy(index_value);
    if (!list || index == SJIT_LIST_INDEX_INVALID) {
        return;
    }
    if (index == SJIT_LIST_INDEX_ALL) {
        sjit_list_clear(list);
    } else {
        sjit_list_delete(list, index);
    }
}

void sjit_jit_script_list_delete(SRuntime *runtime, SCompiledScript *script, int statement_index) {
    sjit_jit_statement_list_delete(runtime, script, jit_statement(script, statement_index));
}

void sjit_jit_statement_list_delete_all(SRuntime *runtime, SCompiledScript *script, SStatement *statement) {
    SList *list = jit_script_list(runtime, script, statement);
    sjit_list_clear(list);
}

void sjit_jit_script_list_delete_all(SRuntime *runtime, SCompiledScript *script, int statement_index) {
    sjit_jit_statement_list_delete_all(runtime, script, jit_statement(script, statement_index));
}

void sjit_jit_statement_list_insert(SRuntime *runtime, SCompiledScript *script, SStatement *statement) {
    SList *list = jit_script_list(runtime, script, statement);
    SValue index_value = sjit_script_eval_statement_expr_ptr(runtime, script, statement, SJIT_STMT_EXPR_INDEX);
    const int index = sjit_list_to_index(runtime, index_value, sjit_list_length(list) + 1, 0);
    sjit_value_destroy(index_value);
    if (!list || index == SJIT_LIST_INDEX_INVALID ||
        index > sjit_list_item_limit(list)) {
        return;
    }
    SValue value = sjit_script_eval_statement_expr_ptr(runtime, script, statement, SJIT_STMT_EXPR_VALUE);
    sjit_list_insert_move(list, index, value);
}

void sjit_jit_script_list_insert(SRuntime *runtime, SCompiledScript *script, int statement_index) {
    sjit_jit_statement_list_insert(runtime, script, jit_statement(script, statement_index));
}

void sjit_jit_statement_list_replace(SRuntime *runtime, SCompiledScript *script, SStatement *statement) {
    SList *list = jit_script_list(runtime, script, statement);
    SValue index_value = sjit_script_eval_statement_expr_ptr(runtime, script, statement, SJIT_STMT_EXPR_INDEX);
    const int index = sjit_list_to_index(runtime, index_value, sjit_list_length(list), 0);
    sjit_value_destroy(index_value);
    if (!list || index == SJIT_LIST_INDEX_INVALID) {
        return;
    }
    if (expr_allows_number_set_fast_path(statement ? statement->value : 0)) {
        sjit_list_replace_number(
            list,
            index,
            sjit_script_eval_statement_number_ptr(runtime, script, statement, SJIT_STMT_EXPR_VALUE));
        return;
    }
    SValue value = sjit_script_eval_statement_expr_ptr(runtime, script, statement, SJIT_STMT_EXPR_VALUE);
    sjit_list_replace_move(list, index, value);
}

void sjit_jit_statement_list_add_number(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    double value) {
    SList *list = jit_script_list(runtime, script, statement);
    sjit_list_push_number(list, value);
}

void sjit_jit_list_variable_add_number(SVariable *list_variable, double value) {
    sjit_list_push_number(jit_list_from_variable(list_variable), value);
}

void sjit_jit_list_variable_clear(SVariable *list_variable) {
    sjit_list_clear(jit_list_from_variable(list_variable));
}

void sjit_jit_list_variable_replace_number_at(
    SRuntime *runtime,
    SVariable *list_variable,
    double index,
    double value) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_LIST_REPLACE_NUMBER);
    SList *list = jit_list_from_variable(list_variable);
    (void)runtime;
    const int resolved_index = jit_list_index_at_number(index, sjit_list_length(list));
    if (list && resolved_index != SJIT_LIST_INDEX_INVALID) {
        jit_list_replace_number_fast(list, resolved_index, value);
    }
}

void sjit_jit_list_variable_replace_number_at_variable(
    SRuntime *runtime,
    SVariable *list_variable,
    SVariable *index_variable,
    double value) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_LIST_REPLACE_NUMBER);
    SList *list = jit_list_from_variable(list_variable);
    const int resolved_index = list && index_variable ? jit_list_index_at_value(
        runtime,
        index_variable->value,
        sjit_list_length(list)) : SJIT_LIST_INDEX_INVALID;
    if (resolved_index != SJIT_LIST_INDEX_INVALID) {
        jit_list_replace_number_fast(list, resolved_index, value);
    }
}

void sjit_jit_list_variable_replace_from_variables(
    SRuntime *runtime,
    SVariable *list_variable,
    SVariable *index_variable,
    SVariable *value_variable) {
    SList *list = jit_list_from_variable(list_variable);
    if (!list || !index_variable) {
        return;
    }
    const int resolved_index = jit_list_index_at_value(
        runtime,
        index_variable->value,
        list->storage->length);
    if (resolved_index == SJIT_LIST_INDEX_INVALID) {
        return;
    }

    const SValue value = value_variable ?
        value_variable->value : sjit_make_number_fast(0.0);
    if (value.tag == SJIT_VALUE_NUMBER) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_LIST_REPLACE_NUMBER);
        jit_list_replace_number_fast(list, resolved_index, value.number);
        return;
    }
    sjit_list_replace(list, resolved_index, value);
}

void sjit_jit_list_variable_replace_list_item_at_variables(
    SRuntime *runtime,
    SVariable *list_variable,
    SVariable *index_variable,
    SVariable *source_list_variable,
    SVariable *source_index_variable) {
    SList *list = jit_list_from_variable(list_variable);
    if (!list || !index_variable) {
        return;
    }
    const int resolved_index = jit_list_index_at_value(
        runtime,
        index_variable->value,
        list->storage->length);
    if (resolved_index == SJIT_LIST_INDEX_INVALID) {
        return;
    }

    const SValue *source_item = source_index_variable ?
        jit_list_item_borrowed_from_list_at_value(
            runtime,
            jit_list_from_variable(source_list_variable),
            source_index_variable->value) : NULL;
    SValue empty = sjit_make_null();
    if (!source_item) {
        empty = sjit_make_string("");
        source_item = &empty;
    }
    if (source_item->tag == SJIT_VALUE_NUMBER) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_LIST_REPLACE_NUMBER);
        jit_list_replace_number_fast(list, resolved_index, source_item->number);
    } else {
        sjit_list_replace(list, resolved_index, *source_item);
    }
    sjit_value_destroy(empty);
}

static void jit_variable_set_borrowed(
    SRuntime *runtime,
    SVariable *destination,
    SValue source) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VARIABLE_SET_FROM_VALUE);
    if (!destination) {
        return;
    }
    if (source.tag == SJIT_VALUE_NUMBER) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VARIABLE_SET_NUMBER_SOURCE);
    } else if (source.tag == SJIT_VALUE_STRING) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VARIABLE_SET_STRING_SOURCE);
    } else if (source.tag == SJIT_VALUE_BOOL) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VARIABLE_SET_BOOL_SOURCE);
    } else if (source.tag == SJIT_VALUE_NULL) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VARIABLE_SET_NULL_SOURCE);
    } else if (source.tag == SJIT_VALUE_LIST) {
        SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VARIABLE_SET_LIST_SOURCE);
    }
    if (destination->scalar_kind == SJIT_SCALAR_BOOL) {
        sjit_variable_set_bool(destination, sjit_to_bool(runtime, source));
        return;
    }
    if (source.tag == SJIT_VALUE_STRING && source.ptr) {
        jit_variable_set_string_borrowed(
            runtime,
            destination,
            (SString *)source.ptr);
        return;
    }
    if (source.tag == SJIT_VALUE_NUMBER) {
        sjit_variable_set_number_fast(destination, source.number);
    } else if (source.tag == SJIT_VALUE_BOOL) {
        sjit_variable_set_bool(destination, source.number != 0.0);
    } else if (source.tag == SJIT_VALUE_NULL) {
        sjit_variable_set_move(destination, source);
    } else {
        sjit_variable_set(destination, source);
    }
}

void sjit_jit_variable_set_string_borrowed(
    SRuntime *runtime,
    SVariable *destination,
    SString *source) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VARIABLE_SET_FROM_VALUE);
    if (!destination || !source) {
        return;
    }
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_VARIABLE_SET_STRING_SOURCE);
    jit_variable_set_string_borrowed(runtime, destination, source);
}

static void jit_list_push_borrowed(SList *list, SValue source) {
    if (!list) {
        return;
    }
    if (source.tag == SJIT_VALUE_NUMBER) {
        sjit_list_push_number(list, source.number);
    } else if (source.tag == SJIT_VALUE_BOOL || source.tag == SJIT_VALUE_NULL) {
        sjit_list_push_move(list, source);
    } else {
        sjit_list_push(list, source);
    }
}

void sjit_jit_variable_set_from_variable(
    SRuntime *runtime,
    SVariable *destination,
    SVariable *source) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_SET_FROM_VARIABLE);
    jit_variable_set_borrowed(
        runtime,
        destination,
        source ? source->value : sjit_make_number_fast(0.0));
}

void sjit_jit_variable_set_from_argument(
    SRuntime *runtime,
    SVariable *destination,
    double *numeric_arguments,
    SValue *arguments,
    int argument_index) {
    SJIT_PROFILE_INC(runtime, SJIT_PROFILE_SET_FROM_ARGUMENT);
    const SValue source = argument_index >= 0 && arguments ?
        arguments[argument_index] :
        sjit_make_number_fast(
            argument_index >= 0 && numeric_arguments ? numeric_arguments[argument_index] : 0.0);
    double number = 0.0;
    if (destination && destination->scalar_kind == SJIT_SCALAR_NUMBER &&
        sjit_value_try_number_for_set(source, &number)) {
        sjit_variable_set_number_fast(destination, number);
        return;
    }
    jit_variable_set_borrowed(runtime, destination, source);
}

void sjit_jit_variable_set_from_literal(
    SRuntime *runtime,
    SVariable *destination,
    SExpr *literal_expr) {
    const SValue source = literal_expr && literal_expr->opcode == SJIT_EXPR_LITERAL ?
        literal_expr->literal : sjit_make_null_fast();
    jit_variable_set_borrowed(runtime, destination, source);
}

void sjit_jit_statement_list_add_from_variable(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SVariable *source) {
    SList *list = jit_script_list(runtime, script, statement);
    jit_list_push_borrowed(list, source ? source->value : sjit_make_number_fast(0.0));
}

void sjit_jit_statement_list_add_from_argument(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    double *numeric_arguments,
    SValue *arguments,
    int argument_index) {
    SList *list = jit_script_list(runtime, script, statement);
    if (!list) {
        return;
    }
    const SValue source = argument_index >= 0 && arguments ?
        arguments[argument_index] :
        sjit_make_number_fast(
            argument_index >= 0 && numeric_arguments ? numeric_arguments[argument_index] : 0.0);
    double number = 0.0;
    if (sjit_value_try_number_for_set(source, &number)) {
        sjit_list_push_number(list, number);
        return;
    }
    jit_list_push_borrowed(list, source);
}

void sjit_jit_statement_list_add_from_literal(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement) {
    SList *list = jit_script_list(runtime, script, statement);
    if (list && statement && statement->value &&
        statement->value->opcode == SJIT_EXPR_LITERAL) {
        jit_list_push_borrowed(list, statement->value->literal);
    }
}

int sjit_jit_statement_list_add_literal_repeated(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    double count) {
    if (!statement || statement->opcode != SJIT_STMT_LIST_ADD ||
        !statement->value || statement->value->opcode != SJIT_EXPR_LITERAL ||
        count != count || count < 0.0) {
        return 0;
    }
    SList *list = jit_script_list(runtime, script, statement);
    if (count > (double)sjit_list_item_limit(list)) {
        return 0;
    }
    if (list) {
        (void)sjit_list_push_repeated(list, statement->value->literal, (int)count);
    }
    return 1;
}

void sjit_jit_statement_list_delete_at_number(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    double index) {
    SList *list = jit_script_list(runtime, script, statement);
    SValue index_value = sjit_make_number(index);
    const int resolved_index = sjit_list_to_index(runtime, index_value, sjit_list_length(list), 1);
    if (!list || resolved_index == SJIT_LIST_INDEX_INVALID) {
        return;
    }
    if (resolved_index == SJIT_LIST_INDEX_ALL) {
        sjit_list_clear(list);
    } else {
        sjit_list_delete(list, resolved_index);
    }
}

void sjit_jit_statement_list_insert_number_at(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    double index,
    double value) {
    SList *list = jit_script_list(runtime, script, statement);
    SValue index_value = sjit_make_number(index);
    const int resolved_index = sjit_list_to_index(runtime, index_value, sjit_list_length(list) + 1, 0);
    if (!list || resolved_index == SJIT_LIST_INDEX_INVALID ||
        resolved_index > sjit_list_item_limit(list)) {
        return;
    }
    sjit_list_insert_move(list, resolved_index, sjit_make_number(value));
}

void sjit_jit_statement_list_replace_number_at(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    double index,
    double value) {
    SList *list = jit_script_list(runtime, script, statement);
    SValue index_value = sjit_make_number(index);
    const int resolved_index = sjit_list_to_index(runtime, index_value, sjit_list_length(list), 0);
    if (!list || resolved_index == SJIT_LIST_INDEX_INVALID) {
        return;
    }
    sjit_list_replace_number(list, resolved_index, value);
}

void sjit_jit_script_list_replace(SRuntime *runtime, SCompiledScript *script, int statement_index) {
    sjit_jit_statement_list_replace(runtime, script, jit_statement(script, statement_index));
}

void sjit_jit_statement_say(SRuntime *runtime, SCompiledScript *script, SStatement *statement) {
    if (!statement) {
        return;
    }
    SValue value = sjit_script_eval_statement_expr_ptr(runtime, script, statement, SJIT_STMT_EXPR_VALUE);
    SValue text = sjit_to_string(runtime, value);
    printf("say: %s\n", sjit_string_cstr((const SString *)text.ptr));
    sjit_value_destroy(value);
    sjit_value_destroy(text);
}

void sjit_jit_script_say(SRuntime *runtime, SCompiledScript *script, int statement_index) {
    sjit_jit_statement_say(runtime, script, jit_statement(script, statement_index));
}

void sjit_jit_statement_pen_set_color(SRuntime *runtime, SCompiledScript *script, SStatement *statement) {
    if (!statement) {
        return;
    }
    SValue value = sjit_script_eval_statement_expr_ptr(runtime, script, statement, SJIT_STMT_EXPR_VALUE);
    sjit_pen_set_color_value(runtime, jit_script_sprite(runtime, script), value);
    sjit_value_destroy(value);
}

void sjit_jit_script_pen_set_color(SRuntime *runtime, SCompiledScript *script, int statement_index) {
    sjit_jit_statement_pen_set_color(runtime, script, jit_statement(script, statement_index));
}

void sjit_jit_statement_pen_change_color_param(SRuntime *runtime, SCompiledScript *script, SStatement *statement) {
    if (!statement) {
        return;
    }
    SValue param_value = sjit_script_eval_statement_expr_ptr(runtime, script, statement, SJIT_STMT_EXPR_INDEX);
    SValue param_text = sjit_to_string(runtime, param_value);
    SValue value = sjit_script_eval_statement_expr_ptr(runtime, script, statement, SJIT_STMT_EXPR_VALUE);
    sjit_pen_change_color_param(
        runtime,
        jit_script_sprite(runtime, script),
        sjit_string_cstr((const SString *)param_text.ptr),
        value);
    sjit_value_destroy(param_value);
    sjit_value_destroy(param_text);
    sjit_value_destroy(value);
}

void sjit_jit_script_pen_change_color_param(SRuntime *runtime, SCompiledScript *script, int statement_index) {
    sjit_jit_statement_pen_change_color_param(runtime, script, jit_statement(script, statement_index));
}

void sjit_jit_statement_set_variable_value_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SValue *value) {
    if (!value) {
        return;
    }
    SValue moved = *value;
    *value = sjit_make_null();
    SVariable *variable = jit_statement_variable(runtime, script, statement, SJIT_VAR_SCALAR);
    double number = 0.0;
    if (variable && variable->scalar_kind == SJIT_SCALAR_NUMBER &&
        statement && statement->value &&
        statement->value->opcode == SJIT_EXPR_ARGUMENT &&
        sjit_value_try_number_for_set(moved, &number)) {
        sjit_variable_set_number_fast(variable, number);
        sjit_value_destroy(moved);
        return;
    }
    if (variable && variable->scalar_kind == SJIT_SCALAR_BOOL) {
        sjit_variable_set_bool(variable, sjit_to_bool(runtime, moved));
        sjit_value_destroy(moved);
        return;
    }
    sjit_variable_set_move(variable, moved);
}

void sjit_jit_statement_list_add_value_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SValue *value) {
    if (!value) {
        return;
    }
    SValue moved = *value;
    *value = sjit_make_null();
    SList *list = jit_script_list(runtime, script, statement);
    double number = 0.0;
    if (list && statement && statement->value &&
        statement->value->opcode == SJIT_EXPR_ARGUMENT &&
        sjit_value_try_number_for_set(moved, &number)) {
        sjit_list_push_number(list, number);
        sjit_value_destroy(moved);
        return;
    }
    if (list) {
        sjit_list_push_move(list, moved);
    } else {
        sjit_value_destroy(moved);
    }
}

void sjit_jit_statement_list_delete_index_value_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SValue *index) {
    if (!index) {
        return;
    }
    SValue moved_index = *index;
    *index = sjit_make_null();
    SList *list = jit_script_list(runtime, script, statement);
    const int resolved_index = sjit_list_to_index(runtime, moved_index, sjit_list_length(list), 1);
    sjit_value_destroy(moved_index);
    if (!list || resolved_index == SJIT_LIST_INDEX_INVALID) {
        return;
    }
    if (resolved_index == SJIT_LIST_INDEX_ALL) {
        sjit_list_clear(list);
    } else {
        sjit_list_delete(list, resolved_index);
    }
}

void sjit_jit_statement_list_insert_value_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SValue *index,
    SValue *value) {
    if (!index || !value) {
        return;
    }
    SValue moved_index = *index;
    SValue moved_value = *value;
    *index = sjit_make_null();
    *value = sjit_make_null();
    SList *list = jit_script_list(runtime, script, statement);
    const int resolved_index = sjit_list_to_index(runtime, moved_index, sjit_list_length(list) + 1, 0);
    sjit_value_destroy(moved_index);
    if (!list || resolved_index == SJIT_LIST_INDEX_INVALID ||
        resolved_index > sjit_list_item_limit(list)) {
        sjit_value_destroy(moved_value);
        return;
    }
    sjit_list_insert_move(list, resolved_index, moved_value);
}

void sjit_jit_statement_list_replace_value_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SStatement *statement,
    SValue *index,
    SValue *value) {
    if (!index || !value) {
        return;
    }
    SValue moved_index = *index;
    SValue moved_value = *value;
    *index = sjit_make_null();
    *value = sjit_make_null();
    SList *list = jit_script_list(runtime, script, statement);
    const int resolved_index = sjit_list_to_index(runtime, moved_index, sjit_list_length(list), 0);
    sjit_value_destroy(moved_index);
    if (!list || resolved_index == SJIT_LIST_INDEX_INVALID) {
        sjit_value_destroy(moved_value);
        return;
    }
    double number = 0.0;
    if (statement && statement->value &&
        statement->value->opcode == SJIT_EXPR_ARGUMENT &&
        sjit_value_try_number_for_set(moved_value, &number)) {
        sjit_list_replace_number(list, resolved_index, number);
        sjit_value_destroy(moved_value);
        return;
    }
    sjit_list_replace_move(list, resolved_index, moved_value);
}

void sjit_jit_statement_say_value_ptr(SRuntime *runtime, SValue *value) {
    if (!value) {
        return;
    }
    SValue moved = *value;
    *value = sjit_make_null();
    SValue text = sjit_to_string(runtime, moved);
    printf("say: %s\n", sjit_string_cstr((const SString *)text.ptr));
    sjit_value_destroy(moved);
    sjit_value_destroy(text);
}

void sjit_jit_statement_pen_set_color_value_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SValue *value) {
    if (!value) {
        return;
    }
    SValue moved = *value;
    *value = sjit_make_null();
    sjit_pen_set_color_value(runtime, jit_script_sprite(runtime, script), moved);
    sjit_value_destroy(moved);
}

void sjit_jit_statement_pen_change_color_param_value_ptr(
    SRuntime *runtime,
    SCompiledScript *script,
    SValue *param,
    SValue *value) {
    if (!param || !value) {
        return;
    }
    SValue moved_param = *param;
    SValue moved_value = *value;
    *param = sjit_make_null();
    *value = sjit_make_null();
    SValue param_text = sjit_to_string(runtime, moved_param);
    sjit_pen_change_color_param(
        runtime,
        jit_script_sprite(runtime, script),
        sjit_string_cstr((const SString *)param_text.ptr),
        moved_value);
    sjit_value_destroy(moved_param);
    sjit_value_destroy(param_text);
    sjit_value_destroy(moved_value);
}

SLoopState *sjit_jit_control_loop_state(SFrame *frame, SStatement *statement, int create) {
    return sjit_control_loop_state_at_depth(frame, statement, 0, create);
}

void sjit_jit_control_loop_reset(SFrame *frame, SStatement *statement) {
    if (statement) {
        statement->loop_state_cache_index = -1;
        statement->loop_state_cache_scope_depth = 0;
    }
    sjit_control_loop_reset_at_depth(frame, statement, 0);
}

SLoopState *sjit_jit_procedure_control_loop_state(
    SFrame *frame,
    SStatement *statement,
    int depth,
    int create) {
    return sjit_control_loop_state_at_depth(frame, statement, depth, create);
}

void sjit_jit_procedure_control_loop_reset(SFrame *frame, SStatement *statement, int depth) {
    if (statement) {
        statement->loop_state_cache_index = -1;
        statement->loop_state_cache_scope_depth = depth;
    }
    sjit_control_loop_reset_at_depth(frame, statement, depth);
}

SLoopState *sjit_jit_procedure_activation_state(
    SFrame *frame,
    const void *key,
    int depth,
    int create) {
    return sjit_control_loop_state_at_depth(frame, key, depth, create);
}

void sjit_jit_procedure_activation_reset(SFrame *frame, const void *key, int depth) {
    sjit_control_loop_reset_at_depth(frame, key, depth);
}

static double jit_scratch_round(double value) {
    if (isnan(value)) {
        return 0.0;
    }
    if (isinf(value)) {
        return value;
    }
    return floor(value + 0.5);
}

int sjit_jit_repeat_should_enter_number(SFrame *frame, SStatement *statement, double times) {
    SLoopState *state = sjit_jit_control_loop_state(frame, statement, 0);
    if (!state) {
        state = sjit_jit_control_loop_state(frame, statement, 1);
        if (state) {
            state->counter = jit_scratch_round(times);
        }
    }
    if (!state) {
        return 0;
    }
    if (state->branch_active) {
        return 1;
    }
    state->counter -= 1.0;
    return state->counter >= 0.0;
}

int sjit_jit_procedure_repeat_should_enter_number(
    SFrame *frame,
    SStatement *statement,
    int depth,
    double times) {
    SLoopState *state = sjit_jit_procedure_control_loop_state(frame, statement, depth, 0);
    if (!state) {
        state = sjit_jit_procedure_control_loop_state(frame, statement, depth, 1);
        if (state) {
            state->counter = jit_scratch_round(times);
        }
    }
    if (!state) {
        return 0;
    }
    if (state->branch_active) {
        return 1;
    }
    state->counter -= 1.0;
    return state->counter >= 0.0;
}

static SLoopState *jit_repeat_number_state(SFrame *frame, SStatement *statement, double times) {
    SLoopState *state = sjit_jit_control_loop_state(frame, statement, 0);
    if (!state) {
        state = sjit_jit_control_loop_state(frame, statement, 1);
        if (state) {
            state->counter = jit_scratch_round(times);
        }
    }
    return state;
}

double sjit_jit_repeat_remaining_number(SFrame *frame, SStatement *statement, double times) {
    SLoopState *state = jit_repeat_number_state(frame, statement, times);
    if (!state || state->branch_active || state->sub_pc != 0) {
        return -1.0;
    }
    if (!isfinite(state->counter)) {
        return -1.0;
    }
    return state->counter > 0.0 ? state->counter : 0.0;
}

double sjit_jit_repeat_take_all_number(SFrame *frame, SStatement *statement, double times) {
    SLoopState *state = jit_repeat_number_state(frame, statement, times);
    if (!state || state->branch_active || state->sub_pc != 0) {
        return -1.0;
    }
    if (!isfinite(state->counter)) {
        return -1.0;
    }
    const double remaining = state->counter > 0.0 ? state->counter : 0.0;
    state->counter = 0.0;
    state->branch_active = 0;
    state->sub_pc = 0;
    return remaining;
}

double sjit_jit_round_repeat_count(double value) {
    return jit_scratch_round(value);
}

SRuntimeStatus sjit_jit_finish_control_branch(SFrame *frame, SLoopState *state) {
    if (state) {
        state->branch_active = 0;
        state->sub_pc = 0;
    }
    if (!frame || !frame->warp_mode) {
        return SJIT_STATUS_YIELDED;
    }
    return SJIT_STATUS_OK;
}

SRuntimeStatus sjit_jit_finish_batched_loop_branch(
    SRuntime *runtime,
    SFrame *frame,
    SLoopState *state,
    int branch_count) {
    if (state) {
        state->branch_active = 0;
        state->sub_pc = 0;
    }
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
