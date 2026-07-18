#include "sjit_number.h"

#include "sjit_list.h"
#include "sjit_quickjs_string.h"
#include "sjit_string.h"
#include "sjit_value.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

int sjit_parse_number_for_compare(SValue value, double *out_number, int *out_is_whitespace) {
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
        if (out_is_whitespace) {
            *out_is_whitespace = 1;
        }
        if (out_number) {
            *out_number = 0.0;
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
        if (!string->number_cache_valid) {
            double parsed = 0.0;
            int whitespace = 0;
            string->number_cache_ok = sjit_qjs_parse_number_like(
                sjit_string_cstr(string),
                &parsed,
                &whitespace);
            string->number_cache = parsed;
            string->number_cache_whitespace = whitespace;
            string->number_cache_valid = 1;
        }
        if (out_number) {
            *out_number = string->number_cache;
        }
        if (out_is_whitespace) {
            *out_is_whitespace = string->number_cache_whitespace;
        }
        return string->number_cache_ok;
    }
    default: {
        SValue text = sjit_to_string(NULL, value);
        const int ok = sjit_qjs_parse_number_like(
            sjit_string_cstr((const SString *)text.ptr),
            out_number,
            out_is_whitespace);
        sjit_value_destroy(text);
        return ok;
    }
    }
}

int sjit_value_try_number_for_set(SValue value, double *out_number) {
    if (!out_number) {
        return 0;
    }
    if (value.tag == SJIT_VALUE_NUMBER) {
        *out_number = isnan(value.number) ? 0.0 : value.number;
        return 1;
    }
    if (value.tag == SJIT_VALUE_BOOL) {
        *out_number = value.number != 0.0 ? 1.0 : 0.0;
        return 1;
    }
    if (value.tag == SJIT_VALUE_NULL) {
        *out_number = 0.0;
        return 1;
    }
    if (value.tag == SJIT_VALUE_STRING) {
        double number = 0.0;
        int whitespace = 0;
        if (sjit_parse_number_for_compare_fast(value, &number, &whitespace) &&
            !whitespace && !isnan(number)) {
            *out_number = number;
            return 1;
        }
    }
    return 0;
}

double sjit_to_number(SRuntime *runtime, SValue value) {
    (void)runtime;
    if (value.tag == SJIT_VALUE_NUMBER) {
        return isnan(value.number) ? 0.0 : value.number;
    }
    if (value.tag == SJIT_VALUE_BOOL) {
        return value.number != 0.0 ? 1.0 : 0.0;
    }
    if (value.tag == SJIT_VALUE_NULL) {
        return 0.0;
    }
    double number = 0.0;
    int whitespace = 0;
    const int ok = sjit_parse_number_for_compare_fast(value, &number, &whitespace);
    (void)whitespace;
    if (!ok || isnan(number)) {
        return 0.0;
    }
    return number;
}

int sjit_to_bool(SRuntime *runtime, SValue value) {
    (void)runtime;
    if (value.tag == SJIT_VALUE_BOOL) {
        return value.number != 0.0;
    }
    if (value.tag == SJIT_VALUE_STRING) {
        const char *text = sjit_string_cstr((const SString *)value.ptr);
        if (text[0] == '\0' || strcmp(text, "0") == 0 || sjit_cstr_equals_ignore_case(text, "false")) {
            return 0;
        }
        return 1;
    }
    if (value.tag == SJIT_VALUE_NUMBER) {
        return value.number != 0.0 && !isnan(value.number);
    }
    if (value.tag == SJIT_VALUE_NULL) {
        return 0;
    }
    return 1;
}

SValue sjit_to_string(SRuntime *runtime, SValue value) {
    (void)runtime;
    if (value.tag == SJIT_VALUE_STRING) {
        return sjit_value_clone(value);
    }
    if (value.tag == SJIT_VALUE_BOOL) {
        return sjit_make_string(value.number != 0.0 ? "true" : "false");
    }
    if (value.tag == SJIT_VALUE_NULL) {
        return sjit_make_string("null");
    }
    if (value.tag == SJIT_VALUE_LIST) {
        return sjit_list_contents((SList *)value.ptr);
    }

    char buffer[64];
    const int length = sjit_qjs_number_to_cstr(value.number, buffer, (int)sizeof(buffer));
    if (length > 0) {
        return sjit_make_string_len(buffer, length);
    }
    snprintf(buffer, sizeof(buffer), "%.15g", value.number);
    return sjit_make_string(buffer);
}
