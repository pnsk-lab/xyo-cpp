#include "sjit_quickjs_string.h"

#include <stdint.h>
#include <stddef.h>

#include "dtoa.h"

#include <ctype.h>
#include <math.h>

static const char *sjit_qjs_skip_ws(const char *value) {
    while (value && *value && isspace((unsigned char)*value)) {
        ++value;
    }
    return value ? value : "";
}

static int sjit_qjs_is_whitespace(const char *value) {
    const char *p = value ? value : "";
    while (*p) {
        if (!isspace((unsigned char)*p)) {
            return 0;
        }
        ++p;
    }
    return 1;
}

int sjit_qjs_parse_number_like(const char *text, double *out_number, int *out_is_whitespace) {
    if (out_is_whitespace) {
        *out_is_whitespace = sjit_qjs_is_whitespace(text);
    }

    const char *start = sjit_qjs_skip_ws(text);
    if (*start == '\0') {
        if (out_number) {
            *out_number = 0.0;
        }
        return 1;
    }

    JSATODTempMem atod_mem;
    const char *next = start;
    const double number = js_atod(start, &next, 0, JS_ATOD_ACCEPT_BIN_OCT, &atod_mem);
    if (next == start || isnan(number)) {
        if (out_number) {
            *out_number = NAN;
        }
        return 0;
    }

    next = sjit_qjs_skip_ws(next);
    if (*next != '\0') {
        if (out_number) {
            *out_number = NAN;
        }
        return 0;
    }

    if (out_number) {
        *out_number = number;
    }
    return 1;
}

int sjit_qjs_number_to_cstr(double value, char *buffer, int buffer_length) {
    if (!buffer || buffer_length <= 0) {
        return 0;
    }
    JSDTOATempMem dtoa_mem;
    const int length = js_dtoa(
        buffer,
        value,
        10,
        0,
        JS_DTOA_FORMAT_FREE | JS_DTOA_MINUS_ZERO,
        &dtoa_mem);
    if (length < 0 || length >= buffer_length) {
        buffer[0] = '\0';
        return 0;
    }
    buffer[length] = '\0';
    return length;
}
