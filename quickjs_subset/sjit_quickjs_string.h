#ifndef SJIT_QUICKJS_STRING_H
#define SJIT_QUICKJS_STRING_H

#ifdef __cplusplus
extern "C" {
#endif

int sjit_qjs_parse_number_like(const char *text, double *out_number, int *out_is_whitespace);
int sjit_qjs_number_to_cstr(double value, char *buffer, int buffer_length);

#ifdef __cplusplus
}
#endif

#endif
