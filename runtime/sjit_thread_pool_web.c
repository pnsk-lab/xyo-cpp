#include "sjit_thread_pool.h"

#include <stddef.h>

/* Web builds deliberately keep scheduling on the browser thread.  This
   preserves the scheduler API without requiring SharedArrayBuffer,
   cross-origin isolation, or a worker-backed pthread pool. */
struct SJitThreadPool {
    int parallelism;
};

SJitThreadPool *sjit_thread_pool_create(int parallelism) {
    (void)parallelism;
    return NULL;
}

void sjit_thread_pool_destroy(SJitThreadPool *pool) {
    (void)pool;
}

int sjit_thread_pool_parallelism(const SJitThreadPool *pool) {
    return pool ? pool->parallelism : 1;
}

int sjit_thread_pool_run(
    SJitThreadPool *pool,
    int task_count,
    SJitThreadPoolTaskFn function,
    void *context) {
    (void)pool;
    (void)task_count;
    (void)function;
    (void)context;
    return 0;
}

int sjit_thread_pool_environment_parallelism(void) {
    return 1;
}
