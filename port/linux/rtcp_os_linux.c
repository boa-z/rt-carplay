/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "rtcp_linux.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "rtcp/rtcp.h"

struct rtcp_thread {
    pthread_t tid;
};

struct rtcp_mutex {
    pthread_mutex_t mutex;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0U;
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

static void sleep_ms(uint32_t ms)
{
    struct timespec req = {
        .tv_sec = (time_t)(ms / 1000U),
        .tv_nsec = (long)(ms % 1000U) * 1000000L,
    };
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
}

struct thread_block {
    struct rtcp_thread handle;
    void (*fn)(void *);
    void *arg;
};

static void *thread_trampoline(void *arg)
{
    struct thread_block *block = arg;
    block->fn(block->arg);
    free(block);
    return NULL;
}

static rtcp_thread_t thread_create(const char *name,
                                   void (*fn)(void *), void *arg,
                                   size_t stack, int prio)
{
    (void)name;
    (void)stack;
    (void)prio;
    if (fn == NULL) return NULL;

    /* The detached thread owns the block and frees it on exit; the returned
     * handle is only an opaque identity and must not be dereferenced. */
    struct thread_block *block = malloc(sizeof(*block));
    if (block == NULL) return NULL;
    block->fn = fn;
    block->arg = arg;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&block->handle.tid, &attr, thread_trampoline, block);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(block);
        return NULL;
    }
    return (rtcp_thread_t)&block->handle;
}

static rtcp_mutex_t mutex_create(void)
{
    struct rtcp_mutex *mutex = malloc(sizeof(*mutex));
    if (mutex == NULL) return NULL;
    if (pthread_mutex_init(&mutex->mutex, NULL) != 0) {
        free(mutex);
        return NULL;
    }
    return mutex;
}

static void mutex_lock(rtcp_mutex_t mutex)
{
    if (mutex != NULL) pthread_mutex_lock(&mutex->mutex);
}

static void mutex_unlock(rtcp_mutex_t mutex)
{
    if (mutex != NULL) pthread_mutex_unlock(&mutex->mutex);
}

static void mutex_destroy(rtcp_mutex_t mutex)
{
    if (mutex == NULL) return;
    pthread_mutex_destroy(&mutex->mutex);
    free(mutex);
}

const struct rtcp_os rtcp_os_linux = {
    .now_ms = now_ms,
    .sleep_ms = sleep_ms,
    .alloc = malloc,
    .free = free,
    .thread_create = thread_create,
    .mutex_create = mutex_create,
    .mutex_lock = mutex_lock,
    .mutex_unlock = mutex_unlock,
    .mutex_destroy = mutex_destroy,
};
