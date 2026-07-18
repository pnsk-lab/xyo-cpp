#ifndef SJIT_TARGET_H
#define SJIT_TARGET_H

#include "sjit_abi.h"
#include "sjit_variable.h"

#ifdef __cplusplus
extern "C" {
#endif

struct STarget {
    int id;
    int is_stage;
    int is_original;
    SString *name;
    /* Storage may move when capacity grows, but entries are append-only:
       an existing variable's index is stable for the target's lifetime. */
    SVariable *variables;
    int variable_count;
    int variable_capacity;
};

void sjit_target_init(STarget *target, int id, const char *name, int is_stage, int is_original);
void sjit_target_destroy(STarget *target);
SVariable *sjit_target_lookup_or_create_variable(STarget *target, int id, const char *name, int type);
SVariable *sjit_target_lookup_variable(STarget *target, int id, const char *name, int type);
SVariable *sjit_target_lookup_or_create_variable_by_scratch_id(
    STarget *target,
    const char *scratch_id,
    const char *name,
    int type);
SVariable *sjit_target_lookup_variable_by_scratch_id(
    STarget *target,
    const char *scratch_id,
    const char *name,
    int type);
int sjit_target_copy_variables(STarget *target, const STarget *source);

#ifdef __cplusplus
}
#endif

#endif
