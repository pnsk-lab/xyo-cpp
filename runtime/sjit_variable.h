#ifndef SJIT_VARIABLE_H
#define SJIT_VARIABLE_H

#include "sjit_abi.h"
#include "sjit_string.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SJIT_VAR_SCALAR = 0,
    SJIT_VAR_LIST = 1,
    SJIT_VAR_BROADCAST = 2
} SVariableType;

typedef enum {
    SJIT_SCALAR_DYNAMIC = 0,
    SJIT_SCALAR_NUMBER = 1,
    SJIT_SCALAR_STRING = 2,
    SJIT_SCALAR_BOOL = 3
} SScalarKind;

typedef struct {
    int id;
    SString *name;
    int type;
    int is_cloud;
    int scalar_kind;
    SValue value;
    /* Scratch/SB3 uses string IDs.  Keep the legacy integer id for the
       public C API, but retain the serialized identity separately. */
    SString *scratch_id;
} SVariable;

void sjit_variable_init(SVariable *variable, int id, const char *name, int type);
void sjit_variable_init_with_scratch_id(
    SVariable *variable,
    const char *scratch_id,
    const char *name,
    int type);
void sjit_variable_destroy(SVariable *variable);
void sjit_variable_set_scalar_kind(SVariable *variable, int scalar_kind);
void sjit_variable_set(SVariable *variable, SValue value);
void sjit_variable_set_move(SVariable *variable, SValue value);
void sjit_variable_set_number(SVariable *variable, double number);
void sjit_variable_set_bool(SVariable *variable, int value);
void sjit_variable_set_list_item_limit(SVariable *variable, int item_limit);
double sjit_variable_number(SRuntime *runtime, const SVariable *variable);
void sjit_variable_change_by(SRuntime *runtime, SVariable *variable, SValue delta);
void sjit_variable_change_by_number(SRuntime *runtime, SVariable *variable, double delta);
SVariable sjit_variable_clone(const SVariable *variable);

static inline __attribute__((always_inline)) void sjit_variable_set_number_fast(
    SVariable *variable,
    double number) {
    if (variable && variable->value.tag == SJIT_VALUE_NUMBER) {
        variable->value.number = number;
        variable->value.ptr = NULL;
        return;
    }
    sjit_variable_set_number(variable, number);
}

#ifdef __cplusplus
}
#endif

#endif
