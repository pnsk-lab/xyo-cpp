#include "sjit_thread_pool.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

enum {
    SJIT_THREAD_POOL_MAX_PARALLELISM = 32
};

struct SJitThreadPool {
    pthread_t *workers;
    int worker_count;
    int parallelism;
    pthread_mutex_t mutex;
    pthread_cond_t work_ready;
    pthread_cond_t batch_complete;
    SJitThreadPoolTaskFn function;
    void *context;
    int task_count;
    int next_task;
    int remaining_tasks;
    uint64_t generation;
    int shutdown;
};

static int take_task(
    SJitThreadPool *pool,
    uint64_t generation,
    int *task_index) {
    if (!pool || !task_index) {
        return 0;
    }
    pthread_mutex_lock(&pool->mutex);
    if (pool->shutdown || pool->generation != generation ||
        pool->next_task >= pool->task_count) {
        pthread_mutex_unlock(&pool->mutex);
        return 0;
    }
    *task_index = pool->next_task++;
    pthread_mutex_unlock(&pool->mutex);
    return 1;
}

static void finish_task(SJitThreadPool *pool) {
    pthread_mutex_lock(&pool->mutex);
    if (pool->remaining_tasks > 0) {
        --pool->remaining_tasks;
        if (pool->remaining_tasks == 0) {
            pthread_cond_broadcast(&pool->batch_complete);
        }
    }
    pthread_mutex_unlock(&pool->mutex);
}

static void execute_available_tasks(
    SJitThreadPool *pool,
    uint64_t generation) {
    for (;;) {
        int task_index = -1;
        if (!take_task(pool, generation, &task_index)) {
            return;
        }
        pool->function(pool->context, task_index);
        finish_task(pool);
    }
}

static void *worker_main(void *opaque) {
    SJitThreadPool *pool = (SJitThreadPool *)opaque;
    uint64_t observed_generation = 0;
    pthread_mutex_lock(&pool->mutex);
    for (;;) {
        while (!pool->shutdown &&
               pool->generation == observed_generation) {
            pthread_cond_wait(&pool->work_ready, &pool->mutex);
        }
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }
        observed_generation = pool->generation;
        pthread_mutex_unlock(&pool->mutex);
        execute_available_tasks(pool, observed_generation);
        pthread_mutex_lock(&pool->mutex);
    }
}

SJitThreadPool *sjit_thread_pool_create(int parallelism) {
    if (parallelism < 2) {
        return NULL;
    }
    if (parallelism > SJIT_THREAD_POOL_MAX_PARALLELISM) {
        parallelism = SJIT_THREAD_POOL_MAX_PARALLELISM;
    }
    SJitThreadPool *pool = (SJitThreadPool *)calloc(1, sizeof(*pool));
    if (!pool) {
        return NULL;
    }
    if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
        free(pool);
        return NULL;
    }
    if (pthread_cond_init(&pool->work_ready, NULL) != 0) {
        pthread_mutex_destroy(&pool->mutex);
        free(pool);
        return NULL;
    }
    if (pthread_cond_init(&pool->batch_complete, NULL) != 0) {
        pthread_cond_destroy(&pool->work_ready);
        pthread_mutex_destroy(&pool->mutex);
        free(pool);
        return NULL;
    }
    pool->workers = (pthread_t *)calloc(
        (size_t)(parallelism - 1), sizeof(pthread_t));
    if (!pool->workers) {
        pthread_cond_destroy(&pool->batch_complete);
        pthread_cond_destroy(&pool->work_ready);
        pthread_mutex_destroy(&pool->mutex);
        free(pool);
        return NULL;
    }
    for (int i = 0; i < parallelism - 1; ++i) {
        if (pthread_create(&pool->workers[i], NULL, worker_main, pool) != 0) {
            break;
        }
        ++pool->worker_count;
    }
    if (pool->worker_count == 0) {
        free(pool->workers);
        pthread_cond_destroy(&pool->batch_complete);
        pthread_cond_destroy(&pool->work_ready);
        pthread_mutex_destroy(&pool->mutex);
        free(pool);
        return NULL;
    }
    pool->parallelism = pool->worker_count + 1;
    return pool;
}

void sjit_thread_pool_destroy(SJitThreadPool *pool) {
    if (!pool) {
        return;
    }
    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->work_ready);
    pthread_mutex_unlock(&pool->mutex);
    for (int i = 0; i < pool->worker_count; ++i) {
        pthread_join(pool->workers[i], NULL);
    }
    free(pool->workers);
    pthread_cond_destroy(&pool->batch_complete);
    pthread_cond_destroy(&pool->work_ready);
    pthread_mutex_destroy(&pool->mutex);
    free(pool);
}

int sjit_thread_pool_parallelism(const SJitThreadPool *pool) {
    return pool ? pool->parallelism : 1;
}

int sjit_thread_pool_run(
    SJitThreadPool *pool,
    int task_count,
    SJitThreadPoolTaskFn function,
    void *context) {
    if (!pool || task_count < 2 || !function) {
        return 0;
    }
    pthread_mutex_lock(&pool->mutex);
    if (pool->shutdown || pool->remaining_tasks != 0) {
        pthread_mutex_unlock(&pool->mutex);
        return 0;
    }
    pool->function = function;
    pool->context = context;
    pool->task_count = task_count;
    pool->next_task = 0;
    pool->remaining_tasks = task_count;
    ++pool->generation;
    if (pool->generation == 0) {
        ++pool->generation;
    }
    const uint64_t generation = pool->generation;
    pthread_cond_broadcast(&pool->work_ready);
    pthread_mutex_unlock(&pool->mutex);

    /* The scheduler participates, so N configured CPUs require only N-1
       persistent background threads. */
    execute_available_tasks(pool, generation);

    pthread_mutex_lock(&pool->mutex);
    while (pool->remaining_tasks > 0) {
        pthread_cond_wait(&pool->batch_complete, &pool->mutex);
    }
    pool->function = NULL;
    pool->context = NULL;
    pool->task_count = 0;
    pool->next_task = 0;
    pthread_mutex_unlock(&pool->mutex);
    return 1;
}

static int parse_parallelism(const char *text) {
    if (!text || text[0] == '\0') {
        return 0;
    }
    char *end = NULL;
    errno = 0;
    const long parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < 1 ||
        parsed > INT_MAX) {
        return 0;
    }
    if (parsed > SJIT_THREAD_POOL_MAX_PARALLELISM) {
        return SJIT_THREAD_POOL_MAX_PARALLELISM;
    }
    return (int)parsed;
}

int sjit_thread_pool_environment_parallelism(void) {
    if (getenv("SJIT_DISABLE_THREAD_POOL") != NULL) {
        return 1;
    }
    const int configured = parse_parallelism(getenv("SJIT_THREAD_POOL_SIZE"));
    if (configured > 0) {
        return configured;
    }
    long detected = sysconf(_SC_NPROCESSORS_ONLN);
    if (detected < 1) {
        detected = 1;
    }
    if (detected > SJIT_THREAD_POOL_MAX_PARALLELISM) {
        detected = SJIT_THREAD_POOL_MAX_PARALLELISM;
    }
    return (int)detected;
}
