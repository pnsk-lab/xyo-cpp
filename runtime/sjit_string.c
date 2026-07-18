#include "sjit_string.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

SString *sjit_string_new_len(const char *bytes, int length) {
    if (length < 0) {
        length = bytes ? (int)strlen(bytes) : 0;
    }
    SString *string = (SString *)calloc(1, sizeof(SString));
    if (!string) {
        return NULL;
    }
    string->bytes = (char *)calloc((size_t)length + 1u, 1u);
    if (!string->bytes) {
        free(string);
        return NULL;
    }
    if (bytes && length > 0) {
        memcpy(string->bytes, bytes, (size_t)length);
    }
    string->bytes[length] = '\0';
    string->length = length;
    string->ref_count = 1;
    return string;
}

SString *sjit_string_new(const char *bytes) {
    return sjit_string_new_len(bytes ? bytes : "", -1);
}

SString *sjit_string_clone(const SString *string) {
    if (!string) {
        return sjit_string_new("");
    }
    SString *mutable_string = (SString *)string;
    ++mutable_string->ref_count;
    return mutable_string;
}

void sjit_string_destroy(SString *string) {
    if (!string) {
        return;
    }
    --string->ref_count;
    if (string->ref_count > 0) {
        return;
    }
    free(string->bytes);
    free(string);
}

const char *sjit_string_cstr(const SString *string) {
    return string && string->bytes ? string->bytes : "";
}

static int utf8_decode(const unsigned char *bytes, int remaining, unsigned int *code_point) {
    if (remaining >= 2 && bytes[0] >= 0xc2 && bytes[0] <= 0xdf &&
        (bytes[1] & 0xc0) == 0x80) {
        *code_point = ((unsigned int)(bytes[0] & 0x1f) << 6) | (unsigned int)(bytes[1] & 0x3f);
        return 2;
    }
    if (remaining >= 3 && bytes[0] >= 0xe0 && bytes[0] <= 0xef &&
        (bytes[1] & 0xc0) == 0x80 && (bytes[2] & 0xc0) == 0x80) {
        const unsigned int value = ((unsigned int)(bytes[0] & 0x0f) << 12) |
            ((unsigned int)(bytes[1] & 0x3f) << 6) | (unsigned int)(bytes[2] & 0x3f);
        if (value >= 0x800 && !(value >= 0xd800 && value <= 0xdfff)) {
            *code_point = value;
            return 3;
        }
    }
    if (remaining >= 4 && bytes[0] >= 0xf0 && bytes[0] <= 0xf4 &&
        (bytes[1] & 0xc0) == 0x80 && (bytes[2] & 0xc0) == 0x80 &&
        (bytes[3] & 0xc0) == 0x80) {
        const unsigned int value = ((unsigned int)(bytes[0] & 0x07) << 18) |
            ((unsigned int)(bytes[1] & 0x3f) << 12) |
            ((unsigned int)(bytes[2] & 0x3f) << 6) | (unsigned int)(bytes[3] & 0x3f);
        if (value >= 0x10000 && value <= 0x10ffff) {
            *code_point = value;
            return 4;
        }
    }
    *code_point = bytes[0];
    return 1;
}

int sjit_string_utf16_length(const SString *string) {
    if (!string || !string->bytes) {
        return 0;
    }
    int units = 0;
    for (int offset = 0; offset < string->length;) {
        unsigned int code_point = 0;
        offset += utf8_decode(
            (const unsigned char *)string->bytes + offset,
            string->length - offset,
            &code_point);
        units += code_point > 0xffff ? 2 : 1;
    }
    return units;
}

SString *sjit_string_utf16_char_at(const SString *string, int zero_based_index) {
    if (!string || !string->bytes || zero_based_index < 0) {
        return sjit_string_new("");
    }
    int unit = 0;
    for (int offset = 0; offset < string->length;) {
        unsigned int code_point = 0;
        const int width = utf8_decode(
            (const unsigned char *)string->bytes + offset,
            string->length - offset,
            &code_point);
        const int code_units = code_point > 0xffff ? 2 : 1;
        if (zero_based_index >= unit && zero_based_index < unit + code_units) {
            if (code_units == 1) {
                return sjit_string_new_len(string->bytes + offset, width);
            }
            unsigned int surrogate = zero_based_index == unit ?
                0xd800u + ((code_point - 0x10000u) >> 10) :
                0xdc00u + ((code_point - 0x10000u) & 0x3ffu);
            char encoded[3];
            encoded[0] = (char)(0xe0u | (surrogate >> 12));
            encoded[1] = (char)(0x80u | ((surrogate >> 6) & 0x3fu));
            encoded[2] = (char)(0x80u | (surrogate & 0x3fu));
            return sjit_string_new_len(encoded, 3);
        }
        unit += code_units;
        offset += width;
    }
    return sjit_string_new("");
}

int sjit_cstr_equals_ignore_case(const char *a, const char *b) {
    if (!a) {
        a = "";
    }
    if (!b) {
        b = "";
    }
    while (*a && *b) {
        const int ca = tolower((unsigned char)*a);
        const int cb = tolower((unsigned char)*b);
        if (ca != cb) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

int sjit_string_equals_ignore_case(const SString *a, const char *b) {
    return sjit_cstr_equals_ignore_case(sjit_string_cstr(a), b);
}

int sjit_cstr_is_whitespace(const char *value) {
    if (!value) {
        return 1;
    }
    while (*value) {
        if (!isspace((unsigned char)*value)) {
            return 0;
        }
        ++value;
    }
    return 1;
}

char *sjit_cstr_dup_lower(const char *value) {
    if (!value) {
        value = "";
    }
    const size_t length = strlen(value);
    char *out = (char *)calloc(length + 1u, 1u);
    if (!out) {
        return NULL;
    }
    for (size_t i = 0; i < length; ++i) {
        out[i] = (char)tolower((unsigned char)value[i]);
    }
    out[length] = '\0';
    return out;
}
