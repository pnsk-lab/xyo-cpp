#ifndef SJIT_THREAD_H
#define SJIT_THREAD_H

#include "sjit_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

struct SThread {
    int id;
    int target_id;
    int script_id;
    int status;
    int is_killed;
    SScriptEntryFn entry;
    void *script_data;
    SFrame frame;
    /* A restart requested by a hat while this thread is on the C/JIT stack
       must not reset its live frame in place.  The scheduler commits it after
       the entry returns, before interpreting the entry status. */
    int is_executing;
    int restart_pending;
};

SThread *sjit_thread_create(int id, int target_id, int script_id, SScriptEntryFn entry, void *script_data);
void sjit_thread_destroy(SThread *thread);
void sjit_thread_restart(SThread *thread);
int sjit_thread_is_alive(const SThread *thread);

#ifdef __cplusplus
}
#endif

#endif
