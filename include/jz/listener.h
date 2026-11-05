#ifndef JZ_LISTENER_H
#define JZ_LISTENER_H
#include "jz/peer.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque passive admission owner. RDMA listen binds and publishes this handle;
 * these generic operations do not expose sockets or CM identifiers. */
typedef struct jz_listener jz_listener;
enum jz_listener_state {
    JZ_LISTENER_LISTENING, /* Logical admission is open. */
    JZ_LISTENER_STOPPING,  /* No new admission or delivery; unclaimed work retires. */
    JZ_LISTENER_CLOSED     /* Owner retired sources and every unclaimed candidate. */
};
struct jz_listener_info {
    enum jz_listener_state state;  /* Snapshot; does not retain resources. */
    size_t max_pending;            /* Maximum unfinished handshakes. */
    size_t capacity;               /* Shared slots: pending + queued + retiring. */
    size_t pending;                /* Handshakes still owned by control code. */
    size_t queued;                 /* Published peers not yet delivered to the application. */
    size_t retiring;               /* Unclaimed peers awaiting or undergoing cleanup. */
    struct jz_error cleanup_error; /* Latest cleanup failure; cleared at CLOSED. */
};

/* Value snapshot only. Query, next_peer, request_close and wait_closed may run
 * concurrently on a live listener. Error stage strings have static lifetime. */
int jz_listener_get_info(const jz_listener *, struct jz_listener_info *, struct jz_error *);

/* Deliver one published passive peer, transferring its handle to *out. Require
 * *out == NULL; failure leaves it unchanged. The peer may have disconnected
 * after publication. Delivery neither drives handshake nor guarantees ACTIVE.
 * timeout_ms bounds this wait only; zero polls once. -ETIMEDOUT leaves admission
 * unchanged; -ECANCELED means this listener is stopping or closed.
 * Deferred pthread cancellation unlocks the wait; asynchronous cancellation is
 * unsupported. After delivery, listener closure does not close this peer. */
int jz_listener_next_peer(jz_listener *, jz_peer **out, uint32_t timeout_ms, struct jz_error *);

/* Idempotent admission gate. Stops unclaimed peers and wakes delivery waiters,
 * but leaves outstanding setup ownership and fallible cleanup to control code.
 * Previously delivered peers are independent of the listener. */
int jz_listener_request_close(jz_listener *, struct jz_error *);
int jz_listener_wait_closed(jz_listener *, uint32_t timeout_ms, struct jz_error *);

/* CLOSED only, otherwise -EBUSY with the handle retained. Success sets *handle
 * to NULL. NULL handles are already destroyed. Exclude concurrent application
 * users and waiters. Internal control pins keep storage live until callbacks
 * return, independently of the caller's handle release. */
int jz_listener_destroy(jz_listener **handle, struct jz_error *);

#ifdef __cplusplus
}
#endif
#endif
