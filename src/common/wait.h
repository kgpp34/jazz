#ifndef JZ_WAIT_H
#define JZ_WAIT_H
#include "common/error.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>

/* Compute once per wait. Repeated notifications must not renew its budget.
 * Linux uses signed time_t; check addition before narrowing on 32-bit builds. */
static inline int jz_wait_deadline(uint32_t timeout_ms, struct timespec *deadline,
                                   struct jz_error *error)
{
    _Static_assert(sizeof(time_t) <= sizeof(uintmax_t), "time_t must fit uintmax_t");
    if (clock_gettime(CLOCK_MONOTONIC, deadline))
        return jz_fail(error, errno, "wait-clock");
    uintmax_t seconds = timeout_ms / UINT32_C(1000);
    long nanos = (long)(timeout_ms % UINT32_C(1000)) * 1000000L + deadline->tv_nsec;
    if (nanos >= 1000000000L) {
        ++seconds;
        nanos -= 1000000000L;
    }
    const uintmax_t maximum = (UINTMAX_C(1) << (sizeof(time_t) * CHAR_BIT - 1U)) - 1U;
    if (deadline->tv_sec < 0 || (uintmax_t)deadline->tv_sec > maximum - seconds)
        return jz_fail(error, EOVERFLOW, "wait-deadline");
    deadline->tv_sec += (time_t)seconds;
    deadline->tv_nsec = nanos;
    return 0;
}

/* A cancelled condition wait reacquires its mutex before running cleanup. */
static inline void jz_wait_unlock(void *lock)
{
    pthread_mutex_unlock(lock);
}
#endif
