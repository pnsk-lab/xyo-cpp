#include "sjit_operator.h"

#include "sjit_compare.h"
#include "sjit_number.h"
#include "sjit_string.h"
#include "sjit_value.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static double operator_to_number(SRuntime *runtime, SValue value) {
    if (value.tag == SJIT_VALUE_NUMBER) {
        return isnan(value.number) ? 0.0 : value.number;
    }
    if (value.tag == SJIT_VALUE_BOOL) {
        return value.number != 0.0 ? 1.0 : 0.0;
    }
    if (value.tag == SJIT_VALUE_NULL) {
        return 0.0;
    }
    return sjit_to_number(runtime, value);
}

SValue sjit_op_add(SRuntime *runtime, SValue a, SValue b) {
    return sjit_make_number(operator_to_number(runtime, a) + operator_to_number(runtime, b));
}

SValue sjit_op_sub(SRuntime *runtime, SValue a, SValue b) {
    return sjit_make_number(operator_to_number(runtime, a) - operator_to_number(runtime, b));
}

SValue sjit_op_mul(SRuntime *runtime, SValue a, SValue b) {
    return sjit_make_number(operator_to_number(runtime, a) * operator_to_number(runtime, b));
}

SValue sjit_op_div(SRuntime *runtime, SValue a, SValue b) {
    return sjit_make_number(operator_to_number(runtime, a) / operator_to_number(runtime, b));
}

SValue sjit_op_join(SRuntime *runtime, SValue a, SValue b) {
    SValue sa = sjit_to_string(runtime, a);
    SValue sb = sjit_to_string(runtime, b);
    const char *astr = sjit_string_cstr((const SString *)sa.ptr);
    const char *bstr = sjit_string_cstr((const SString *)sb.ptr);
    const size_t len_a = strlen(astr);
    const size_t len_b = strlen(bstr);
    char *joined = (char *)calloc(len_a + len_b + 1u, 1u);
    if (!joined) {
        sjit_value_destroy(sa);
        sjit_value_destroy(sb);
        return sjit_make_string("");
    }
    memcpy(joined, astr, len_a);
    memcpy(joined + len_a, bstr, len_b);
    SValue out = sjit_make_string(joined);
    free(joined);
    sjit_value_destroy(sa);
    sjit_value_destroy(sb);
    return out;
}

int sjit_op_contains(SRuntime *runtime, SValue text, SValue substring) {
    SValue text_string = sjit_to_string(runtime, text);
    SValue substring_string = sjit_to_string(runtime, substring);
    char *text_lower = sjit_cstr_dup_lower(
        sjit_string_cstr((const SString *)text_string.ptr));
    char *substring_lower = sjit_cstr_dup_lower(
        sjit_string_cstr((const SString *)substring_string.ptr));
    const int contains = text_lower && substring_lower && strstr(text_lower, substring_lower) != NULL;
    free(text_lower);
    free(substring_lower);
    sjit_value_destroy(text_string);
    sjit_value_destroy(substring_string);
    return contains;
}

int sjit_op_contains_value_ptr(
    SRuntime *runtime,
    const SValue *text,
    const SValue *substring) {
    return sjit_op_contains(
        runtime,
        text ? *text : sjit_make_null(),
        substring ? *substring : sjit_make_null());
}

SValue sjit_op_mod(SRuntime *runtime, SValue a, SValue b) {
    const double n = operator_to_number(runtime, a);
    const double modulus = operator_to_number(runtime, b);
    double result = fmod(n, modulus);
    if (result / modulus < 0.0) {
        result += modulus;
    }
    return sjit_make_number(result);
}

double sjit_op_round_number(double number) {
    const double below = floor(number);
    double result = number - below >= 0.5 ? below + 1.0 : below;
    if (result == 0.0 && signbit(number)) {
        result = -0.0;
    }
    return result;
}

SValue sjit_op_round(SRuntime *runtime, SValue x) {
    return sjit_make_number(sjit_op_round_number(operator_to_number(runtime, x)));
}

static double round_10(double value) {
    return round(value * 10000000000.0) / 10000000000.0;
}

static double deg_to_rad(double degrees) {
    return degrees * 3.14159265358979323846 / 180.0;
}

static double rad_to_deg(double radians) {
    return radians * 180.0 / 3.14159265358979323846;
}

int sjit_op_mathop_id(const char *operator_name) {
    if (!operator_name) {
        return 0;
    }
    if (sjit_cstr_equals_ignore_case(operator_name, "abs")) {
        return 1;
    }
    if (sjit_cstr_equals_ignore_case(operator_name, "floor")) {
        return 2;
    }
    if (sjit_cstr_equals_ignore_case(operator_name, "ceiling")) {
        return 3;
    }
    if (sjit_cstr_equals_ignore_case(operator_name, "sqrt")) {
        return 4;
    }
    if (sjit_cstr_equals_ignore_case(operator_name, "sin")) {
        return 5;
    }
    if (sjit_cstr_equals_ignore_case(operator_name, "cos")) {
        return 6;
    }
    if (sjit_cstr_equals_ignore_case(operator_name, "tan")) {
        return 7;
    }
    if (sjit_cstr_equals_ignore_case(operator_name, "asin")) {
        return 8;
    }
    if (sjit_cstr_equals_ignore_case(operator_name, "acos")) {
        return 9;
    }
    if (sjit_cstr_equals_ignore_case(operator_name, "atan")) {
        return 10;
    }
    if (sjit_cstr_equals_ignore_case(operator_name, "ln")) {
        return 11;
    }
    if (sjit_cstr_equals_ignore_case(operator_name, "log")) {
        return 12;
    }
    if (sjit_cstr_equals_ignore_case(operator_name, "e ^")) {
        return 13;
    }
    if (sjit_cstr_equals_ignore_case(operator_name, "10 ^")) {
        return 14;
    }
    return 0;
}

double sjit_op_mathop_number_id(int operator_id, double n) {
    switch (operator_id) {
    case 1:
        return fabs(n);
    case 2:
        return floor(n);
    case 3:
        return ceil(n);
    case 4:
        return sqrt(n);
    case 5:
        return round_10(sin(deg_to_rad(n)));
    case 6:
        return round_10(cos(deg_to_rad(n)));
    case 7: {
        const double angle = fmod(n, 360.0);
        if (angle == 90.0 || angle == -270.0) {
            return INFINITY;
        }
        if (angle == -90.0 || angle == 270.0) {
            return -INFINITY;
        }
        return round_10(tan(deg_to_rad(angle)));
    }
    case 8:
        return rad_to_deg(asin(n));
    case 9:
        return rad_to_deg(acos(n));
    case 10:
        return rad_to_deg(atan(n));
    case 11:
        return log(n);
    case 12:
        return log(n) / log(10.0);
    case 13:
        return exp(n);
    case 14:
        return pow(10.0, n);
    default:
        return 0.0;
    }
}

SValue sjit_op_mathop(SRuntime *runtime, const char *operator_name, SValue x) {
    return sjit_make_number(
        sjit_op_mathop_number_id(sjit_op_mathop_id(operator_name), operator_to_number(runtime, x)));
}

int sjit_op_lt(SRuntime *runtime, SValue a, SValue b) {
    return sjit_lt(runtime, a, b);
}

int sjit_op_eq(SRuntime *runtime, SValue a, SValue b) {
    return sjit_eq(runtime, a, b);
}

int sjit_op_gt(SRuntime *runtime, SValue a, SValue b) {
    return sjit_gt(runtime, a, b);
}

int sjit_op_and(SRuntime *runtime, SValue a, SValue b) {
    return sjit_to_bool(runtime, a) && sjit_to_bool(runtime, b);
}

int sjit_op_or(SRuntime *runtime, SValue a, SValue b) {
    return sjit_to_bool(runtime, a) || sjit_to_bool(runtime, b);
}

int sjit_op_not(SRuntime *runtime, SValue x) {
    return !sjit_to_bool(runtime, x);
}
