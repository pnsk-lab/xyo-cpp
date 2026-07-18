#ifndef SJIT_COMPARE_H
#define SJIT_COMPARE_H

#include "sjit_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

int sjit_compare(SRuntime *runtime, SValue a, SValue b);
int sjit_eq(SRuntime *runtime, SValue a, SValue b);
int sjit_lt(SRuntime *runtime, SValue a, SValue b);
int sjit_gt(SRuntime *runtime, SValue a, SValue b);

#ifdef __cplusplus
}
#endif

#endif

