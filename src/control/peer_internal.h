#ifndef JZ_PEER_INTERNAL_H
#define JZ_PEER_INTERNAL_H
#include "jz/peer.h"

/* Private control-owner entry points, not a second public creation API. Creation
 * publishes CONNECTING only after synchronization primitives are initialized.
 * No Engine, QP, thread, network connection, or remote view is created here. */
int jz_peer_create(jz_peer **out, struct jz_error *error);
/* Control ownership outlives public handle release during a CLOSED publication.
 * Retain only an already-live reference; release never performs hardware cleanup. */
void jz_peer_retain(jz_peer *);
void jz_peer_release(jz_peer *);

/* Publish protocol/path readiness only while CONNECTING. The same lock used by
 * request_close prevents late readiness from reviving a stopping session. */
int jz_peer_set_active(jz_peer *peer, struct jz_error *error);

/* Record the first session failure and begin stopping. The diagnostic is copied;
 * its stage must have static storage duration. Existing failure is preserved. */
int jz_peer_fail(jz_peer *peer, const struct jz_error *cause);

/* Cleanup failures retain STOPPING. A later successful retirement clears this
 * diagnostic. The owner is responsible for keeping all live resources reachable. */
int jz_peer_set_cleanup_error(jz_peer *peer, const struct jz_error *cause);

/* STOPPING only. The owner must already have retired all session resources,
 * hardware work and callbacks that could outlive the handle. This transition
 * does not discover, destroy, drain, or invalidate transport resources itself. */
int jz_peer_set_closed(jz_peer *peer, struct jz_error *error);
#endif
