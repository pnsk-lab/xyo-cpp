#ifndef SJIT_OPERATOR_H
#define SJIT_OPERATOR_H

#include "sjit_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

SValue sjit_op_add(SRuntime *runtime, SValue a, SValue b);
SValue sjit_op_sub(SRuntime *runtime, SValue a, SValue b);
SValue sjit_op_mul(SRuntime *runtime, SValue a, SValue b);
SValue sjit_op_div(SRuntime *runtime, SValue a, SValue b);
SValue sjit_op_join(SRuntime *runtime, SValue a, SValue b);
int sjit_op_contains(SRuntime *runtime, SValue text, SValue substring);
int sjit_op_contains_value_ptr(
    SRuntime *runtime,
    const SValue *text,
    const SValue *substring);
SValue sjit_op_mod(SRuntime *runtime, SValue a, SValue b);
SValue sjit_op_round(SRuntime *runtime, SValue x);
double sjit_op_round_number(double number);
SValue sjit_op_mathop(SRuntime *runtime, const char *operator_name, SValue x);
int sjit_op_mathop_id(const char *operator_name);
double sjit_op_mathop_number_id(int operator_id, double number);
int sjit_op_lt(SRuntime *runtime, SValue a, SValue b);
int sjit_op_eq(SRuntime *runtime, SValue a, SValue b);
int sjit_op_gt(SRuntime *runtime, SValue a, SValue b);
int sjit_op_and(SRuntime *runtime, SValue a, SValue b);
int sjit_op_or(SRuntime *runtime, SValue a, SValue b);
int sjit_op_not(SRuntime *runtime, SValue x);

#ifdef __cplusplus
}
#endif

#endif
