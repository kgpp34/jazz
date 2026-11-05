#include "control/listener_internal.h"
#include "control/peer_internal.h"
#include "check.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

void *__real_calloc(size_t count, size_t size);
void *__wrap_calloc(size_t count, size_t size);
static size_t fail_after;
void *__wrap_calloc(size_t count, size_t size)
{
    if (fail_after && !--fail_after)
        return NULL;
    return __real_calloc(count, size);
}

static struct jz_listener_info info(jz_listener *listener)
{
    struct jz_listener_info snapshot;
    CHECK(jz_listener_get_info(listener, &snapshot, NULL) == 0);
    CHECK(snapshot.pending + snapshot.queued + snapshot.retiring <= snapshot.capacity);
    CHECK(snapshot.pending <= snapshot.max_pending);
    return snapshot;
}

static jz_listener *create(size_t pending, size_t capacity)
{
    jz_listener *listener = NULL;
    CHECK(jz_listener_create(&listener, pending, capacity, NULL) == 0 && listener);
    CHECK(info(listener).state == JZ_LISTENER_LISTENING);
    return listener;
}

static void destroy_peer(jz_peer **peer)
{
    CHECK(jz_peer_request_close(*peer, NULL) == 0);
    CHECK(jz_peer_set_closed(*peer, NULL) == 0);
    CHECK(jz_peer_destroy(peer, NULL) == 0 && !*peer);
}

static void drain(jz_listener *listener)
{
    struct jz_listener_ticket ticket;
    jz_peer *peer = NULL;
    int rc;
    while (!(rc = jz_listener_take_retiring(listener, &ticket, &peer, NULL))) {
        struct jz_peer_info snapshot;
        CHECK(jz_peer_get_info(peer, &snapshot, NULL) == 0);
        CHECK(snapshot.state == JZ_PEER_STOPPING);
        CHECK(jz_listener_retire_done(listener, ticket, NULL) == 0);
        peer = NULL;
    }
    CHECK(rc == -EAGAIN);
}

static void finish(jz_listener **listener)
{
    CHECK(jz_listener_request_close(*listener, NULL) == 0);
    CHECK(jz_listener_source_closed(*listener, NULL) == 0);
    drain(*listener);
    CHECK(jz_listener_wait_closed(*listener, 0, NULL) == 0);
    CHECK(jz_listener_destroy(listener, NULL) == 0 && !*listener);
}

static void limits_and_delivery(void)
{
    jz_listener *listener = create(1, 2);
    struct jz_listener_ticket a, b, unused = {99, 99};
    jz_peer *pa = NULL, *pb = NULL, *other = NULL;
    CHECK(jz_listener_admit(listener, &a, &pa, NULL) == 0);
    CHECK(jz_listener_admit(listener, &unused, &other, NULL) == -EAGAIN);
    CHECK(!other && unused.slot == 99 && unused.generation == 99);
    CHECK(jz_listener_publish(listener, a, NULL) == 0);
    CHECK(jz_listener_publish(listener, a, NULL) == -EALREADY);
    CHECK(jz_listener_admit(listener, &b, &pb, NULL) == 0);
    CHECK(jz_listener_publish(listener, b, NULL) == 0);
    CHECK(jz_listener_admit(listener, &unused, &other, NULL) == -ENOSPC && !other);
    struct jz_listener_info snapshot = info(listener);
    CHECK(snapshot.queued == 2 && !snapshot.pending && !snapshot.retiring);
    jz_peer *first = NULL, *second = NULL;
    CHECK(jz_listener_next_peer(listener, &first, 0, NULL) == 0 && first == pa);
    CHECK(jz_listener_next_peer(listener, &second, 0, NULL) == 0 && second == pb);
    CHECK(jz_listener_next_peer(listener, &other, 0, NULL) == -ETIMEDOUT && !other);
    CHECK(info(listener).state == JZ_LISTENER_LISTENING);
    CHECK(snapshot.queued == 2 && info(listener).queued == 0);
    finish(&listener);
    struct jz_peer_info peer_snapshot;
    CHECK(jz_peer_get_info(first, &peer_snapshot, NULL) == 0);
    CHECK(peer_snapshot.state ==
          JZ_PEER_ACTIVE); /* Closing the listener left delivered peers alone. */
    CHECK(jz_peer_wait_ready(second, 0, NULL) == 0);
    destroy_peer(&first);
    destroy_peer(&second);
}

static void close_during_setup_and_retry(void)
{
    jz_listener *listener = create(1, 1);
    struct jz_listener_ticket setup, lease, retry;
    jz_peer *borrowed = NULL, *cleanup = NULL;
    CHECK(jz_listener_admit(listener, &setup, &borrowed, NULL) == 0);
    CHECK(jz_listener_source_closed(listener, NULL) == -EBUSY);
    CHECK(jz_listener_request_close(listener, NULL) == 0);
    CHECK(jz_listener_request_close(listener, NULL) == 0);
    CHECK(jz_listener_source_closed(listener, NULL) == 0);
    CHECK(info(listener).pending == 1);
    CHECK(jz_listener_take_retiring(listener, &lease, &cleanup, NULL) == -EAGAIN);
    CHECK(jz_listener_wait_closed(listener, 0, NULL) == -ETIMEDOUT);
    CHECK(jz_listener_destroy(&listener, NULL) == -EBUSY && listener);
    CHECK(jz_peer_wait_ready(borrowed, 0, NULL) == -ECANCELED);
    CHECK(jz_listener_publish(listener, setup, NULL) == -ECANCELED);
    borrowed = NULL; /* End of the setup borrow, even though publication failed. */
    CHECK(info(listener).pending == 0 && info(listener).retiring == 1);
    CHECK(jz_listener_take_retiring(listener, &lease, &cleanup, NULL) == 0);
    CHECK(lease.generation != setup.generation);
    const struct jz_error busy = {-EBUSY, "test-cleanup-busy", EBUSY};
    CHECK(jz_listener_retire_failed(listener, lease, &busy, NULL) == 0);
    cleanup = NULL;
    CHECK(info(listener).cleanup_error.code == -EBUSY);
    CHECK(jz_listener_wait_closed(listener, 0, NULL) == -ETIMEDOUT);
    CHECK(jz_listener_take_retiring(listener, &retry, &cleanup, NULL) == 0);
    CHECK(retry.generation != lease.generation);
    CHECK(jz_listener_retire_done(listener, lease, NULL) == -ESTALE);
    CHECK(jz_listener_retire_failed(listener, lease, &busy, NULL) == -ESTALE);
    CHECK(jz_listener_retire_done(listener, retry, NULL) == 0);
    cleanup = NULL;
    CHECK(info(listener).state == JZ_LISTENER_CLOSED && !info(listener).cleanup_error.code);
    CHECK(jz_listener_retire_done(listener, retry, NULL) == -ESTALE);
    CHECK(jz_listener_publish(listener, setup, NULL) == -ESTALE);
    CHECK(jz_listener_next_peer(listener, &borrowed, 0, NULL) == -ECANCELED && !borrowed);
    CHECK(jz_listener_admit(listener, &setup, &borrowed, NULL) == -ECANCELED);
    finish(&listener);
}

static void cleanup_capacity_and_fairness(void)
{
    jz_listener *listener = create(2, 2);
    struct jz_listener_ticket a, b, first, second, retry, later;
    jz_peer *peer = NULL, *unused = NULL;
    CHECK(jz_listener_admit(listener, &a, &peer, NULL) == 0);
    const struct jz_error failure = {-EPROTO, "test-invalid-protocol", EPROTO};
    struct jz_error aliased = failure;
    CHECK(jz_listener_abort(listener, a, &aliased, &aliased) == 0 && !aliased.code);
    peer = NULL;
    CHECK(jz_listener_admit(listener, &b, &peer, NULL) == 0);
    CHECK(jz_listener_abort(listener, b, NULL, NULL) == 0);
    peer = NULL;
    CHECK(jz_listener_admit(listener, &later, &unused, NULL) == -ENOSPC && !unused);
    CHECK(jz_listener_take_retiring(listener, &first, &peer, NULL) == 0);
    CHECK(first.slot == a.slot);
    CHECK(jz_listener_retire_done(listener, b, NULL) == -EBUSY);
    struct jz_peer_info peer_info;
    CHECK(jz_peer_get_info(peer, &peer_info, NULL) == 0 &&
          peer_info.terminal_error.code == -EPROTO);
    const struct jz_error cleanup = {-EIO, "test-cleanup-error", EIO};
    aliased = cleanup;
    CHECK(jz_listener_retire_failed(listener, first, &aliased, &aliased) == 0 && !aliased.code);
    peer = NULL;
    CHECK(jz_listener_take_retiring(listener, &second, &peer, NULL) == 0);
    CHECK(second.slot == b.slot); /* Repeated failure in slot A cannot starve B. */
    CHECK(jz_listener_retire_done(listener, second, NULL) == 0);
    peer = NULL;
    CHECK(jz_listener_admit(listener, &later, &unused, NULL) == 0);
    CHECK(later.slot == b.slot && later.generation != b.generation);
    CHECK(jz_listener_publish(listener, b, NULL) == -ESTALE);
    CHECK(jz_listener_abort(listener, b, &failure, NULL) == -ESTALE);
    CHECK(jz_listener_abort(listener, later, NULL, NULL) == 0);
    unused = NULL;
    CHECK(jz_listener_take_retiring(listener, &retry, &peer, NULL) == 0);
    CHECK(retry.slot == a.slot);
    CHECK(jz_listener_retire_done(listener, first, NULL) == -ESTALE);
    CHECK(jz_listener_retire_done(listener, retry, NULL) == 0);
    peer = NULL;
    finish(&listener);
}

static void allocation_failures(void)
{
    for (size_t allocation = 1; allocation <= 3; ++allocation) {
        jz_listener *listener = NULL;
        fail_after = allocation;
        CHECK(jz_listener_create(&listener, 2, 2, NULL) == -ENOMEM && !listener);
        fail_after = 0;
    }
    jz_listener *listener = create(1, 1);
    struct jz_listener_ticket ticket = {9, 9};
    jz_peer *peer = NULL;
    fail_after = 1;
    CHECK(jz_listener_admit(listener, &ticket, &peer, NULL) == -ENOMEM && !peer);
    fail_after = 0;
    CHECK(ticket.slot == 9 && ticket.generation == 9 && !info(listener).pending);
    CHECK(jz_listener_admit(listener, &ticket, &peer, NULL) == 0);
    CHECK(jz_listener_abort(listener, ticket, NULL, NULL) == 0);
    finish(&listener);
}

struct race {
    jz_listener *listener;
    struct jz_listener_ticket ticket;
    pthread_barrier_t *barrier;
    bool publish;
    bool close;
    int result;
    jz_peer *received;
};
static void *race_thread(void *arg)
{
    struct race *race = arg;
    int rc = pthread_barrier_wait(race->barrier);
    CHECK(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
    if (race->close)
        race->result = jz_listener_request_close(race->listener, NULL);
    else if (race->publish)
        race->result = jz_listener_publish(race->listener, race->ticket, NULL);
    else
        race->result = jz_listener_next_peer(race->listener, &race->received, 0, NULL);
    return NULL;
}

static void close_races(bool publish)
{
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        jz_listener *listener = create(1, 1);
        struct jz_listener_ticket ticket;
        jz_peer *peer = NULL;
        CHECK(jz_listener_admit(listener, &ticket, &peer, NULL) == 0);
        if (!publish)
            CHECK(jz_listener_publish(listener, ticket, NULL) == 0);
        pthread_barrier_t barrier;
        CHECK(pthread_barrier_init(&barrier, NULL, 2) == 0);
        struct race operation = {
            .listener = listener, .ticket = ticket, .barrier = &barrier, .publish = publish};
        struct race close = {.listener = listener, .barrier = &barrier, .close = true};
        pthread_t a, b;
        CHECK(pthread_create(&a, NULL, race_thread, &operation) == 0);
        CHECK(pthread_create(&b, NULL, race_thread, &close) == 0);
        CHECK(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0);
        CHECK(close.result == 0);
        CHECK(operation.result == 0 || operation.result == -ECANCELED);
        if (operation.received) {
            CHECK(operation.received == peer && jz_peer_wait_ready(peer, 0, NULL) == 0);
            destroy_peer(&operation.received);
        }
        CHECK(pthread_barrier_destroy(&barrier) == 0);
        finish(&listener);
    }
}

struct stress {
    jz_listener *listener;
    atomic_uint received;
};
enum { PRODUCERS = 4, PER_PRODUCER = 64 };
static void *producer(void *arg)
{
    struct stress *stress = arg;
    for (unsigned i = 0; i < PER_PRODUCER; ++i) {
        jz_peer *peer = NULL;
        struct jz_listener_ticket ticket;
        int rc;
        while ((rc = jz_listener_admit(stress->listener, &ticket, &peer, NULL))) {
            CHECK(rc == -EAGAIN || rc == -ENOSPC);
            sched_yield();
        }
        CHECK(jz_listener_publish(stress->listener, ticket, NULL) == 0);
    }
    return NULL;
}
static void *consumer(void *arg)
{
    struct stress *stress = arg;
    while (atomic_load(&stress->received) < PRODUCERS * PER_PRODUCER) {
        jz_peer *peer = NULL;
        int rc = jz_listener_next_peer(stress->listener, &peer, 20, NULL);
        if (rc == -ETIMEDOUT)
            continue;
        CHECK(rc == 0 && peer && jz_peer_wait_ready(peer, 0, NULL) == 0);
        destroy_peer(&peer);
        atomic_fetch_add(&stress->received, 1);
        (void)info(stress->listener);
    }
    return NULL;
}
static void concurrent_admission(void)
{
    struct stress stress = {.listener = create(4, 8), .received = 0};
    pthread_t writers[PRODUCERS], readers[2];
    for (size_t i = 0; i < 2; ++i)
        CHECK(pthread_create(&readers[i], NULL, consumer, &stress) == 0);
    for (size_t i = 0; i < PRODUCERS; ++i)
        CHECK(pthread_create(&writers[i], NULL, producer, &stress) == 0);
    for (size_t i = 0; i < PRODUCERS; ++i)
        CHECK(pthread_join(writers[i], NULL) == 0);
    for (size_t i = 0; i < 2; ++i)
        CHECK(pthread_join(readers[i], NULL) == 0);
    CHECK(atomic_load(&stress.received) == PRODUCERS * PER_PRODUCER);
    finish(&stress.listener);
}

struct waiting {
    jz_listener *listener;
    atomic_bool entered;
    bool closing;
    int result;
};
static void *wait_thread(void *arg)
{
    struct waiting *waiting = arg;
    atomic_store(&waiting->entered, true);
    jz_peer *peer = NULL;
    waiting->result = waiting->closing
                          ? jz_listener_wait_closed(waiting->listener, 2000, NULL)
                          : jz_listener_next_peer(waiting->listener, &peer, 2000, NULL);
    CHECK(!peer);
    return NULL;
}
static void waiters(bool closing, bool cancel)
{
    jz_listener *listener = create(1, 1);
    struct waiting waiting = {.listener = listener, .entered = false, .closing = closing};
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, wait_thread, &waiting) == 0);
    while (!atomic_load(&waiting.entered))
        sched_yield();
    if (cancel)
        CHECK(pthread_cancel(thread) == 0);
    else {
        CHECK(jz_listener_request_close(listener, NULL) == 0);
        CHECK(jz_listener_source_closed(listener, NULL) == 0);
    }
    void *result = NULL;
    CHECK(pthread_join(thread, &result) == 0);
    if (cancel) {
        CHECK(result == PTHREAD_CANCELED);
        CHECK(info(listener).state == JZ_LISTENER_LISTENING);
    } else
        CHECK(waiting.result == (closing ? 0 : -ECANCELED));
    finish(&listener);
}

static void invalid_arguments(void)
{
    jz_listener *listener = NULL;
    jz_peer *peer = NULL;
    struct jz_listener_ticket ticket = {0};
    CHECK(jz_listener_create(NULL, 1, 1, NULL) == -EINVAL);
    CHECK(jz_listener_create(&listener, 0, 1, NULL) == -EINVAL);
    CHECK(jz_listener_create(&listener, 1, 0, NULL) == -EINVAL);
    CHECK(jz_listener_create(&listener, 1, SIZE_MAX, NULL) == -EOVERFLOW);
    CHECK(jz_listener_destroy(NULL, NULL) == -EINVAL);
    CHECK(jz_listener_destroy(&listener, NULL) == 0);
    CHECK(jz_listener_get_info(NULL, NULL, NULL) == -EINVAL);
    CHECK(jz_listener_next_peer(NULL, &peer, 0, NULL) == -EINVAL);
    CHECK(jz_listener_request_close(NULL, NULL) == -EINVAL);
    CHECK(jz_listener_wait_closed(NULL, 0, NULL) == -EINVAL);
    CHECK(jz_listener_admit(NULL, &ticket, &peer, NULL) == -EINVAL);
    CHECK(jz_listener_publish(NULL, ticket, NULL) == -EINVAL);
    CHECK(jz_listener_abort(NULL, ticket, NULL, NULL) == -EINVAL);
    CHECK(jz_listener_take_retiring(NULL, &ticket, &peer, NULL) == -EINVAL);
    CHECK(jz_listener_retire_done(NULL, ticket, NULL) == -EINVAL);
    CHECK(jz_listener_source_closed(NULL, NULL) == -EINVAL);
    listener = create(1, 1);
    CHECK(jz_listener_destroy(&listener, NULL) == -EBUSY && listener);
    CHECK(jz_listener_publish(listener, ticket, NULL) == -ESTALE);
    CHECK(jz_listener_retire_done(listener, ticket, NULL) == -ESTALE);
    CHECK(jz_listener_get_info(listener, NULL, NULL) == -EINVAL);
    CHECK(jz_listener_next_peer(listener, NULL, 0, NULL) == -EINVAL);
    CHECK(jz_listener_admit(listener, NULL, &peer, NULL) == -EINVAL);
    const struct jz_error invalid = {0};
    CHECK(jz_listener_abort(listener, ticket, &invalid, NULL) == -EINVAL);
    CHECK(jz_listener_retire_failed(listener, ticket, &invalid, NULL) == -EINVAL);
    CHECK(jz_listener_set_cleanup_error(listener, &invalid, NULL) == -EINVAL);
    const struct jz_error busy = {-EBUSY, "test-source-busy", EBUSY};
    CHECK(jz_listener_set_cleanup_error(listener, &busy, NULL) == -EBUSY);
    CHECK(jz_listener_request_close(listener, NULL) == 0);
    struct jz_error aliased = busy;
    CHECK(jz_listener_set_cleanup_error(listener, &aliased, &aliased) == 0 && !aliased.code);
    CHECK(info(listener).cleanup_error.code == -EBUSY);
    CHECK(jz_listener_wait_closed(listener, 0, NULL) == -ETIMEDOUT);
    finish(&listener);
}

int main(void)
{
    invalid_arguments();
    allocation_failures();
    limits_and_delivery();
    close_during_setup_and_retry();
    cleanup_capacity_and_fairness();
    close_races(false);
    close_races(true);
    concurrent_admission();
    waiters(false, false);
    waiters(true, false);
    waiters(false, true);
    waiters(true, true);
    return 0;
}
