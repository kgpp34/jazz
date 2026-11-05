#include "control/listener_internal.h"
#include "control/peer_internal.h"
#include "common/wait.h"
#include <stdbool.h>
#include <stdlib.h>

enum slot_state { SLOT_FREE, SLOT_PENDING, SLOT_QUEUED, SLOT_RETIRING, SLOT_REAPING };
struct incoming {
    jz_peer *peer;         /* Listener-owned until delivery; never freed during setup. */
    uint64_t generation;   /* Never wrap: old tickets must not become valid again. */
    enum slot_state state; /* Setup, delivery and cleanup ownership are exclusive. */
    size_t next_free;      /* Free-list link; capacity is the end sentinel. */
};
struct jz_listener {
    pthread_mutex_t lock;   /* Admission, publication, delivery and close linearize here. */
    pthread_cond_t changed; /* Monotonic delivery/closed wait notification. */
    struct jz_listener_info info;
    struct incoming *slots; /* Fixed capacity, including unreclaimed failures. */
    size_t *queue;          /* FIFO of published slot indices, allocated at construction. */
    size_t head;            /* Queue head; queued count lives in info. */
    size_t free_head;       /* O(1) free-slot allocation and return. */
    size_t retire_cursor;   /* Failed cleanup must not starve other retired entries. */
    bool source_closed;     /* Owner certified listen-source retirement. */
};

static bool valid_cause(const struct jz_error *cause)
{
    return cause && cause->code < 0 && cause->stage;
}

int jz_listener_create(jz_listener **out, size_t max_pending, size_t capacity,
                       struct jz_error *error)
{
    jz_error_clear(error);
    if (!out || *out || !max_pending || !capacity)
        return jz_fail(error, EINVAL, "listener-config");
    if (capacity > SIZE_MAX / sizeof(struct incoming) || capacity > SIZE_MAX / sizeof(size_t))
        return jz_fail(error, EOVERFLOW, "listener-capacity");
    jz_listener *listener = calloc(1, sizeof(*listener));
    if (!listener)
        return jz_fail(error, ENOMEM, "listener-allocate");
    listener->slots = calloc(capacity, sizeof(*listener->slots));
    listener->queue = calloc(capacity, sizeof(*listener->queue));
    int rc = ENOMEM;
    if (!listener->slots || !listener->queue)
        goto failed_storage;
    rc = pthread_mutex_init(&listener->lock, NULL);
    if (rc)
        goto failed_storage;
    pthread_condattr_t attr;
    rc = pthread_condattr_init(&attr);
    if (rc)
        goto failed_mutex;
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!rc)
        rc = pthread_cond_init(&listener->changed, &attr);
    pthread_condattr_destroy(&attr);
    if (rc)
        goto failed_mutex;
    listener->info = (struct jz_listener_info){
        .state = JZ_LISTENER_LISTENING, .max_pending = max_pending, .capacity = capacity};
    for (size_t i = 0; i < capacity; ++i)
        listener->slots[i].next_free = i + 1;
    *out = listener;
    return 0;
failed_mutex:
    pthread_mutex_destroy(&listener->lock);
failed_storage:
    free(listener->queue);
    free(listener->slots);
    free(listener);
    return jz_fail(error, rc, "listener-initialize");
}

int jz_listener_get_info(const jz_listener *listener, struct jz_listener_info *out,
                         struct jz_error *error)
{
    jz_error_clear(error);
    if (!listener || !out)
        return jz_fail(error, EINVAL, "listener-query");
    jz_listener *mutable = (jz_listener *)listener;
    pthread_mutex_lock(&mutable->lock);
    *out = listener->info;
    pthread_mutex_unlock(&mutable->lock);
    return 0;
}

/* The slot and generation together validate a callback without dereferencing
 * an old peer pointer. Always called while holding the listener lock. */
static struct incoming *lookup(jz_listener *listener, struct jz_listener_ticket ticket)
{
    if (ticket.slot >= listener->info.capacity || !ticket.generation)
        return NULL;
    struct incoming *entry = &listener->slots[ticket.slot];
    return entry->state != SLOT_FREE && entry->generation == ticket.generation ? entry : NULL;
}

static void check_closed(jz_listener *listener)
{
    if (listener->info.state == JZ_LISTENER_STOPPING && listener->source_closed &&
        !listener->info.pending && !listener->info.queued && !listener->info.retiring) {
        listener->info.state = JZ_LISTENER_CLOSED;
        listener->info.cleanup_error = (struct jz_error){0};
        pthread_cond_broadcast(&listener->changed);
    }
}

static void release_slot(jz_listener *listener, size_t index)
{
    struct incoming *entry = &listener->slots[index];
    entry->peer = NULL;
    entry->state = SLOT_FREE;
    entry->next_free = listener->free_head;
    listener->free_head = index;
}

int jz_listener_admit(jz_listener *listener, struct jz_listener_ticket *ticket, jz_peer **peer,
                      struct jz_error *error)
{
    jz_error_clear(error);
    if (!listener || !ticket || !peer || *peer)
        return jz_fail(error, EINVAL, "listener-admit-output");
    pthread_mutex_lock(&listener->lock);
    int rc = 0;
    if (listener->info.state != JZ_LISTENER_LISTENING)
        rc = jz_fail(error, ECANCELED, "listener-stopped");
    else if (listener->info.pending == listener->info.max_pending)
        rc = jz_fail(error, EAGAIN, "listener-pending-limit");
    else if (listener->free_head == listener->info.capacity)
        rc = jz_fail(error, ENOSPC, "listener-result-capacity");
    else {
        size_t index = listener->free_head;
        struct incoming *entry = &listener->slots[index];
        if (entry->generation == UINT64_MAX)
            rc = jz_fail(error, EOVERFLOW, "listener-ticket");
        else {
            jz_peer *candidate = NULL;
            rc = jz_peer_create(&candidate, error);
            if (!rc) {
                listener->free_head = entry->next_free;
                ++entry->generation;
                entry->peer = candidate;
                entry->state = SLOT_PENDING;
                ++listener->info.pending;
                *ticket = (struct jz_listener_ticket){index, entry->generation};
                *peer = candidate;
            }
        }
    }
    pthread_mutex_unlock(&listener->lock);
    return rc;
}

int jz_listener_publish(jz_listener *listener, struct jz_listener_ticket ticket,
                        struct jz_error *error)
{
    jz_error_clear(error);
    if (!listener)
        return jz_fail(error, EINVAL, "listener-null");
    pthread_mutex_lock(&listener->lock);
    struct incoming *entry = lookup(listener, ticket);
    int rc = 0;
    if (!entry)
        rc = jz_fail(error, ESTALE, "listener-ticket");
    else if (entry->state != SLOT_PENDING)
        rc = jz_fail(error, EALREADY, "listener-publish-state");
    else {
        rc = listener->info.state == JZ_LISTENER_LISTENING
                 ? jz_peer_set_active(entry->peer, error)
                 : jz_fail(error, ECANCELED, "listener-stopped");
        --listener->info.pending;
        if (!rc) {
            size_t tail = (listener->head + listener->info.queued) % listener->info.capacity;
            listener->queue[tail] = ticket.slot;
            entry->state = SLOT_QUEUED;
            ++listener->info.queued;
        } else {
            jz_peer_request_close(entry->peer, NULL);
            entry->state = SLOT_RETIRING;
            ++listener->info.retiring;
        }
        pthread_cond_broadcast(&listener->changed);
    }
    pthread_mutex_unlock(&listener->lock);
    return rc;
}

int jz_listener_abort(jz_listener *listener, struct jz_listener_ticket ticket,
                      const struct jz_error *cause, struct jz_error *error)
{
    struct jz_error saved = cause ? *cause : (struct jz_error){0};
    if (cause)
        cause = &saved; /* Permit reporting a cause through the same error object. */
    jz_error_clear(error);
    if (!listener || (cause && !valid_cause(cause)))
        return jz_fail(error, EINVAL, "listener-abort-arguments");
    pthread_mutex_lock(&listener->lock);
    struct incoming *entry = lookup(listener, ticket);
    int rc = 0;
    if (!entry)
        rc = jz_fail(error, ESTALE, "listener-ticket");
    else if (entry->state != SLOT_PENDING)
        rc = jz_fail(error, EALREADY, "listener-abort-state");
    else {
        if (cause)
            jz_peer_fail(entry->peer, cause);
        else
            jz_peer_request_close(entry->peer, NULL);
        entry->state = SLOT_RETIRING;
        --listener->info.pending;
        ++listener->info.retiring;
    }
    pthread_mutex_unlock(&listener->lock);
    return rc;
}

int jz_listener_next_peer(jz_listener *listener, jz_peer **out, uint32_t timeout_ms,
                          struct jz_error *error)
{
    jz_error_clear(error);
    if (!listener || !out || *out)
        return jz_fail(error, EINVAL, "listener-peer-output");
    struct timespec deadline;
    int rc = jz_wait_deadline(timeout_ms, &deadline, error);
    if (rc)
        return rc;
    pthread_mutex_lock(&listener->lock);
    pthread_cleanup_push(jz_wait_unlock, &listener->lock);
    while (listener->info.state == JZ_LISTENER_LISTENING && !listener->info.queued) {
        int wait_rc = pthread_cond_timedwait(&listener->changed, &listener->lock, &deadline);
        if (wait_rc && (wait_rc != ETIMEDOUT || (listener->info.state == JZ_LISTENER_LISTENING &&
                                                 !listener->info.queued))) {
            rc = jz_fail(error, wait_rc, "listener-peer-wait");
            break;
        }
    }
    if (!rc && listener->info.state != JZ_LISTENER_LISTENING)
        rc = jz_fail(error, ECANCELED, "listener-stopped");
    if (!rc) {
        size_t index = listener->queue[listener->head];
        *out = listener->slots[index].peer;
        listener->head = (listener->head + 1) % listener->info.capacity;
        --listener->info.queued;
        release_slot(listener, index); /* The application now owns the handle. */
    }
    pthread_cleanup_pop(1);
    return rc;
}

int jz_listener_request_close(jz_listener *listener, struct jz_error *error)
{
    jz_error_clear(error);
    if (!listener)
        return jz_fail(error, EINVAL, "listener-null");
    pthread_mutex_lock(&listener->lock);
    if (listener->info.state == JZ_LISTENER_LISTENING) {
        listener->info.state = JZ_LISTENER_STOPPING;
        for (size_t i = 0; i < listener->info.capacity; ++i) {
            struct incoming *entry = &listener->slots[i];
            if (entry->state == SLOT_FREE)
                continue;
            jz_peer_request_close(entry->peer, NULL);
            if (entry->state == SLOT_QUEUED) {
                entry->state = SLOT_RETIRING;
                ++listener->info.retiring;
            }
            /* PENDING remains borrowed by setup; REAPING remains borrowed by
             * cleanup. Closing admission cannot revoke either live pointer. */
        }
        listener->head = 0;
        listener->info.queued = 0;
        pthread_cond_broadcast(&listener->changed);
    }
    pthread_mutex_unlock(&listener->lock);
    return 0;
}

int jz_listener_take_retiring(jz_listener *listener, struct jz_listener_ticket *ticket,
                              jz_peer **peer, struct jz_error *error)
{
    jz_error_clear(error);
    if (!listener || !ticket || !peer || *peer)
        return jz_fail(error, EINVAL, "listener-retire-output");
    pthread_mutex_lock(&listener->lock);
    int rc = jz_fail(error, EAGAIN, "listener-no-retirement");
    for (size_t n = 0; n < listener->info.capacity; ++n) {
        size_t i = (listener->retire_cursor + n) % listener->info.capacity;
        struct incoming *entry = &listener->slots[i];
        if (entry->state != SLOT_RETIRING)
            continue;
        if (entry->generation == UINT64_MAX) {
            rc = jz_fail(error, EOVERFLOW, "listener-ticket");
            break;
        }
        /* Every cleanup lease gets a fresh ticket, including retries. A late
         * report from the previous lease cannot retire the new owner's peer. */
        ++entry->generation;
        entry->state = SLOT_REAPING;
        listener->retire_cursor = (i + 1) % listener->info.capacity;
        *ticket = (struct jz_listener_ticket){i, entry->generation};
        *peer = entry->peer;
        jz_error_clear(error);
        rc = 0;
        break;
    }
    pthread_mutex_unlock(&listener->lock);
    return rc;
}

int jz_listener_retire_failed(jz_listener *listener, struct jz_listener_ticket ticket,
                              const struct jz_error *cause, struct jz_error *error)
{
    struct jz_error saved = cause ? *cause : (struct jz_error){0};
    if (cause)
        cause = &saved;
    jz_error_clear(error);
    if (!listener || !valid_cause(cause))
        return jz_fail(error, EINVAL, "listener-cleanup-cause");
    pthread_mutex_lock(&listener->lock);
    struct incoming *entry = lookup(listener, ticket);
    int rc = 0;
    if (!entry)
        rc = jz_fail(error, ESTALE, "listener-ticket");
    else if (entry->state != SLOT_REAPING)
        rc = jz_fail(error, EBUSY, "listener-retire-state");
    else {
        rc = jz_peer_set_cleanup_error(entry->peer, cause);
        if (!rc) {
            listener->info.cleanup_error = *cause;
            entry->state = SLOT_RETIRING;
        } else
            jz_fail(error, -rc, "listener-peer-cleanup-state");
    }
    pthread_mutex_unlock(&listener->lock);
    return rc;
}

int jz_listener_retire_done(jz_listener *listener, struct jz_listener_ticket ticket,
                            struct jz_error *error)
{
    jz_error_clear(error);
    if (!listener)
        return jz_fail(error, EINVAL, "listener-null");
    pthread_mutex_lock(&listener->lock);
    struct incoming *entry = lookup(listener, ticket);
    int rc = 0;
    if (!entry)
        rc = jz_fail(error, ESTALE, "listener-ticket");
    else if (entry->state != SLOT_REAPING)
        rc = jz_fail(error, EBUSY, "listener-retire-state");
    else {
        rc = jz_peer_set_closed(entry->peer, error);
        if (!rc)
            rc = jz_peer_destroy(&entry->peer, error);
        if (!rc) {
            --listener->info.retiring;
            release_slot(listener, ticket.slot);
            check_closed(listener);
        }
    }
    pthread_mutex_unlock(&listener->lock);
    return rc;
}

int jz_listener_source_closed(jz_listener *listener, struct jz_error *error)
{
    jz_error_clear(error);
    if (!listener)
        return jz_fail(error, EINVAL, "listener-null");
    pthread_mutex_lock(&listener->lock);
    int rc = 0;
    if (listener->info.state == JZ_LISTENER_LISTENING)
        rc = jz_fail(error, EBUSY, "listener-source-state");
    else {
        listener->source_closed = true;
        check_closed(listener);
    }
    pthread_mutex_unlock(&listener->lock);
    return rc;
}

int jz_listener_set_cleanup_error(jz_listener *listener, const struct jz_error *cause,
                                  struct jz_error *error)
{
    struct jz_error saved = cause ? *cause : (struct jz_error){0};
    if (cause)
        cause = &saved;
    jz_error_clear(error);
    if (!listener || !valid_cause(cause))
        return jz_fail(error, EINVAL, "listener-cleanup-cause");
    pthread_mutex_lock(&listener->lock);
    int rc = 0;
    if (listener->info.state != JZ_LISTENER_STOPPING)
        rc = jz_fail(error, EBUSY, "listener-cleanup-state");
    else
        listener->info.cleanup_error = *cause;
    pthread_mutex_unlock(&listener->lock);
    return rc;
}

int jz_listener_wait_closed(jz_listener *listener, uint32_t timeout_ms, struct jz_error *error)
{
    jz_error_clear(error);
    if (!listener)
        return jz_fail(error, EINVAL, "listener-null");
    struct timespec deadline;
    int rc = jz_wait_deadline(timeout_ms, &deadline, error);
    if (rc)
        return rc;
    pthread_mutex_lock(&listener->lock);
    pthread_cleanup_push(jz_wait_unlock, &listener->lock);
    while (listener->info.state != JZ_LISTENER_CLOSED) {
        int wait_rc = pthread_cond_timedwait(&listener->changed, &listener->lock, &deadline);
        if (wait_rc && (wait_rc != ETIMEDOUT || listener->info.state != JZ_LISTENER_CLOSED)) {
            rc = jz_fail(error, wait_rc, "listener-close-wait");
            break;
        }
    }
    pthread_cleanup_pop(1);
    return rc;
}

int jz_listener_destroy(jz_listener **handle, struct jz_error *error)
{
    jz_error_clear(error);
    if (!handle)
        return jz_fail(error, EINVAL, "listener-pointer");
    jz_listener *listener = *handle;
    if (!listener)
        return 0;
    pthread_mutex_lock(&listener->lock);
    bool closed = listener->info.state == JZ_LISTENER_CLOSED;
    pthread_mutex_unlock(&listener->lock);
    if (!closed)
        return jz_fail(error, EBUSY, "listener-destroy-state");
    pthread_cond_destroy(&listener->changed);
    pthread_mutex_destroy(&listener->lock);
    free(listener->queue);
    free(listener->slots);
    free(listener);
    *handle = NULL;
    return 0;
}
