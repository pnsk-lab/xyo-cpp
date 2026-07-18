#include "sjit_compare.h"

#include "sjit_number.h"
#include "sjit_string.h"
#include "sjit_value.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int compare_strings_lower(const char *a, const char *b) {
    const unsigned char *pa = (const unsigned char *)(a ? a : "");
    const unsigned char *pb = (const unsigned char *)(b ? b : "");
    while (*pa && *pb) {
        const int ca = tolower(*pa);
        const int cb = tolower(*pb);
        if (ca < cb) {
            return -1;
        }
        if (ca > cb) {
            return 1;
        }
        ++pa;
        ++pb;
    }
    if (*pa) {
        return 1;
    }
    if (*pb) {
        return -1;
    }
    return 0;
}

static int compare_number(SValue value, double *out) {
    double number = NAN;
    int whitespace = 0;
    sjit_parse_number_for_compare_fast(value, &number, &whitespace);
    if (number == 0.0 && whitespace) {
        number = NAN;
    }
    *out = number;
    return !isnan(number);
}

static int compare_as_text(SRuntime *runtime, SValue a, SValue b) {
    SValue sa = sjit_make_null_fast();
    SValue sb = sjit_make_null_fast();
    const char *a_text = NULL;
    const char *b_text = NULL;
    if (a.tag == SJIT_VALUE_STRING) {
        a_text = sjit_string_cstr((const SString *)a.ptr);
    } else {
        sa = sjit_to_string(runtime, a);
        a_text = sjit_string_cstr((const SString *)sa.ptr);
    }
    if (b.tag == SJIT_VALUE_STRING) {
        b_text = sjit_string_cstr((const SString *)b.ptr);
    } else {
        sb = sjit_to_string(runtime, b);
        b_text = sjit_string_cstr((const SString *)sb.ptr);
    }
    const int result = compare_strings_lower(a_text, b_text);
    sjit_value_destroy_fast(sa);
    sjit_value_destroy_fast(sb);
    return result;
}

int sjit_compare(SRuntime *runtime, SValue a, SValue b) {
    double na = NAN;
    double nb = NAN;
    if (!compare_number(a, &na) || !compare_number(b, &nb)) {
        return compare_as_text(runtime, a, b);
    }
    if (na < nb) {
        return -1;
    }
    if (na > nb) {
        return 1;
    }
    return 0;
}

int sjit_eq(SRuntime *runtime, SValue a, SValue b) {
    double na = NAN;
    double nb = NAN;
    const int a_is_number = compare_number(a, &na);
    const int b_is_number = compare_number(b, &nb);
    if (a_is_number && b_is_number) {
        return na == nb;
    }

    // A finite number's Scratch string representation is itself always a
    // valid number. It therefore cannot equal a value that failed numeric
    // parsing. Keep NaN and infinities on the text path: their spellings can
    // legitimately compare equal to strings such as "NaN" or "Infinity".
    if ((a.tag == SJIT_VALUE_NUMBER && isfinite(a.number) && !b_is_number) ||
        (b.tag == SJIT_VALUE_NUMBER && isfinite(b.number) && !a_is_number)) {
        return 0;
    }
    return compare_as_text(runtime, a, b) == 0;
}

int sjit_lt(SRuntime *runtime, SValue a, SValue b) {
    return sjit_compare(runtime, a, b) < 0;
}

int sjit_gt(SRuntime *runtime, SValue a, SValue b) {
    return sjit_compare(runtime, a, b) > 0;
}
