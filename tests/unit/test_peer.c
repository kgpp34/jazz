#include "control/peer_internal.h"
#include "check.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);
static bool fail_allocation;

void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_allocation)
        return NULL;
    return __real_calloc(count, size);
}

static void allocation_failure(void)
{
    jz_peer *peer = NULL;
    struct jz_error error;
    fail_allocation = true;
    CHECK(jz_peer_create(&peer, &error) == -ENOMEM && !peer);
    fail_allocation = false;
    CHECK(error.code == -ENOMEM && !strcmp(error.stage, "peer-allocate"));
}

static struct jz_peer_info info(jz_peer *peer)
{
    struct jz_peer_info snapshot;
    CHECK(jz_peer_get_info(peer, &snapshot, NULL) == 0);
    return snapshot;
}

static jz_peer *create_peer(void)
{
    jz_peer *peer = NULL;
    struct jz_error error = {.code = -EIO};
    CHECK(jz_peer_create(&peer, &error) == 0 && peer && !error.code);
    CHECK(info(peer).state == JZ_PEER_CONNECTING);
    return peer;
}

static void finish_peer(jz_peer **peer)
{
    CHECK(jz_peer_request_close(*peer, NULL) == 0);
    CHECK(jz_peer_set_closed(*peer, NULL) == 0);
    CHECK(jz_peer_destroy(peer, NULL) == 0 && !*peer);
}

static void lifecycle(void)
{
    jz_peer *peer = create_peer();
    struct jz_error error;
    CHECK(jz_peer_destroy(&peer, &error) == -EBUSY && peer);
    CHECK(jz_peer_set_closed(peer, NULL) == -EBUSY);
    CHECK(jz_peer_wait_ready(peer, 0, &error) == -ETIMEDOUT);
    CHECK(error.code == -ETIMEDOUT && info(peer).state == JZ_PEER_CONNECTING);
    CHECK(jz_peer_wait_ready(peer, 10, NULL) == -ETIMEDOUT);
    CHECK(info(peer).state == JZ_PEER_CONNECTING);
    CHECK(jz_peer_set_active(peer, NULL) == 0);
    CHECK(jz_peer_set_active(peer, NULL) == -EALREADY);
    CHECK(jz_peer_wait_ready(peer, UINT32_MAX, &error) == 0 && !error.code);
    CHECK(jz_peer_wait_closed(peer, 0, NULL) == -ETIMEDOUT);
    CHECK(jz_peer_destroy(&peer, NULL) == -EBUSY && peer);
    struct jz_peer_info snapshot = info(peer);
    CHECK(jz_peer_request_close(peer, &error) == 0 && !error.code);
    CHECK(jz_peer_request_close(peer, NULL) == 0);
    CHECK(snapshot.state == JZ_PEER_ACTIVE); /* Queries return values, not live views. */
    CHECK(info(peer).state == JZ_PEER_STOPPING);
    CHECK(jz_peer_set_active(peer, NULL) == -ECANCELED);
    CHECK(jz_peer_wait_ready(peer, 0, NULL) == -ECANCELED);
    CHECK(jz_peer_wait_closed(peer, 0, NULL) == -ETIMEDOUT);
    CHECK(jz_peer_set_closed(peer, &error) == 0 && !error.code);
    CHECK(jz_peer_set_closed(peer, NULL) == 0);
    CHECK(jz_peer_request_close(peer, NULL) == 0);
    CHECK(jz_peer_wait_closed(peer, 0, &error) == 0 && !error.code);
    CHECK(jz_peer_wait_ready(peer, 0, NULL) == -ECANCELED);
    CHECK(jz_peer_destroy(&peer, &error) == 0 && !peer && !error.code);
    CHECK(jz_peer_destroy(&peer, NULL) == 0);
}

static void failure_and_cleanup(void)
{
    jz_peer *peer = create_peer();
    const struct jz_error refused = {-ECONNREFUSED, "test-refused", ECONNREFUSED};
    const struct jz_error later = {-ETIMEDOUT, "test-late-timeout", ETIMEDOUT};
    const struct jz_error cleanup = {-EBUSY, "test-destroy-qp", EBUSY};
    CHECK(jz_peer_set_cleanup_error(peer, &cleanup) == -EBUSY);
    CHECK(jz_peer_fail(peer, &refused) == 0);
    CHECK(jz_peer_fail(peer, &later) == 0);
    CHECK(jz_peer_set_cleanup_error(peer, &cleanup) == 0);
    struct jz_peer_info snapshot = info(peer);
    CHECK(snapshot.state == JZ_PEER_STOPPING);
    CHECK(snapshot.terminal_error.code == -ECONNREFUSED);
    CHECK(!strcmp(snapshot.terminal_error.stage, "test-refused"));
    CHECK(snapshot.cleanup_error.code == -EBUSY);
    struct jz_error error;
    CHECK(jz_peer_wait_ready(peer, 0, &error) == -ECONNREFUSED);
    CHECK(error.system_errno == ECONNREFUSED);
    CHECK(jz_peer_wait_closed(peer, 0, NULL) == -ETIMEDOUT);
    CHECK(jz_peer_destroy(&peer, NULL) == -EBUSY && peer);
    CHECK(jz_peer_set_closed(peer, NULL) == 0); /* Test owner certifies retirement. */
    snapshot = info(peer);
    CHECK(snapshot.state == JZ_PEER_CLOSED && !snapshot.cleanup_error.code);
    CHECK(snapshot.terminal_error.code == -ECONNREFUSED);
    CHECK(jz_peer_wait_closed(peer, 0, NULL) == 0);
    CHECK(jz_peer_fail(peer, &later) == -EALREADY);
    CHECK(jz_peer_set_cleanup_error(peer, &cleanup) == -EBUSY);
    CHECK(jz_peer_destroy(&peer, NULL) == 0);
    CHECK(!strcmp(snapshot.terminal_error.stage, "test-refused"));
    peer = create_peer();
    CHECK(jz_peer_set_active(peer, NULL) == 0);
    CHECK(jz_peer_fail(peer, &refused) == 0);
    CHECK(info(peer).state == JZ_PEER_STOPPING);
    finish_peer(&peer);
}

struct waiter {
    jz_peer *peer;
    atomic_uint *entered;
    bool closing;
    int result;
};

static void *wait_thread(void *arg)
{
    struct waiter *waiter = arg;
    atomic_fetch_add(waiter->entered, 1);
    waiter->result = waiter->closing ? jz_peer_wait_closed(waiter->peer, 2000, NULL)
                                     : jz_peer_wait_ready(waiter->peer, 2000, NULL);
    return NULL;
}

/* Publish may happen before or after a waiter actually blocks. Both schedules
 * must work without depending on a sleep to make a waiter "probably ready". */
static void multiple_waiters(bool closing, bool fail)
{
    enum { COUNT = 8 };
    jz_peer *peer = create_peer();
    if (closing)
        CHECK(jz_peer_request_close(peer, NULL) == 0);
    atomic_uint entered = 0;
    struct waiter waiters[COUNT];
    pthread_t threads[COUNT];
    for (size_t i = 0; i < COUNT; ++i) {
        waiters[i] = (struct waiter){.peer = peer, .entered = &entered, .closing = closing};
        CHECK(pthread_create(&threads[i], NULL, wait_thread, &waiters[i]) == 0);
    }
    while (atomic_load(&entered) != COUNT)
        sched_yield();
    if (closing)
        CHECK(jz_peer_set_closed(peer, NULL) == 0);
    else if (fail)
        CHECK(jz_peer_request_close(peer, NULL) == 0);
    else
        CHECK(jz_peer_set_active(peer, NULL) == 0);
    for (size_t i = 0; i < COUNT; ++i) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        CHECK(waiters[i].result == (fail ? -ECANCELED : 0));
    }
    finish_peer(&peer);
}

static void cancelled_waiter_releases_lock(void)
{
    jz_peer *peer = create_peer();
    atomic_uint entered = 0;
    struct waiter waiter = {.peer = peer, .entered = &entered};
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, wait_thread, &waiter) == 0);
    while (!atomic_load(&entered))
        sched_yield();
    CHECK(pthread_cancel(thread) == 0);
    void *result = NULL;
    CHECK(pthread_join(thread, &result) == 0 && result == PTHREAD_CANCELED);
    /* Cancellation must leave the session CONNECTING and its mutex usable. */
    CHECK(info(peer).state == JZ_PEER_CONNECTING);
    CHECK(jz_peer_set_active(peer, NULL) == 0);
    finish_peer(&peer);
}

struct racing_close {
    jz_peer *peer;
    pthread_barrier_t *start;
    bool activate;
    int result;
};

static void *race_thread(void *arg)
{
    struct racing_close *race = arg;
    int rc = pthread_barrier_wait(race->start);
    CHECK(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
    race->result = race->activate ? jz_peer_set_active(race->peer, NULL)
                                  : jz_peer_request_close(race->peer, NULL);
    return NULL;
}

static void close_races_readiness(void)
{
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
        jz_peer *peer = create_peer();
        pthread_barrier_t start;
        CHECK(pthread_barrier_init(&start, NULL, 2) == 0);
        struct racing_close active = {.peer = peer, .start = &start, .activate = true};
        struct racing_close close = {.peer = peer, .start = &start};
        pthread_t a, b;
        CHECK(pthread_create(&a, NULL, race_thread, &active) == 0);
        CHECK(pthread_create(&b, NULL, race_thread, &close) == 0);
        CHECK(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0);
        CHECK(active.result == 0 || active.result == -ECANCELED);
        CHECK(close.result == 0 && info(peer).state == JZ_PEER_STOPPING);
        CHECK(jz_peer_set_active(peer, NULL) == -ECANCELED);
        CHECK(pthread_barrier_destroy(&start) == 0);
        finish_peer(&peer);
    }
}

struct wake_storm {
    jz_peer *peer;
    atomic_bool done;
};

static void *repeated_failure(void *arg)
{
    struct wake_storm *storm = arg;
    const struct jz_error cause = {-EIO, "test-repeated-failure", EIO};
    const struct timespec pause = {.tv_nsec = 1000000};
    while (!atomic_load(&storm->done)) {
        CHECK(jz_peer_fail(storm->peer, &cause) == 0);
        nanosleep(&pause, NULL);
    }
    return NULL;
}

static void bounded_wait_despite_wakeups(void)
{
    jz_peer *peer = create_peer();
    CHECK(jz_peer_request_close(peer, NULL) == 0);
    struct wake_storm storm = {.peer = peer, .done = false};
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, repeated_failure, &storm) == 0);
    /* Repeated publications must not renew this wait's original deadline. */
    CHECK(jz_peer_wait_closed(peer, 20, NULL) == -ETIMEDOUT);
    atomic_store(&storm.done, true);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(info(peer).state == JZ_PEER_STOPPING);
    finish_peer(&peer);
}

static void invalid_arguments(void)
{
    jz_peer *peer = NULL;
    CHECK(jz_peer_create(NULL, NULL) == -EINVAL);
    CHECK(jz_peer_destroy(NULL, NULL) == -EINVAL);
    CHECK(jz_peer_get_info(NULL, NULL, NULL) == -EINVAL);
    CHECK(jz_peer_wait_ready(NULL, 0, NULL) == -EINVAL);
    CHECK(jz_peer_wait_closed(NULL, 0, NULL) == -EINVAL);
    CHECK(jz_peer_request_close(NULL, NULL) == -EINVAL);
    CHECK(jz_peer_set_active(NULL, NULL) == -EINVAL);
    CHECK(jz_peer_set_closed(NULL, NULL) == -EINVAL);
    CHECK(jz_peer_fail(NULL, NULL) == -EINVAL);
    CHECK(jz_peer_set_cleanup_error(NULL, NULL) == -EINVAL);
    peer = create_peer();
    CHECK(jz_peer_create(&peer, NULL) == -EINVAL);
    CHECK(jz_peer_get_info(peer, NULL, NULL) == -EINVAL);
    const struct jz_error invalid = {0};
    CHECK(jz_peer_fail(peer, &invalid) == -EINVAL);
    CHECK(jz_peer_set_cleanup_error(peer, &invalid) == -EINVAL);
    finish_peer(&peer);
}

int main(void)
{
    allocation_failure();
    invalid_arguments();
    lifecycle();
    failure_and_cleanup();
    multiple_waiters(false, false);
    multiple_waiters(false, true);
    multiple_waiters(true, false);
    cancelled_waiter_releases_lock();
    close_races_readiness();
    bounded_wait_despite_wakeups();
    return 0;
}
