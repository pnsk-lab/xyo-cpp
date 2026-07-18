#ifndef SJIT_STRING_H
#define SJIT_STRING_H

#include "sjit_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SString {
    char *bytes;
    int length;
    int ref_count;
    int number_cache_valid;
    int number_cache_ok;
    int number_cache_whitespace;
    double number_cache;
};

SString *sjit_string_new(const char *bytes);
SString *sjit_string_new_len(const char *bytes, int length);
SString *sjit_string_clone(const SString *string);
void sjit_string_destroy(SString *string);
const char *sjit_string_cstr(const SString *string);
int sjit_string_utf16_length(const SString *string);
SString *sjit_string_utf16_char_at(const SString *string, int zero_based_index);
int sjit_string_equals_ignore_case(const SString *a, const char *b);
int sjit_cstr_equals_ignore_case(const char *a, const char *b);
int sjit_cstr_is_whitespace(const char *value);
char *sjit_cstr_dup_lower(const char *value);

#ifdef __cplusplus
}
#endif

#endif
