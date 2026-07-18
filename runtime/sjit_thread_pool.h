#ifndef SJIT_THREAD_POOL_H
#define SJIT_THREAD_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SJitThreadPool SJitThreadPool;
typedef void (*SJitThreadPoolTaskFn)(void *context, int task_index);

/* parallelism includes the scheduler thread.  A value below two has no
   background worker and is intentionally represented by a NULL pool in the
   scheduler. */
SJitThreadPool *sjit_thread_pool_create(int parallelism);
void sjit_thread_pool_destroy(SJitThreadPool *pool);
int sjit_thread_pool_parallelism(const SJitThreadPool *pool);
int sjit_thread_pool_run(
    SJitThreadPool *pool,
    int task_count,
    SJitThreadPoolTaskFn function,
    void *context);
int sjit_thread_pool_environment_parallelism(void);

#ifdef __cplusplus
}
#endif

#endif
