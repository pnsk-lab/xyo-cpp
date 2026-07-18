#include "sjit_value.h"

#include "sjit_list.h"
#include "sjit_string.h"

SValue sjit_make_number(double number) {
    SValue value;
    value.tag = SJIT_VALUE_NUMBER;
    value.number = number;
    value.ptr = NULL;
    return value;
}

SValue sjit_make_bool(int raw) {
    SValue value;
    value.tag = SJIT_VALUE_BOOL;
    value.number = raw ? 1.0 : 0.0;
    value.ptr = NULL;
    return value;
}

SValue sjit_make_null(void) {
    SValue value;
    value.tag = SJIT_VALUE_NULL;
    value.number = 0.0;
    value.ptr = NULL;
    return value;
}

SValue sjit_make_string_len(const char *bytes, int length) {
    SValue value;
    value.tag = SJIT_VALUE_STRING;
    value.number = 0.0;
    value.ptr = sjit_string_new_len(bytes ? bytes : "", length);
    return value;
}

SValue sjit_make_string(const char *bytes) {
    return sjit_make_string_len(bytes ? bytes : "", -1);
}

SValue sjit_make_list(SList *list) {
    SValue value;
    value.tag = SJIT_VALUE_LIST;
    value.number = 0.0;
    value.ptr = list;
    return value;
}

SValue sjit_value_clone(SValue value) {
    if (value.tag == SJIT_VALUE_STRING) {
        SValue out = sjit_make_null();
        out.tag = SJIT_VALUE_STRING;
        out.ptr = sjit_string_clone((const SString *)value.ptr);
        return out;
    }
    if (value.tag == SJIT_VALUE_LIST) {
        return sjit_make_list(sjit_list_clone((const SList *)value.ptr));
    }
    return value;
}

void sjit_value_destroy(SValue value) {
    if (value.tag == SJIT_VALUE_STRING) {
        sjit_string_destroy((SString *)value.ptr);
    } else if (value.tag == SJIT_VALUE_LIST) {
        sjit_list_destroy((SList *)value.ptr);
    }
}

const char *sjit_value_debug_cstr(SValue value) {
    if (value.tag == SJIT_VALUE_STRING) {
        return sjit_string_cstr((const SString *)value.ptr);
    }
    if (value.tag == SJIT_VALUE_BOOL) {
        return value.number != 0.0 ? "true" : "false";
    }
    if (value.tag == SJIT_VALUE_NULL) {
        return "null";
    }
    if (value.tag == SJIT_VALUE_LIST) {
        return "[list]";
    }
    return "[number]";
}

