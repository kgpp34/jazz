#ifndef JZ_PEER_H
#define JZ_PEER_H
#include "jz/error.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A caller-owned handle to one communication session. Copying the pointer does
 * not retain it. Connection setup will publish handles in a subsequent stage;
 * this header does not provide a public constructor or imply a live QP. */
typedef struct jz_peer jz_peer;

enum jz_peer_state {
    JZ_PEER_CONNECTING, /* Protocol and path preparation are still in progress. */
    JZ_PEER_ACTIVE,     /* Activated session; individual submissions must revalidate. */
    JZ_PEER_STOPPING,   /* No new admission; setup/work/resources must be retired. */
    JZ_PEER_CLOSED      /* Owner confirmed retirement; the software handle remains. */
};

struct jz_peer_info {
    enum jz_peer_state state;       /* Value snapshot, never a use reference. */
    struct jz_error terminal_error; /* First failure; zero for an ordinary close. */
    struct jz_error cleanup_error;  /* Latest cleanup failure; cleared on CLOSED. */
};

/* Query/wait/request_close may run concurrently on a live handle. Query copies
 * a snapshot and never drives setup or acquires permission to submit a transfer.
 * Diagnostic stage strings have static storage duration. */
int jz_peer_get_info(const jz_peer *peer, struct jz_peer_info *out, struct jz_error *error);

/* timeout_ms bounds this wait only, using CLOCK_MONOTONIC. Zero polls once.
 * -ETIMEDOUT neither cancels setup nor closes the session. ACTIVE returns zero;
 * STOPPING/CLOSED return the saved failure, or -ECANCELED for ordinary close.
 * A successful wait is an observation, not a guarantee of future availability.
 * Deferred pthread cancellation releases the wait mutex and leaves state alone;
 * asynchronous pthread cancellation is outside this API's contract. */
int jz_peer_wait_ready(jz_peer *peer, uint32_t timeout_ms, struct jz_error *error);

/* Idempotently stops new admission and wakes waiters. This does not perform or
 * certify transport cleanup. The control owner must retire its resources and
 * explicitly publish CLOSED; caller threads do not poll hardware here. */
int jz_peer_request_close(jz_peer *peer, struct jz_error *error);

/* Waits for resource retirement, independently of the original session outcome.
 * CLOSED returns zero even after a connection failure; inspect terminal_error.
 * A timeout or cleanup failure never grants permission to free the handle. */
int jz_peer_wait_closed(jz_peer *peer, uint32_t timeout_ms, struct jz_error *error);

/* CLOSED only; otherwise -EBUSY, retaining *peer. NULL is already destroyed.
 * The caller must exclude every concurrent use, including waiters and control
 * owner callbacks. Success frees the handle and sets *peer to NULL. */
int jz_peer_destroy(jz_peer **peer, struct jz_error *error);

#ifdef __cplusplus
}
#endif
#endif
