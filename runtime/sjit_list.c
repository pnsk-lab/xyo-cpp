#include "sjit_list.h"
#include "sjit_list_internal.h"

#include "sjit_compare.h"
#include "sjit_number.h"
#include "sjit_string.h"
#include "sjit_value.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static SListStorage *storage_create(void) {
    SListStorage *storage = (SListStorage *)calloc(1, sizeof(SListStorage));
    if (storage) {
        storage->ref_count = 1;
        storage->numbers_valid =
            getenv("SJIT_DISABLE_NUMERIC_LIST_CACHE") == NULL;
    }
    return storage;
}

static void invalidate_numbers(SListStorage *storage) {
    if (!storage || !storage->numbers_valid) {
        return;
    }
    free(storage->numbers);
    storage->numbers = NULL;
    storage->numbers_valid = 0;
}

static void storage_release(SListStorage *storage) {
    if (!storage) {
        return;
    }
    --storage->ref_count;
    if (storage->ref_count > 0) {
        return;
    }
    for (int i = 0; i < storage->length; ++i) {
        sjit_value_destroy(storage->items[i]);
    }
    free(storage->items);
    free(storage->numbers);
    free(storage);
}

static int ensure_capacity(SList *list, int wanted) {
    if (!list || !list->storage) {
        return 0;
    }
    if (wanted <= list->storage->capacity) {
        if (list->storage->numbers_valid && !list->storage->numbers &&
            list->storage->capacity > 0) {
            list->storage->numbers = (double *)malloc(
                sizeof(double) * (size_t)list->storage->capacity);
            if (!list->storage->numbers) {
                list->storage->numbers_valid = 0;
            }
        }
        return 1;
    }
    int next = list->storage->capacity > 0 ? list->storage->capacity : SJIT_INITIAL_CAPACITY;
    while (next < wanted) {
        next *= 2;
    }
    SValue *items = (SValue *)realloc(list->storage->items, sizeof(SValue) * (size_t)next);
    if (!items) {
        return 0;
    }
    list->storage->items = items;
    if (list->storage->numbers_valid) {
        double *numbers = (double *)realloc(
            list->storage->numbers,
            sizeof(double) * (size_t)next);
        if (numbers) {
            list->storage->numbers = numbers;
        } else {
            invalidate_numbers(list->storage);
        }
    }
    list->storage->capacity = next;
    return 1;
}

static __attribute__((noinline)) int ensure_unique_slow(SList *list) {
    if (!list || !list->storage) {
        return 0;
    }
    SListStorage *copy = storage_create();
    if (!copy) {
        return 0;
    }
    if (list->storage->length > 0) {
        copy->items = (SValue *)calloc((size_t)list->storage->length, sizeof(SValue));
        if (!copy->items) {
            storage_release(copy);
            return 0;
        }
        copy->capacity = list->storage->length;
        if (list->storage->numbers_valid) {
            copy->numbers = (double *)malloc(
                sizeof(double) * (size_t)list->storage->length);
            if (copy->numbers) {
                memcpy(
                    copy->numbers,
                    list->storage->numbers,
                    sizeof(double) * (size_t)list->storage->length);
            } else {
                copy->numbers_valid = 0;
            }
        } else {
            copy->numbers_valid = 0;
        }
        for (int i = 0; i < list->storage->length; ++i) {
            copy->items[i] = sjit_value_clone(list->storage->items[i]);
        }
    }
    copy->length = list->storage->length;
    storage_release(list->storage);
    list->storage = copy;
    return 1;
}

static inline __attribute__((always_inline)) int ensure_unique(SList *list) {
    if (list && list->storage && list->storage->ref_count == 1) {
        return 1;
    }
    return ensure_unique_slow(list);
}

SList *sjit_list_create(void) {
    SList *list = (SList *)calloc(1, sizeof(SList));
    if (!list) {
        return NULL;
    }
    list->storage = storage_create();
    if (!list->storage) {
        free(list);
        return NULL;
    }
    return list;
}

SList *sjit_list_clone(const SList *list) {
    if (!list || !list->storage) {
        return sjit_list_create();
    }
    SList *copy = (SList *)calloc(1, sizeof(SList));
    if (!copy) {
        return NULL;
    }
    ++list->storage->ref_count;
    copy->storage = list->storage;
    return copy;
}

void sjit_list_destroy(SList *list) {
    if (!list) {
        return;
    }
    storage_release(list->storage);
    free(list);
}

int sjit_list_length(const SList *list) {
    return list && list->storage ? list->storage->length : 0;
}

int sjit_list_push(SList *list, SValue value) {
    return sjit_list_push_move(list, sjit_value_clone(value));
}

int sjit_list_push_move(SList *list, SValue value) {
    if (!ensure_unique(list) || list->storage->length >= SJIT_LIST_ITEM_LIMIT ||
        !ensure_capacity(list, list->storage->length + 1)) {
        sjit_value_destroy(value);
        return 0;
    }
    const int index = list->storage->length++;
    list->storage->items[index] = value;
    if (list->storage->numbers_valid) {
        if (value.tag == SJIT_VALUE_NUMBER) {
            list->storage->numbers[index] = value.number;
        } else {
            invalidate_numbers(list->storage);
        }
    }
    return 1;
}

int sjit_list_push_number(SList *list, double number) {
    if (!ensure_unique(list) || list->storage->length >= SJIT_LIST_ITEM_LIMIT ||
        !ensure_capacity(list, list->storage->length + 1)) {
        return 0;
    }
    SValue *item = &list->storage->items[list->storage->length++];
    item->tag = SJIT_VALUE_NUMBER;
    item->number = number;
    item->ptr = NULL;
    if (list->storage->numbers_valid) {
        list->storage->numbers[list->storage->length - 1] = number;
    }
    return 1;
}

int sjit_list_push_repeated(SList *list, SValue value, int count) {
    if (count <= 0) {
        return 1;
    }
    if (!ensure_unique(list)) {
        return 0;
    }
    const int available = SJIT_LIST_ITEM_LIMIT - list->storage->length;
    const int append_count = count < available ? count : available;
    if (append_count <= 0) {
        return 1;
    }
    if (!ensure_capacity(list, list->storage->length + append_count)) {
        return 0;
    }

    const int start = list->storage->length;
    if (list->storage->numbers_valid) {
        if (value.tag == SJIT_VALUE_NUMBER) {
            for (int i = 0; i < append_count; ++i) {
                list->storage->numbers[start + i] = value.number;
            }
        } else {
            invalidate_numbers(list->storage);
        }
    }
    if (value.tag == SJIT_VALUE_STRING && value.ptr) {
        ((SString *)value.ptr)->ref_count += append_count;
        for (int i = 0; i < append_count; ++i) {
            list->storage->items[start + i] = value;
        }
    } else {
        for (int i = 0; i < append_count; ++i) {
            list->storage->items[start + i] = sjit_value_clone_fast(value);
        }
    }
    list->storage->length += append_count;
    return 1;
}

int sjit_list_insert(SList *list, int one_based_index, SValue value) {
    return sjit_list_insert_move(list, one_based_index, sjit_value_clone(value));
}

int sjit_list_insert_move(SList *list, int one_based_index, SValue value) {
    if (!ensure_unique(list)) {
        sjit_value_destroy(value);
        return 0;
    }
    if (one_based_index < 1) {
        one_based_index = 1;
    }
    if (one_based_index > list->storage->length + 1) {
        one_based_index = list->storage->length + 1;
    }
    if (one_based_index > SJIT_LIST_ITEM_LIMIT) {
        sjit_value_destroy(value);
        return 0;
    }
    if (list->storage->length >= SJIT_LIST_ITEM_LIMIT) {
        const int zero = one_based_index - 1;
        sjit_value_destroy(list->storage->items[list->storage->length - 1]);
        if (zero < list->storage->length - 1) {
            memmove(
                &list->storage->items[zero + 1],
                &list->storage->items[zero],
                sizeof(SValue) * (size_t)(list->storage->length - zero - 1));
        }
        list->storage->items[zero] = value;
        if (list->storage->numbers_valid) {
            if (value.tag == SJIT_VALUE_NUMBER) {
                if (zero < list->storage->length - 1) {
                    memmove(
                        &list->storage->numbers[zero + 1],
                        &list->storage->numbers[zero],
                        sizeof(double) * (size_t)(list->storage->length - zero - 1));
                }
                list->storage->numbers[zero] = value.number;
            } else {
                invalidate_numbers(list->storage);
            }
        }
        return 1;
    }
    if (!ensure_capacity(list, list->storage->length + 1)) {
        sjit_value_destroy(value);
        return 0;
    }
    const int zero = one_based_index - 1;
    if (zero < list->storage->length) {
        memmove(
            &list->storage->items[zero + 1],
            &list->storage->items[zero],
            sizeof(SValue) * (size_t)(list->storage->length - zero));
    }
    list->storage->items[zero] = value;
    if (list->storage->numbers_valid) {
        if (value.tag == SJIT_VALUE_NUMBER) {
            if (zero < list->storage->length) {
                memmove(
                    &list->storage->numbers[zero + 1],
                    &list->storage->numbers[zero],
                    sizeof(double) * (size_t)(list->storage->length - zero));
            }
            list->storage->numbers[zero] = value.number;
        } else {
            invalidate_numbers(list->storage);
        }
    }
    ++list->storage->length;
    return 1;
}

int sjit_list_replace(SList *list, int one_based_index, SValue value) {
    return sjit_list_replace_move(list, one_based_index, sjit_value_clone(value));
}

int sjit_list_replace_move(SList *list, int one_based_index, SValue value) {
    if (!ensure_unique(list) || one_based_index < 1 || one_based_index > list->storage->length) {
        sjit_value_destroy(value);
        return 0;
    }
    const int zero = one_based_index - 1;
    sjit_value_destroy_fast(list->storage->items[zero]);
    list->storage->items[zero] = value;
    if (list->storage->numbers_valid) {
        if (value.tag == SJIT_VALUE_NUMBER) {
            list->storage->numbers[zero] = value.number;
        } else {
            invalidate_numbers(list->storage);
        }
    }
    return 1;
}

int sjit_list_replace_number(SList *list, int one_based_index, double number) {
    if (!ensure_unique(list) || one_based_index < 1 || one_based_index > list->storage->length) {
        return 0;
    }
    SValue *item = &list->storage->items[one_based_index - 1];
    sjit_value_destroy_fast(*item);
    item->tag = SJIT_VALUE_NUMBER;
    item->number = number;
    item->ptr = NULL;
    if (list->storage->numbers_valid) {
        list->storage->numbers[one_based_index - 1] = number;
    }
    return 1;
}

int sjit_list_delete(SList *list, int one_based_index) {
    if (!ensure_unique(list) || one_based_index < 1 || one_based_index > list->storage->length) {
        return 0;
    }
    const int zero = one_based_index - 1;
    sjit_value_destroy(list->storage->items[zero]);
    for (int i = zero; i < list->storage->length - 1; ++i) {
        list->storage->items[i] = list->storage->items[i + 1];
    }
    if (list->storage->numbers_valid && zero < list->storage->length - 1) {
        memmove(
            &list->storage->numbers[zero],
            &list->storage->numbers[zero + 1],
            sizeof(double) * (size_t)(list->storage->length - zero - 1));
    }
    --list->storage->length;
    return 1;
}

void sjit_list_clear(SList *list) {
    if (!ensure_unique(list)) {
        return;
    }
    /* An exact numeric cache also proves that every SValue is an owning-free
       NUMBER. Large render buffers can therefore be reset in constant time. */
    if (!list->storage->numbers_valid) {
        for (int i = 0; i < list->storage->length; ++i) {
            sjit_value_destroy(list->storage->items[i]);
        }
    }
    list->storage->length = 0;
    if (!list->storage->numbers_valid) {
        list->storage->numbers_valid =
            getenv("SJIT_DISABLE_NUMERIC_LIST_CACHE") == NULL;
    }
}

SValue sjit_list_get(const SList *list, int one_based_index) {
    if (!list || !list->storage || one_based_index < 1 || one_based_index > list->storage->length) {
        return sjit_make_null();
    }
    return sjit_value_clone(list->storage->items[one_based_index - 1]);
}

const SValue *sjit_list_get_borrowed(const SList *list, int one_based_index) {
    if (!list || !list->storage || one_based_index < 1 || one_based_index > list->storage->length) {
        return NULL;
    }
    return &list->storage->items[one_based_index - 1];
}

SValue sjit_list_get_fast_number(const SList *list, int one_based_index, int *ok) {
    if (ok) {
        *ok = 0;
    }
    if (!list || !list->storage || one_based_index < 1 || one_based_index > list->storage->length) {
        return sjit_make_null();
    }
    SValue item = list->storage->items[one_based_index - 1];
    if (item.tag != SJIT_VALUE_NUMBER && item.tag != SJIT_VALUE_BOOL) {
        return sjit_make_null();
    }
    if (ok) {
        *ok = 1;
    }
    return item;
}

int sjit_list_get_number(const SList *list, int one_based_index, double *out) {
    if (!out || !list || !list->storage || one_based_index < 1 || one_based_index > list->storage->length) {
        return 0;
    }
    if (list->storage->numbers_valid && list->storage->numbers) {
        const double number = list->storage->numbers[one_based_index - 1];
        *out = number != number ? 0.0 : number;
        return 1;
    }
    SValue item = list->storage->items[one_based_index - 1];
    if (item.tag == SJIT_VALUE_NUMBER) {
        *out = item.number != item.number ? 0.0 : item.number;
        return 1;
    }
    if (item.tag == SJIT_VALUE_BOOL) {
        *out = item.number != 0.0 ? 1.0 : 0.0;
        return 1;
    }
    if (item.tag == SJIT_VALUE_STRING) {
        double number = 0.0;
        int whitespace = 0;
        if (sjit_parse_number_for_compare_fast(item, &number, &whitespace) && !whitespace && number == number) {
            *out = number;
            return 1;
        }
    }
    return 0;
}

int sjit_list_get_cached_number(const SList *list, int one_based_index, double *out) {
    if (!out || !list || !list->storage || !list->storage->numbers_valid ||
        !list->storage->numbers || one_based_index < 1 ||
        one_based_index > list->storage->length) {
        return 0;
    }
    *out = list->storage->numbers[one_based_index - 1];
    return 1;
}

double sjit_list_get_coerced_number(SRuntime *runtime, const SList *list, int one_based_index) {
    if (!list || !list->storage || one_based_index < 1 || one_based_index > list->storage->length) {
        return 0.0;
    }
    if (list->storage->numbers_valid && list->storage->numbers) {
        const double number = list->storage->numbers[one_based_index - 1];
        return number != number ? 0.0 : number;
    }
    return sjit_to_number_fast(runtime, list->storage->items[one_based_index - 1]);
}

int sjit_list_contains(SRuntime *runtime, const SList *list, SValue value) {
    return sjit_list_item_number(runtime, list, value) > 0;
}

int sjit_list_item_number(SRuntime *runtime, const SList *list, SValue value) {
    if (!list) {
        return 0;
    }
    if (!list || !list->storage) {
        return 0;
    }
    for (int i = 0; i < list->storage->length; ++i) {
        if (sjit_compare(runtime, list->storage->items[i], value) == 0) {
            return i + 1;
        }
    }
    return 0;
}

int sjit_list_to_index(SRuntime *runtime, SValue index, int length, int accept_all) {
    if (index.tag == SJIT_VALUE_STRING) {
        const char *text = sjit_string_cstr((const SString *)index.ptr);
        if (strcmp(text, "all") == 0) {
            return accept_all ? SJIT_LIST_INDEX_ALL : SJIT_LIST_INDEX_INVALID;
        }
        if (strcmp(text, "last") == 0) {
            return length > 0 ? length : SJIT_LIST_INDEX_INVALID;
        }
        if (strcmp(text, "random") == 0 || strcmp(text, "any") == 0) {
            return length > 0 ? 1 + (rand() % length) : SJIT_LIST_INDEX_INVALID;
        }
    }
    const int converted = (int)floor(sjit_to_number_fast(runtime, index));
    if (converted < 1 || converted > length) {
        return SJIT_LIST_INDEX_INVALID;
    }
    return converted;
}

SValue sjit_list_contents(SList *list) {
    if (!list || !list->storage || list->storage->length == 0) {
        return sjit_make_string("");
    }

    int compact = 1;
    size_t total = 0;
    for (int i = 0; i < list->storage->length; ++i) {
        const SValue item = list->storage->items[i];
        SValue text = sjit_to_string(NULL, item);
        const SString *string = (const SString *)text.ptr;
        if (item.tag != SJIT_VALUE_STRING || sjit_string_utf16_length(string) != 1) {
            compact = 0;
        }
        total += (size_t)string->length;
        sjit_value_destroy(text);
    }
    if (!compact) {
        total += (size_t)(list->storage->length - 1);
    }

    char *buffer = (char *)calloc(total + 1u, 1u);
    if (!buffer) {
        return sjit_make_string("");
    }
    size_t offset = 0;
    for (int i = 0; i < list->storage->length; ++i) {
        SValue text = sjit_to_string(NULL, list->storage->items[i]);
        const SString *string = (const SString *)text.ptr;
        if (!compact && i > 0) {
            buffer[offset++] = ' ';
        }
        memcpy(buffer + offset, string->bytes, (size_t)string->length);
        offset += (size_t)string->length;
        sjit_value_destroy(text);
    }
    SValue out = sjit_make_string(buffer);
    free(buffer);
    return out;
}
