#ifndef SJIT_OWNERSHIP_H
#define SJIT_OWNERSHIP_H

#include "sjit_script.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SJIT_OWNERSHIP_REJECT_NONE = 0,
    SJIT_OWNERSHIP_REJECT_INVALID_REGISTRATION = UINT64_C(1) << 0,
    SJIT_OWNERSHIP_REJECT_INVALID_TREE = UINT64_C(1) << 1,
    SJIT_OWNERSHIP_REJECT_TARGET_NOT_EXCLUSIVE = UINT64_C(1) << 2,
    SJIT_OWNERSHIP_REJECT_NONLOCAL_VARIABLE = UINT64_C(1) << 3,
    SJIT_OWNERSHIP_REJECT_OWNING_VALUE = UINT64_C(1) << 4,
    SJIT_OWNERSHIP_REJECT_LIST_ALIAS = UINT64_C(1) << 5,
    SJIT_OWNERSHIP_REJECT_SHARED_EFFECT = UINT64_C(1) << 6,
    SJIT_OWNERSHIP_REJECT_SHARED_RANDOM = UINT64_C(1) << 7,
    SJIT_OWNERSHIP_REJECT_LIVE_CLOCK = UINT64_C(1) << 8,
    SJIT_OWNERSHIP_REJECT_SHARED_REDRAW = UINT64_C(1) << 9,
    SJIT_OWNERSHIP_REJECT_INTERPRETER_ONLY = UINT64_C(1) << 10,
    SJIT_OWNERSHIP_REJECT_UNKNOWN_OPCODE = UINT64_C(1) << 11,
    SJIT_OWNERSHIP_REJECT_MISSING_PROCEDURE = UINT64_C(1) << 12,
    SJIT_OWNERSHIP_REJECT_RECURSIVE_PROCEDURE = UINT64_C(1) << 13
} SOwnershipRejectFlag;

typedef struct {
    int parallel_safe;
    int owner_target_id;
    uint64_t reject_flags;
} SOwnershipAnalysis;

SOwnershipAnalysis sjit_analyze_script_ownership(
    SRuntime *runtime,
    const SCompiledScript *script);

#ifdef __cplusplus
}
#endif

#endif
