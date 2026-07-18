#include "sjit_ownership_internal.h"

#include "sjit_opcode_effects.h"
#include "sjit_string.h"
#include "sjit_target.h"

#include <stdlib.h>
#include <string.h>

enum {
    SJIT_OWNERSHIP_MAX_TREE_DEPTH = 512
};

typedef enum {
    SJIT_EXPR_RESULT_INVALID = 0,
    SJIT_EXPR_RESULT_PRIMITIVE = 1,
    SJIT_EXPR_RESULT_OWNED = 2
} SOwnershipExprResult;

typedef struct {
    SRuntime *runtime;
    const SCompiledScript *script;
    SSprite *owner;
    unsigned char *procedure_states;
    int *owned_variable_indices;
    int owned_variable_count;
    int owned_variable_capacity;
    uint64_t reject_flags;
} SOwnershipContext;

static SOwnershipExprResult analyze_expr(
    SOwnershipContext *context,
    const SExpr *expr,
    int depth);
static int analyze_statements(
    SOwnershipContext *context,
    const SStatement *statements,
    int count,
    int depth);

static int owned_variable_index(
    const SOwnershipContext *context,
    const SVariable *variable);

static int reject_with(SOwnershipContext *context, uint64_t flag) {
    if (context) {
        context->reject_flags |= flag;
    }
    return 0;
}

static SOwnershipExprResult reject_expr(
    SOwnershipContext *context,
    uint64_t flag) {
    reject_with(context, flag);
    return SJIT_EXPR_RESULT_INVALID;
}

static int value_is_primitive(SValue value) {
    return value.tag == SJIT_VALUE_NUMBER ||
        value.tag == SJIT_VALUE_BOOL ||
        value.tag == SJIT_VALUE_NULL;
}

static int record_owned_variable(
    SOwnershipContext *context,
    const SVariable *variable) {
    const int index = owned_variable_index(context, variable);
    if (!context || index < 0) {
        return reject_with(context, SJIT_OWNERSHIP_REJECT_INVALID_TREE);
    }
    for (int i = 0; i < context->owned_variable_count; ++i) {
        if (context->owned_variable_indices[i] == index) {
            return 1;
        }
    }
    if (context->owned_variable_count == context->owned_variable_capacity) {
        int next = 4;
        if (context->owned_variable_capacity > 0) {
            next = context->owned_variable_capacity >
                    context->owner->base.variable_count / 2 ?
                context->owner->base.variable_count :
                context->owned_variable_capacity * 2;
        }
        if (next < context->owned_variable_capacity ||
            next > context->owner->base.variable_count) {
            next = context->owner->base.variable_count;
        }
        if (next <= context->owned_variable_count) {
            return reject_with(
                context, SJIT_OWNERSHIP_REJECT_INVALID_TREE);
        }
        int *indices = (int *)realloc(
            context->owned_variable_indices,
            sizeof(int) * (size_t)next);
        if (!indices) {
            return reject_with(
                context, SJIT_OWNERSHIP_REJECT_INVALID_TREE);
        }
        context->owned_variable_indices = indices;
        context->owned_variable_capacity = next;
    }
    context->owned_variable_indices[context->owned_variable_count++] = index;
    return 1;
}

static SVariable *owned_scalar(
    SOwnershipContext *context,
    const SString *scratch_id,
    const SString *name) {
    if (!context || !context->owner || !name) {
        reject_with(context, SJIT_OWNERSHIP_REJECT_INVALID_TREE);
        return NULL;
    }
    SVariable *variable = sjit_target_lookup_variable(
        &context->owner->base,
        0,
        sjit_string_cstr(name),
        SJIT_VAR_SCALAR);
    if (scratch_id && sjit_string_cstr(scratch_id)[0] != '\0') {
        variable = sjit_target_lookup_variable_by_scratch_id(
            &context->owner->base,
            sjit_string_cstr(scratch_id),
            sjit_string_cstr(name),
            SJIT_VAR_SCALAR);
    }
    if (!variable) {
        reject_with(context, SJIT_OWNERSHIP_REJECT_NONLOCAL_VARIABLE);
        return NULL;
    }
    /* Restrict the first parallel tier to pointer-free scalar storage.  This
       makes the proof independent of non-atomic SString/SList refcounts and
       of any historical aliasing between targets. */
    if (variable->is_cloud ||
        (variable->scalar_kind != SJIT_SCALAR_NUMBER &&
         variable->scalar_kind != SJIT_SCALAR_BOOL) ||
        !value_is_primitive(variable->value)) {
        reject_with(context, SJIT_OWNERSHIP_REJECT_OWNING_VALUE);
        return NULL;
    }
    if (!record_owned_variable(context, variable)) {
        return NULL;
    }
    return variable;
}

static int owned_variable_index(
    const SOwnershipContext *context,
    const SVariable *variable) {
    if (!context || !context->owner || !variable) {
        return -1;
    }
    for (int i = 0; i < context->owner->base.variable_count; ++i) {
        if (&context->owner->base.variables[i] == variable) {
            return i;
        }
    }
    return -1;
}

static int validate_owned_scalar_cache(
    SOwnershipContext *context,
    const SVariable *variable,
    SSprite *cache_owner,
    int cache_target_id,
    int cache_owner_target_id,
    int cache_index,
    int cache_type,
    int cache_owner_is_original,
    SRuntime *cache_runtime,
    uint64_t cache_runtime_instance_id) {
    if (!context || !context->runtime || !context->script ||
        !context->owner || !variable) {
        return reject_with(
            context, SJIT_OWNERSHIP_REJECT_INVALID_TREE);
    }
    const int runtime_cache_active =
        cache_runtime == context->runtime &&
        context->runtime->instance_id != 0 &&
        cache_runtime_instance_id == context->runtime->instance_id;
    if (!runtime_cache_active) {
        /* Runtime-specialized native IR captured these locations at compile
           time.  Missing/stale metadata cannot be repaired by its helper's
           interpreter slow path. */
        if (context->script->jit_runtime_instance_id ==
            context->runtime->instance_id) {
            return reject_with(
                context, SJIT_OWNERSHIP_REJECT_INVALID_REGISTRATION);
        }
        return 1;
    }
    const int expected_index =
        owned_variable_index(context, variable);
    if (expected_index < 0 ||
        cache_target_id != context->script->target_id ||
        cache_owner_target_id != context->owner->base.id ||
        cache_index != expected_index ||
        cache_type != SJIT_VAR_SCALAR ||
        !cache_owner_is_original ||
        (cache_owner && cache_owner != context->owner)) {
        return reject_with(
            context, SJIT_OWNERSHIP_REJECT_INVALID_REGISTRATION);
    }
    return 1;
}

static int validate_expr_owned_scalar_cache(
    SOwnershipContext *context,
    const SExpr *expr,
    const SVariable *variable) {
    return expr && validate_owned_scalar_cache(
        context,
        variable,
        expr->variable_cache_owner,
        expr->variable_cache_target_id,
        expr->variable_cache_owner_target_id,
        expr->variable_cache_index,
        expr->variable_cache_type,
        expr->variable_cache_owner_is_original,
        expr->variable_cache_runtime,
        expr->variable_cache_runtime_instance_id);
}

static int validate_statement_owned_scalar_cache(
    SOwnershipContext *context,
    const SStatement *statement,
    const SVariable *variable) {
    return statement && validate_owned_scalar_cache(
        context,
        variable,
        statement->variable_cache_owner,
        statement->variable_cache_target_id,
        statement->variable_cache_owner_target_id,
        statement->variable_cache_index,
        statement->variable_cache_type,
        statement->variable_cache_owner_is_original,
        statement->variable_cache_runtime,
        statement->variable_cache_runtime_instance_id);
}

static SOwnershipExprResult analyze_unary(
    SOwnershipContext *context,
    const SExpr *expr,
    int depth,
    SOwnershipExprResult result) {
    if (analyze_expr(context, expr ? expr->left : NULL, depth + 1) ==
        SJIT_EXPR_RESULT_INVALID) {
        return SJIT_EXPR_RESULT_INVALID;
    }
    return result;
}

static SOwnershipExprResult analyze_binary(
    SOwnershipContext *context,
    const SExpr *expr,
    int depth,
    SOwnershipExprResult result) {
    if (analyze_expr(context, expr ? expr->left : NULL, depth + 1) ==
            SJIT_EXPR_RESULT_INVALID ||
        analyze_expr(context, expr ? expr->right : NULL, depth + 1) ==
            SJIT_EXPR_RESULT_INVALID) {
        return SJIT_EXPR_RESULT_INVALID;
    }
    return result;
}

static SOwnershipExprResult analyze_expr(
    SOwnershipContext *context,
    const SExpr *expr,
    int depth) {
    if (!expr) {
        /* A missing Scratch input is evaluated as null. */
        return SJIT_EXPR_RESULT_PRIMITIVE;
    }
    if (depth > SJIT_OWNERSHIP_MAX_TREE_DEPTH) {
        return reject_expr(context, SJIT_OWNERSHIP_REJECT_INVALID_TREE);
    }
    const OpcodeEffects opcode_effects = sjit_expr_opcode_effects(expr->opcode);
    if (opcode_effects.canCallUnknown) {
        return reject_expr(context, SJIT_OWNERSHIP_REJECT_UNKNOWN_OPCODE);
    }
    switch (expr->opcode) {
    case SJIT_EXPR_LITERAL:
        if (value_is_primitive(expr->literal)) {
            return SJIT_EXPR_RESULT_PRIMITIVE;
        }
        if (expr->literal.tag == SJIT_VALUE_STRING && expr->literal.ptr &&
            ((const SString *)expr->literal.ptr)->ref_count == 1) {
            /* Literal strings belong to this immutable compiled-script tree;
               one active thread per registration prevents cache/refcount
               sharing between pool tasks. */
            return SJIT_EXPR_RESULT_OWNED;
        }
        return reject_expr(context, SJIT_OWNERSHIP_REJECT_OWNING_VALUE);
    case SJIT_EXPR_TIMER:
    case SJIT_EXPR_MOUSE_X:
    case SJIT_EXPR_MOUSE_Y:
    case SJIT_EXPR_MOUSE_DOWN:
    case SJIT_EXPR_X_POSITION:
    case SJIT_EXPR_Y_POSITION:
        return SJIT_EXPR_RESULT_PRIMITIVE;
    case SJIT_EXPR_VARIABLE:
        if (expr->literal.tag != SJIT_VALUE_STRING || !expr->literal.ptr) {
            return SJIT_EXPR_RESULT_INVALID;
        }
        {
            SVariable *variable = owned_scalar(
                context,
                expr->variable_id,
                (const SString *)expr->literal.ptr);
            if (!variable ||
                !validate_expr_owned_scalar_cache(
                    context, expr, variable)) {
                return SJIT_EXPR_RESULT_INVALID;
            }
        }
        return SJIT_EXPR_RESULT_PRIMITIVE;
    case SJIT_EXPR_ARGUMENT:
        /* Argument storage belongs to the current thread/procedure frame.
           It is safe to read, but is not proven pointer-free. */
        return SJIT_EXPR_RESULT_OWNED;
    case SJIT_EXPR_ADD:
    case SJIT_EXPR_SUB:
    case SJIT_EXPR_MUL:
    case SJIT_EXPR_DIV:
    case SJIT_EXPR_MOD:
    case SJIT_EXPR_LT:
    case SJIT_EXPR_EQ:
    case SJIT_EXPR_GT:
    case SJIT_EXPR_AND:
    case SJIT_EXPR_OR:
    case SJIT_EXPR_CONTAINS:
        return analyze_binary(
            context, expr, depth, SJIT_EXPR_RESULT_PRIMITIVE);
    case SJIT_EXPR_JOIN:
    case SJIT_EXPR_LETTER_OF:
        return analyze_binary(context, expr, depth, SJIT_EXPR_RESULT_OWNED);
    case SJIT_EXPR_NOT:
    case SJIT_EXPR_MATHOP:
    case SJIT_EXPR_ROUND:
    case SJIT_EXPR_LENGTH:
    case SJIT_EXPR_KEY_PRESSED:
        return analyze_unary(
            context, expr, depth, SJIT_EXPR_RESULT_PRIMITIVE);
    case SJIT_EXPR_RANDOM:
        return reject_expr(context, SJIT_OWNERSHIP_REJECT_SHARED_RANDOM);
    case SJIT_EXPR_DAYS_SINCE_2000:
        return reject_expr(context, SJIT_OWNERSHIP_REJECT_LIVE_CLOCK);
    case SJIT_EXPR_DIRECTION:
        return reject_expr(context, SJIT_OWNERSHIP_REJECT_INTERPRETER_ONLY);
    case SJIT_EXPR_LIST_ITEM:
    case SJIT_EXPR_LIST_ITEM_NUMBER:
    case SJIT_EXPR_LIST_LENGTH:
    case SJIT_EXPR_LIST_CONTAINS:
    case SJIT_EXPR_LIST_VARIABLE:
        return reject_expr(context, SJIT_OWNERSHIP_REJECT_LIST_ALIAS);
    default:
        return reject_expr(context, SJIT_OWNERSHIP_REJECT_UNKNOWN_OPCODE);
    }
}

static int find_procedure_index(
    const SCompiledScript *script,
    const SString *name) {
    if (!script || !name || script->procedure_count < 0 ||
        (script->procedure_count > 0 && !script->procedures)) {
        return -1;
    }
    const char *needle = sjit_string_cstr(name);
    for (int i = 0; i < script->procedure_count; ++i) {
        if (script->procedures[i].name &&
            strcmp(sjit_string_cstr(script->procedures[i].name), needle) == 0) {
            return i;
        }
    }
    return -1;
}

static int analyze_procedure(
    SOwnershipContext *context,
    int procedure_index,
    int depth) {
    if (!context || !context->script || !context->procedure_states ||
        procedure_index < 0 ||
        procedure_index >= context->script->procedure_count) {
        return reject_with(context, SJIT_OWNERSHIP_REJECT_MISSING_PROCEDURE);
    }
    unsigned char *state = &context->procedure_states[procedure_index];
    if (*state == 2) {
        return 1;
    }
    if (*state == 3) {
        return 0;
    }
    if (*state == 1) {
        *state = 3;
        return reject_with(
            context, SJIT_OWNERSHIP_REJECT_RECURSIVE_PROCEDURE);
    }
    *state = 1;
    const SCompiledProcedure *procedure =
        &context->script->procedures[procedure_index];
    if (procedure->argument_count < 0 ||
        (procedure->argument_count > 0 && !procedure->argument_names) ||
        !analyze_statements(
            context,
            procedure->statements,
            procedure->statement_count,
            depth + 1)) {
        *state = 3;
        return 0;
    }
    *state = 2;
    return 1;
}

static int analyze_statement(
    SOwnershipContext *context,
    const SStatement *statement,
    int depth) {
    if (!statement || depth > SJIT_OWNERSHIP_MAX_TREE_DEPTH) {
        return reject_with(context, SJIT_OWNERSHIP_REJECT_INVALID_TREE);
    }
    const OpcodeEffects opcode_effects =
        sjit_statement_opcode_effects(statement->opcode);
    if (opcode_effects.requiresInterpreter) {
        return reject_with(context, SJIT_OWNERSHIP_REJECT_UNKNOWN_OPCODE);
    }
    switch (statement->opcode) {
    case SJIT_STMT_NOOP:
    case SJIT_STMT_STOP_THIS_SCRIPT:
        return 1;
    case SJIT_STMT_SET_VARIABLE: {
        SVariable *variable = owned_scalar(
            context, statement->variable_id, statement->variable_name);
        const SOwnershipExprResult value =
            analyze_expr(context, statement->value, depth + 1);
        if (!variable ||
            !validate_statement_owned_scalar_cache(
                context, statement, variable) ||
            value == SJIT_EXPR_RESULT_INVALID) {
            return 0;
        }
        if (value != SJIT_EXPR_RESULT_PRIMITIVE) {
            return reject_with(context, SJIT_OWNERSHIP_REJECT_OWNING_VALUE);
        }
        return 1;
    }
    case SJIT_STMT_CHANGE_VARIABLE: {
        SVariable *variable = owned_scalar(
            context, statement->variable_id, statement->variable_name);
        return variable &&
            validate_statement_owned_scalar_cache(
                context, statement, variable) &&
            analyze_expr(context, statement->value, depth + 1) !=
                SJIT_EXPR_RESULT_INVALID;
    }
    case SJIT_STMT_REPEAT:
        return analyze_expr(context, statement->times, depth + 1) !=
                SJIT_EXPR_RESULT_INVALID &&
            analyze_statements(
                context,
                statement->substack,
                statement->substack_count,
                depth + 1);
    case SJIT_STMT_REPEAT_UNTIL:
    case SJIT_STMT_WHILE:
        return analyze_expr(context, statement->condition, depth + 1) !=
                SJIT_EXPR_RESULT_INVALID &&
            analyze_statements(
                context,
                statement->substack,
                statement->substack_count,
                depth + 1);
    case SJIT_STMT_IF:
        return analyze_expr(context, statement->condition, depth + 1) !=
                SJIT_EXPR_RESULT_INVALID &&
            analyze_statements(
                context,
                statement->substack,
                statement->substack_count,
                depth + 1);
    case SJIT_STMT_IF_ELSE:
        return analyze_expr(context, statement->condition, depth + 1) !=
                SJIT_EXPR_RESULT_INVALID &&
            analyze_statements(
                context,
                statement->substack,
                statement->substack_count,
                depth + 1) &&
            analyze_statements(
                context,
                statement->substack2,
                statement->substack2_count,
                depth + 1);
    case SJIT_STMT_FOREVER:
        return analyze_statements(
            context,
            statement->substack,
            statement->substack_count,
            depth + 1);
    case SJIT_STMT_FOR_EACH: {
        SVariable *variable = owned_scalar(
            context, statement->variable_id, statement->variable_name);
        return variable &&
            validate_statement_owned_scalar_cache(
                context, statement, variable) &&
            analyze_expr(context, statement->times, depth + 1) !=
                SJIT_EXPR_RESULT_INVALID &&
            analyze_statements(
                context,
                statement->substack,
                statement->substack_count,
                depth + 1);
    }
    case SJIT_STMT_PROCEDURE_CALL: {
        if (statement->argument_count < 0 ||
            (statement->argument_count > 0 && !statement->arguments)) {
            return reject_with(context, SJIT_OWNERSHIP_REJECT_INVALID_TREE);
        }
        for (int i = 0; i < statement->argument_count; ++i) {
            if (!statement->arguments[i].name ||
                analyze_expr(
                    context,
                    statement->arguments[i].value,
                    depth + 1) == SJIT_EXPR_RESULT_INVALID) {
                return reject_with(context, SJIT_OWNERSHIP_REJECT_INVALID_TREE);
            }
        }
        const int procedure_index =
            find_procedure_index(context->script, statement->procedure_name);
        if (procedure_index < 0) {
            return reject_with(
                context, SJIT_OWNERSHIP_REJECT_MISSING_PROCEDURE);
        }
        if (statement->procedure_cache_valid &&
            statement->procedure_cache_index != procedure_index) {
            return reject_with(
                context, SJIT_OWNERSHIP_REJECT_INVALID_TREE);
        }
        const SCompiledProcedure *procedure =
            &context->script->procedures[procedure_index];
        if (procedure->argument_count != statement->argument_count) {
            return reject_with(
                context, SJIT_OWNERSHIP_REJECT_INVALID_TREE);
        }
        for (int i = 0; i < statement->argument_count; ++i) {
            if (!procedure->argument_names[i] ||
                strcmp(
                    sjit_string_cstr(procedure->argument_names[i]),
                    sjit_string_cstr(statement->arguments[i].name)) != 0) {
                return reject_with(
                    context, SJIT_OWNERSHIP_REJECT_INVALID_TREE);
            }
        }
        return analyze_procedure(context, procedure_index, depth + 1);
    }
    case SJIT_STMT_LIST_ADD:
    case SJIT_STMT_LIST_DELETE:
    case SJIT_STMT_LIST_DELETE_ALL:
    case SJIT_STMT_LIST_INSERT:
    case SJIT_STMT_LIST_REPLACE:
        return reject_with(context, SJIT_OWNERSHIP_REJECT_LIST_ALIAS);
    case SJIT_STMT_WAIT:
    case SJIT_STMT_WAIT_UNTIL:
        return reject_with(context, SJIT_OWNERSHIP_REJECT_SHARED_REDRAW);
    case SJIT_STMT_RESET_TIMER:
    case SJIT_STMT_SAY:
    case SJIT_STMT_PEN_CLEAR:
    case SJIT_STMT_PEN_DOWN:
    case SJIT_STMT_PEN_UP:
    case SJIT_STMT_PEN_SET_SIZE:
    case SJIT_STMT_PEN_SET_COLOR:
    case SJIT_STMT_PEN_CHANGE_COLOR_PARAM:
    case SJIT_STMT_MOTION_SET_X:
    case SJIT_STMT_MOTION_SET_Y:
    case SJIT_STMT_MOTION_CHANGE_X:
    case SJIT_STMT_MOTION_CHANGE_Y:
    case SJIT_STMT_LOOKS_SET_SIZE:
    case SJIT_STMT_MOTION_GOTO_XY:
    case SJIT_STMT_LOOKS_SHOW:
    case SJIT_STMT_LOOKS_HIDE:
    case SJIT_STMT_BROADCAST:
    case SJIT_STMT_STOP_OTHER_SCRIPTS:
    case SJIT_STMT_LOOKS_SAY_FOR_SECS:
    case SJIT_STMT_MONITOR_SHOW:
    case SJIT_STMT_MONITOR_HIDE:
    case SJIT_STMT_STOP_ALL:
    case SJIT_STMT_LOOKS_SWITCH_BACKDROP:
    case SJIT_STMT_LOOKS_SET_EFFECT:
    case SJIT_STMT_LOOKS_CHANGE_EFFECT:
    case SJIT_STMT_LOOKS_CLEAR_EFFECTS:
    case SJIT_STMT_SENSING_SET_DRAG_MODE:
        return reject_with(context, SJIT_OWNERSHIP_REJECT_SHARED_EFFECT);
    default:
        return reject_with(context, SJIT_OWNERSHIP_REJECT_UNKNOWN_OPCODE);
    }
}

static int analyze_statements(
    SOwnershipContext *context,
    const SStatement *statements,
    int count,
    int depth) {
    if (count < 0 || (count > 0 && !statements) ||
        depth > SJIT_OWNERSHIP_MAX_TREE_DEPTH) {
        return reject_with(context, SJIT_OWNERSHIP_REJECT_INVALID_TREE);
    }
    for (int i = 0; i < count; ++i) {
        if (!analyze_statement(context, &statements[i], depth + 1)) {
            return 0;
        }
    }
    return 1;
}

SOwnershipManifest sjit_analyze_script_ownership_manifest(
    SRuntime *runtime,
    const SCompiledScript *script) {
    SOwnershipManifest manifest = {
        {0, 0, SJIT_OWNERSHIP_REJECT_NONE}, NULL, 0};
    SOwnershipAnalysis *result = &manifest.analysis;
    SOwnershipContext context = {0};
    context.runtime = runtime;
    context.script = script;
    if (!runtime || !script) {
        result->reject_flags = SJIT_OWNERSHIP_REJECT_INVALID_TREE;
        return manifest;
    }
    SSprite *owner = sjit_runtime_get_sprite(runtime, script->target_id);
    if (!owner || owner->base.is_stage || !owner->base.is_original) {
        result->reject_flags = SJIT_OWNERSHIP_REJECT_TARGET_NOT_EXCLUSIVE;
        return manifest;
    }
    context.owner = owner;
    result->owner_target_id = owner->base.id;
    if (script->procedure_count < 0 ||
        (script->procedure_count > 0 && !script->procedures)) {
        result->reject_flags = SJIT_OWNERSHIP_REJECT_INVALID_TREE;
        return manifest;
    }
    for (int i = 0; i < script->procedure_count; ++i) {
        const SCompiledProcedure *procedure = &script->procedures[i];
        if (!procedure->name || procedure->argument_count < 0 ||
            (procedure->argument_count > 0 && !procedure->argument_names)) {
            result->reject_flags = SJIT_OWNERSHIP_REJECT_INVALID_TREE;
            return manifest;
        }
        for (int argument = 0;
             argument < procedure->argument_count;
             ++argument) {
            if (!procedure->argument_names[argument]) {
                result->reject_flags = SJIT_OWNERSHIP_REJECT_INVALID_TREE;
                return manifest;
            }
        }
        for (int other = i + 1; other < script->procedure_count; ++other) {
            if (script->procedures[other].name &&
                strcmp(
                    sjit_string_cstr(procedure->name),
                    sjit_string_cstr(script->procedures[other].name)) == 0) {
                result->reject_flags = SJIT_OWNERSHIP_REJECT_INVALID_TREE;
                return manifest;
            }
        }
    }
    if (script->procedure_count > 0) {
        context.procedure_states = (unsigned char *)calloc(
            (size_t)script->procedure_count,
            sizeof(unsigned char));
        if (!context.procedure_states) {
            result->reject_flags = SJIT_OWNERSHIP_REJECT_INVALID_TREE;
            return manifest;
        }
    }
    const int safe = analyze_statements(
        &context, script->statements, script->statement_count, 0);
    free(context.procedure_states);
    result->reject_flags = context.reject_flags;
    result->parallel_safe = safe && result->reject_flags == 0;
    if (result->parallel_safe) {
        manifest.owned_variable_indices = context.owned_variable_indices;
        manifest.owned_variable_count = context.owned_variable_count;
    } else {
        free(context.owned_variable_indices);
    }
    return manifest;
}

void sjit_ownership_manifest_destroy(SOwnershipManifest *manifest) {
    if (!manifest) {
        return;
    }
    free(manifest->owned_variable_indices);
    manifest->owned_variable_indices = NULL;
    manifest->owned_variable_count = 0;
}

SOwnershipAnalysis sjit_analyze_script_ownership(
    SRuntime *runtime,
    const SCompiledScript *script) {
    SOwnershipManifest manifest =
        sjit_analyze_script_ownership_manifest(runtime, script);
    const SOwnershipAnalysis analysis = manifest.analysis;
    sjit_ownership_manifest_destroy(&manifest);
    return analysis;
}
