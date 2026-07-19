#ifndef SJIT_LIST_H
#define SJIT_LIST_H

#include "sjit_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SJIT_LIST_INDEX_ALL = -1,
    SJIT_LIST_INDEX_INVALID = 0
};

struct SList {
    struct SListStorage *storage;
};

SList *sjit_list_create(void);
SList *sjit_list_clone(const SList *list);
void sjit_list_destroy(SList *list);
int sjit_list_length(const SList *list);
int sjit_list_set_item_limit(SList *list, int item_limit);
int sjit_list_item_limit(const SList *list);
int sjit_list_push(SList *list, SValue value);
int sjit_list_push_move(SList *list, SValue value);
int sjit_list_push_number(SList *list, double number);
int sjit_list_push_repeated(SList *list, SValue value, int count);
int sjit_list_insert(SList *list, int one_based_index, SValue value);
int sjit_list_insert_move(SList *list, int one_based_index, SValue value);
int sjit_list_replace(SList *list, int one_based_index, SValue value);
int sjit_list_replace_move(SList *list, int one_based_index, SValue value);
int sjit_list_replace_number(SList *list, int one_based_index, double number);
int sjit_list_delete(SList *list, int one_based_index);
void sjit_list_clear(SList *list);
SValue sjit_list_get(const SList *list, int one_based_index);
const SValue *sjit_list_get_borrowed(const SList *list, int one_based_index);
SValue sjit_list_get_fast_number(const SList *list, int one_based_index, int *ok);
int sjit_list_get_number(const SList *list, int one_based_index, double *out);
/* Returns true only while every list item is represented as an exact NUMBER. */
int sjit_list_get_cached_number(const SList *list, int one_based_index, double *out);
double sjit_list_get_coerced_number(SRuntime *runtime, const SList *list, int one_based_index);
int sjit_list_contains(SRuntime *runtime, const SList *list, SValue value);
int sjit_list_item_number(SRuntime *runtime, const SList *list, SValue value);
int sjit_list_to_index(SRuntime *runtime, SValue index, int length, int accept_all);
SValue sjit_list_contents(SList *list);

#ifdef __cplusplus
}
#endif

#endif
