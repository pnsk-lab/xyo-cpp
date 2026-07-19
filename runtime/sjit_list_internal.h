#ifndef SJIT_LIST_INTERNAL_H
#define SJIT_LIST_INTERNAL_H

#include "sjit_list.h"

/* Private storage shared only by list.c and tightly guarded runtime helpers.
   Keeping the layout in one header prevents native fast paths from silently
   drifting out of sync with the owning list implementation. */
typedef struct SListStorage {
    SValue *items;
    int length;
    int capacity;
    int ref_count;
    double *numbers;
    int numbers_valid;
    /* Runtime compatibility setting.  This field is appended after the
       JIT-visible prefix used by older list fast paths. */
    int item_limit;
} SListStorage;

#endif
