#ifndef SJIT_VALUE_H
#define SJIT_VALUE_H

#include "sjit_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

SValue sjit_make_number(double number);
SValue sjit_make_bool(int value);
SValue sjit_make_null(void);
SValue sjit_make_string(const char *bytes);
SValue sjit_make_string_len(const char *bytes, int length);
SValue sjit_make_list(SList *list);
SValue sjit_value_clone(SValue value);
void sjit_value_destroy(SValue value);
const char *sjit_value_debug_cstr(SValue value);

static inline SValue sjit_make_number_fast(double number) {
    SValue value;
    value.tag = SJIT_VALUE_NUMBER;
    value.number = number;
    value.ptr = 0;
    return value;
}

static inline SValue sjit_make_bool_fast(int raw) {
    SValue value;
    value.tag = SJIT_VALUE_BOOL;
    value.number = raw ? 1.0 : 0.0;
    value.ptr = 0;
    return value;
}

static inline SValue sjit_make_null_fast(void) {
    SValue value;
    value.tag = SJIT_VALUE_NULL;
    value.number = 0.0;
    value.ptr = 0;
    return value;
}

static inline SValue sjit_value_clone_fast(SValue value) {
    if (value.tag != SJIT_VALUE_STRING && value.tag != SJIT_VALUE_LIST) {
        return value;
    }
    return sjit_value_clone(value);
}

static inline void sjit_value_destroy_fast(SValue value) {
    if (value.tag == SJIT_VALUE_STRING || value.tag == SJIT_VALUE_LIST) {
        sjit_value_destroy(value);
    }
}

#ifdef __cplusplus
}
#endif

#endif
