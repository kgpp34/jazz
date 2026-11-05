#ifndef JZ_LISTENER_INTERNAL_H
#define JZ_LISTENER_INTERNAL_H
#include "jz/listener.h"

/* Slot identity is local admission bookkeeping, not a network session ID.
 * Generation prevents a delayed callback from modifying a reused slot or a
 * later cleanup lease. Taking retirement ownership invalidates the setup ticket. */
struct jz_listener_ticket {
    size_t slot;
    uint64_t generation;
};

/* Allocate bounded bookkeeping only; no sockets, CM IDs, QPs or threads. Both
 * limits are explicit and nonzero. max_pending may exceed capacity, but every
 * admission must satisfy both limits. Output remains unchanged on failure. */
int jz_listener_create(jz_listener **out, size_t max_pending, size_t capacity, struct jz_error *);

/* Reserve result capacity before creating a candidate. *peer must be NULL.
 * The returned peer is BORROWED by the setup owner, not an application handle.
 * The owner must publish or abort exactly once before relinquishing this lease.
 * request_close cannot recycle a slot while that setup lease is outstanding. */
int jz_listener_admit(jz_listener *, struct jz_listener_ticket *, jz_peer **peer,
                      struct jz_error *);

/* Publish after real setup is ready. A valid PENDING candidate always ends its
 * setup lease here: it is queued on success, or retained for retirement on a
 * close/readiness race. Do not access the borrowed peer after either result.
 * STALE/duplicate calls have no effect and do not terminate some newer lease. */
int jz_listener_publish(jz_listener *, struct jz_listener_ticket, struct jz_error *);
int jz_listener_abort(jz_listener *, struct jz_listener_ticket, const struct jz_error *cause,
                      struct jz_error *);

/* Take exclusive retirement ownership. Cleanup happens OUTSIDE the listener
 * lock. Report failure to requeue without releasing its capacity; report done
 * only after transport resources/events/callbacks are retired. The returned
 * peer is borrowed until retire_failed/done; never destroy it directly. */
int jz_listener_take_retiring(jz_listener *, struct jz_listener_ticket *, jz_peer **,
                              struct jz_error *);
int jz_listener_retire_failed(jz_listener *, struct jz_listener_ticket,
                              const struct jz_error *cause, struct jz_error *);
int jz_listener_retire_done(jz_listener *, struct jz_listener_ticket, struct jz_error *);

/* Confirm that listen sources and their callbacks are retired. Delivered peers
 * must retain any shared event channels independently. This is owner evidence,
 * not an implementation of socket/CM teardown. Cleanup failures retain STOPPING. */
int jz_listener_source_closed(jz_listener *, struct jz_error *);
int jz_listener_set_cleanup_error(jz_listener *, const struct jz_error *cause, struct jz_error *);
#endif
