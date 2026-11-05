#include "control/peer_internal.h"
#include "common/wait.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>

struct jz_peer {
    pthread_mutex_t lock;     /* Protects the entire info snapshot and wait predicates. */
    pthread_cond_t changed;   /* Monotonic timed waits; broadcast on state publication. */
    struct jz_peer_info info; /* Independent outcome, cleanup diagnostic and state. */
};

/* The API takes a const handle for queries; synchronization still mutates the
 * mutex. All instances originate from the mutable private constructor. */
static struct jz_peer *mutable_peer(const jz_peer *peer)
{
    return (struct jz_peer *)peer;
}

int jz_peer_create(jz_peer **out, struct jz_error *error)
{
    jz_error_clear(error);
    if (!out || *out)
        return jz_fail(error, EINVAL, "peer-output");
    jz_peer *peer = calloc(1, sizeof(*peer));
    if (!peer)
        return jz_fail(error, ENOMEM, "peer-allocate");
    int rc = pthread_mutex_init(&peer->lock, NULL);
    if (rc) {
        free(peer);
        return jz_fail(error, rc, "peer-mutex-init");
    }
    pthread_condattr_t attr;
    rc = pthread_condattr_init(&attr);
    if (rc) {
        pthread_mutex_destroy(&peer->lock);
        free(peer);
        return jz_fail(error, rc, "peer-condattr-init");
    }
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!rc)
        rc = pthread_cond_init(&peer->changed, &attr);
    pthread_condattr_destroy(&attr);
    if (rc) {
        pthread_mutex_destroy(&peer->lock);
        free(peer);
        return jz_fail(error, rc, "peer-condition-init");
    }
    peer->info.state = JZ_PEER_CONNECTING;
    *out = peer;
    return 0;
}

int jz_peer_get_info(const jz_peer *peer, struct jz_peer_info *out, struct jz_error *error)
{
    jz_error_clear(error);
    if (!peer || !out)
        return jz_fail(error, EINVAL, "peer-query");
    pthread_mutex_lock(&mutable_peer(peer)->lock);
    *out = peer->info;
    pthread_mutex_unlock(&mutable_peer(peer)->lock);
    return 0;
}

static bool valid_cause(const struct jz_error *cause)
{
    return cause && cause->code < 0 && cause->stage;
}

int jz_peer_set_active(jz_peer *peer, struct jz_error *error)
{
    jz_error_clear(error);
    if (!peer)
        return jz_fail(error, EINVAL, "peer-null");
    pthread_mutex_lock(&peer->lock);
    int rc = 0;
    if (peer->info.state == JZ_PEER_CONNECTING) {
        peer->info.state = JZ_PEER_ACTIVE;
        pthread_cond_broadcast(&peer->changed);
    } else {
        rc = jz_fail(error, peer->info.state == JZ_PEER_ACTIVE ? EALREADY : ECANCELED,
                     "peer-activate-state");
    }
    pthread_mutex_unlock(&peer->lock);
    return rc;
}

int jz_peer_request_close(jz_peer *peer, struct jz_error *error)
{
    jz_error_clear(error);
    if (!peer)
        return jz_fail(error, EINVAL, "peer-null");
    pthread_mutex_lock(&peer->lock);
    if (peer->info.state == JZ_PEER_CONNECTING || peer->info.state == JZ_PEER_ACTIVE) {
        peer->info.state = JZ_PEER_STOPPING;
        pthread_cond_broadcast(&peer->changed);
    }
    pthread_mutex_unlock(&peer->lock);
    return 0;
}

int jz_peer_fail(jz_peer *peer, const struct jz_error *cause)
{
    if (!peer || !valid_cause(cause))
        return -EINVAL;
    pthread_mutex_lock(&peer->lock);
    int rc = 0;
    if (peer->info.state == JZ_PEER_CLOSED) {
        rc = -EALREADY;
    } else {
        if (!peer->info.terminal_error.code)
            peer->info.terminal_error = *cause;
        peer->info.state = JZ_PEER_STOPPING;
        pthread_cond_broadcast(&peer->changed);
    }
    pthread_mutex_unlock(&peer->lock);
    return rc;
}

int jz_peer_set_cleanup_error(jz_peer *peer, const struct jz_error *cause)
{
    if (!peer || !valid_cause(cause))
        return -EINVAL;
    pthread_mutex_lock(&peer->lock);
    int rc = -EBUSY;
    if (peer->info.state == JZ_PEER_STOPPING) {
        peer->info.cleanup_error = *cause;
        rc = 0;
    }
    pthread_mutex_unlock(&peer->lock);
    return rc;
}

int jz_peer_set_closed(jz_peer *peer, struct jz_error *error)
{
    jz_error_clear(error);
    if (!peer)
        return jz_fail(error, EINVAL, "peer-null");
    pthread_mutex_lock(&peer->lock);
    int rc = 0;
    if (peer->info.state == JZ_PEER_STOPPING) {
        peer->info.cleanup_error = (struct jz_error){0};
        peer->info.state = JZ_PEER_CLOSED;
        pthread_cond_broadcast(&peer->changed);
    } else if (peer->info.state != JZ_PEER_CLOSED) {
        rc = jz_fail(error, EBUSY, "peer-close-state");
    }
    pthread_mutex_unlock(&peer->lock);
    return rc;
}

static bool wait_pending(const jz_peer *peer, bool closing)
{
    return closing ? peer->info.state != JZ_PEER_CLOSED : peer->info.state == JZ_PEER_CONNECTING;
}

static int wait_peer(jz_peer *peer, uint32_t timeout_ms, bool closing, struct jz_error *error)
{
    jz_error_clear(error);
    if (!peer)
        return jz_fail(error, EINVAL, "peer-null");
    struct timespec deadline;
    int rc = jz_wait_deadline(timeout_ms, &deadline, error);
    if (rc)
        return rc;
    pthread_mutex_lock(&peer->lock);
    pthread_cleanup_push(jz_wait_unlock, &peer->lock);
    while (wait_pending(peer, closing)) {
        int wait_rc = pthread_cond_timedwait(&peer->changed, &peer->lock, &deadline);
        /* Timeout can race with a state publication. Recheck the predicate
         * while still holding the mutex before reporting a timeout. */
        if (wait_rc && (wait_rc != ETIMEDOUT || wait_pending(peer, closing))) {
            rc = jz_fail(error, wait_rc, "peer-wait");
            break;
        }
    }
    if (!rc && !closing && peer->info.state != JZ_PEER_ACTIVE) {
        if (peer->info.terminal_error.code) {
            rc = peer->info.terminal_error.code;
            if (error)
                *error = peer->info.terminal_error;
        } else {
            rc = jz_fail(error, ECANCELED, "peer-stopped");
        }
    }
    pthread_cleanup_pop(1);
    return rc;
}

int jz_peer_wait_ready(jz_peer *peer, uint32_t timeout_ms, struct jz_error *error)
{
    return wait_peer(peer, timeout_ms, false, error);
}

int jz_peer_wait_closed(jz_peer *peer, uint32_t timeout_ms, struct jz_error *error)
{
    return wait_peer(peer, timeout_ms, true, error);
}

int jz_peer_destroy(jz_peer **handle, struct jz_error *error)
{
    jz_error_clear(error);
    if (!handle)
        return jz_fail(error, EINVAL, "peer-pointer");
    jz_peer *peer = *handle;
    if (!peer)
        return 0;
    /* External exclusion is required; the lock makes the state check explicit,
     * but cannot protect a raw pointer against concurrent destruction. */
    pthread_mutex_lock(&peer->lock);
    bool closed = peer->info.state == JZ_PEER_CLOSED;
    pthread_mutex_unlock(&peer->lock);
    if (!closed)
        return jz_fail(error, EBUSY, "peer-destroy-state");
    pthread_cond_destroy(&peer->changed);
    pthread_mutex_destroy(&peer->lock);
    free(peer);
    *handle = NULL;
    return 0;
}
