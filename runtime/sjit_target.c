#include "sjit_target.h"

#include <stdlib.h>
#include <string.h>

static int ensure_variable_capacity(STarget *target, int wanted) {
    if (!target) {
        return 0;
    }
    if (wanted <= target->variable_capacity) {
        return 1;
    }
    int next = target->variable_capacity > 0 ? target->variable_capacity : SJIT_INITIAL_CAPACITY;
    while (next < wanted) {
        next *= 2;
    }
    SVariable *variables = (SVariable *)realloc(target->variables, sizeof(SVariable) * (size_t)next);
    if (!variables) {
        return 0;
    }
    target->variables = variables;
    target->variable_capacity = next;
    return 1;
}

void sjit_target_init(STarget *target, int id, const char *name, int is_stage, int is_original) {
    if (!target) {
        return;
    }
    target->id = id;
    target->is_stage = is_stage;
    target->is_original = is_original;
    target->name = sjit_string_new(name ? name : "");
    target->variables = NULL;
    target->variable_count = 0;
    target->variable_capacity = 0;
}

void sjit_target_destroy(STarget *target) {
    if (!target) {
        return;
    }
    for (int i = 0; i < target->variable_count; ++i) {
        sjit_variable_destroy(&target->variables[i]);
    }
    free(target->variables);
    target->variables = NULL;
    target->variable_count = 0;
    target->variable_capacity = 0;
    sjit_string_destroy(target->name);
    target->name = NULL;
}

SVariable *sjit_target_lookup_variable(STarget *target, int id, const char *name, int type) {
    if (!target) {
        return NULL;
    }
    for (int i = 0; i < target->variable_count; ++i) {
        SVariable *variable = &target->variables[i];
        if (id != 0 && variable->id == id) {
            return variable;
        }
    }
    for (int i = 0; i < target->variable_count; ++i) {
        SVariable *variable = &target->variables[i];
        if (variable->type == type && sjit_string_equals_ignore_case(variable->name, name ? name : "")) {
            return variable;
        }
    }
    return NULL;
}

SVariable *sjit_target_lookup_variable_by_scratch_id(
    STarget *target,
    const char *scratch_id,
    const char *name,
    int type) {
    if (!target) {
        return NULL;
    }
    if (scratch_id && scratch_id[0] != '\0') {
        for (int i = 0; i < target->variable_count; ++i) {
            SVariable *variable = &target->variables[i];
            if (variable->type == type && variable->scratch_id &&
                strcmp(sjit_string_cstr(variable->scratch_id), scratch_id) == 0) {
                return variable;
            }
        }
        return NULL;
    }
    return sjit_target_lookup_variable(target, 0, name, type);
}

SVariable *sjit_target_lookup_or_create_variable(STarget *target, int id, const char *name, int type) {
    SVariable *existing = sjit_target_lookup_variable(target, id, name, type);
    if (existing) {
        return existing;
    }
    if (!target || !ensure_variable_capacity(target, target->variable_count + 1)) {
        return NULL;
    }
    SVariable *created = &target->variables[target->variable_count++];
    sjit_variable_init(created, id, name ? name : "", type);
    return created;
}

SVariable *sjit_target_lookup_or_create_variable_by_scratch_id(
    STarget *target,
    const char *scratch_id,
    const char *name,
    int type) {
    SVariable *existing = sjit_target_lookup_variable_by_scratch_id(
        target, scratch_id, name, type);
    if (existing) {
        return existing;
    }
    if (!target || !ensure_variable_capacity(target, target->variable_count + 1)) {
        return NULL;
    }
    SVariable *created = &target->variables[target->variable_count++];
    sjit_variable_init_with_scratch_id(created, scratch_id, name ? name : "", type);
    return created;
}

int sjit_target_copy_variables(STarget *target, const STarget *source) {
    if (!target || !source || !ensure_variable_capacity(target, source->variable_count)) {
        return 0;
    }
    for (int i = 0; i < source->variable_count; ++i) {
        target->variables[i] = sjit_variable_clone(&source->variables[i]);
    }
    target->variable_count = source->variable_count;
    return 1;
}
