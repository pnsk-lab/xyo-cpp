#ifndef SJIT_OPCODE_EFFECTS_H
#define SJIT_OPCODE_EFFECTS_H

#include "sjit_script.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Conservative, centralized effect information shared by the interpreter and
 * the JIT eligibility/region analysis.  Unknown opcode values deliberately
 * return every effect plus requiresInterpreter=true; callers must never turn
 * an unknown reporter into a literal or an unknown statement into a no-op.
 */
typedef struct OpcodeEffects {
    bool canYield;
    bool canAllocate;
    bool canMutateTarget;
    bool canChangeValueType;
    bool canCallUnknown;
    bool requiresInterpreter;
} OpcodeEffects;

OpcodeEffects sjit_expr_opcode_effects(int opcode);
OpcodeEffects sjit_statement_opcode_effects(int opcode);
const char *sjit_expr_opcode_name(int opcode);
const char *sjit_statement_opcode_name(int opcode);

#ifdef __cplusplus
}
#endif

#endif
