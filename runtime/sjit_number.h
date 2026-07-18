#ifndef SJIT_NUMBER_H
#define SJIT_NUMBER_H

#include "sjit_abi.h"
#include "sjit_string.h"

#ifdef __cplusplus
extern "C" {
#endif

double sjit_to_number(SRuntime *runtime, SValue value);
int sjit_to_bool(SRuntime *runtime, SValue value);
SValue sjit_to_string(SRuntime *runtime, SValue value);
int sjit_parse_number_for_compare(SValue value, double *out_number, int *out_is_whitespace);
int sjit_value_try_number_for_set(SValue value, double *out_number);

static inline __attribute__((always_inline)) double sjit_to_number_fast(
    SRuntime *runtime,
    SValue value) {
    if (value.tag == SJIT_VALUE_NUMBER) {
        return value.number != value.number ? 0.0 : value.number;
    }
    if (value.tag == SJIT_VALUE_BOOL) {
        return value.number != 0.0 ? 1.0 : 0.0;
    }
    if (value.tag == SJIT_VALUE_NULL) {
        return 0.0;
    }
    return sjit_to_number(runtime, value);
}

static inline __attribute__((always_inline)) int sjit_parse_number_for_compare_fast(
    SValue value,
    double *out_number,
    int *out_is_whitespace) {
    if (out_is_whitespace) {
        *out_is_whitespace = 0;
    }
    switch (value.tag) {
    case SJIT_VALUE_NUMBER:
        if (out_number) {
            *out_number = value.number;
        }
        return 1;
    case SJIT_VALUE_BOOL:
        if (out_number) {
            *out_number = value.number != 0.0 ? 1.0 : 0.0;
        }
        return 1;
    case SJIT_VALUE_NULL:
        if (out_number) {
            *out_number = 0.0;
        }
        if (out_is_whitespace) {
            *out_is_whitespace = 1;
        }
        return 1;
    case SJIT_VALUE_STRING: {
        SString *string = (SString *)value.ptr;
        if (!string) {
            if (out_number) {
                *out_number = 0.0;
            }
            if (out_is_whitespace) {
                *out_is_whitespace = 1;
            }
            return 1;
        }
        if (string->number_cache_valid) {
            if (out_number) {
                *out_number = string->number_cache;
            }
            if (out_is_whitespace) {
                *out_is_whitespace = string->number_cache_whitespace;
            }
            return string->number_cache_ok;
        }
        break;
    }
    default:
        break;
    }
    return sjit_parse_number_for_compare(value, out_number, out_is_whitespace);
}

#ifdef __cplusplus
}
#endif

#endif
