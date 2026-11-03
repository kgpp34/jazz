#ifndef JZ_PROTOTYPE_H
#define JZ_PROTOTYPE_H

#include <stddef.h>
#include <stdint.h>
#include "jz/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public API for the historical synchronous prototype; verbs objects remain internal.
 * Each connection has one owner and executes one operation at a time. There is no
 * background progress thread or asynchronous batch API, and the ABI is experimental.
 * Integer operations return zero on success or a negative errno on failure.
 * Addresses must be numeric IPv4; both peers must explicitly choose the same CM/OOB mode.
 * Initialize output handles to NULL. Creation normally cleans up on failure, but may
 * retain a non-NULL handle if an underlying resource cannot be destroyed. Such a handle
 * is for cleanup only. Do not discard it, because it owns resources that remain live. */
enum jz_mode { JZ_CM = 1, JZ_OOB = 2 };
enum jz_access { JZ_REMOTE_READ = 1, JZ_REMOTE_WRITE = 2 };
enum { JZ_MESSAGE_MAX = 80, JZ_DESCRIPTOR_SIZE = 32 };

/* Listen/connect copy the configuration; its strings are not retained.
 * port is the server listening port; ib_port is the RNIC port. */
struct jz_config {
    enum jz_mode mode;
    const char *local_address; /* Required; selects the local IP and RNIC path. */
    const char *peer_address;  /* Required for connect, unused for listen. */
    const char *device;        /* Optional RNIC name; verified on both paths. */
    uint16_t port;             /* Server port; zero requests an ephemeral listener. */
    uint8_t ib_port;           /* OOB only; zero scans active Ethernet ports. */
    int gid_index;            /* OOB only; -1 selects a matching RoCEv2 GID. */
    uint32_t timeout_ms;       /* Connection/completion deadline; timeout does not prove DMA has stopped. */
};

/* Remote memory metadata, not a locally dereferenceable pointer.
 * access/session support software checks; QP/PD/MR/rkey enforce device access.
 * session identifies a connection, not an MR generation or authenticated identity.
 * Use encode/decode across processes; never transmit this C struct verbatim. */
struct jz_remote_region {
    uint64_t address;
    uint64_t length;
    uint32_t rkey;
    uint32_t access;
    uint64_t session;
};

/* Opaque handles; layouts are in src/rdma/internal.h.
 * The listener accepts connections, a connection manages its path, and a region
 * manages a memory registration. Release them through the API rather than free(). */
typedef struct jz_listener jz_listener;
typedef struct jz_connection jz_connection;
typedef struct jz_region jz_region;

/* Initialize configuration before overriding fields; an all-zero configuration is invalid. */
void jz_config_init(struct jz_config *config);
int jz_listen(const struct jz_config *config, jz_listener **out,
                struct jz_error *error);
/* Actual listening port when port=0 was requested; a NULL listener yields zero. */
uint16_t jz_listener_port(const jz_listener *listener);
/* An accepted connection outlives the listener independently. CM children have
 * separate event channels; the temporary OOB TCP socket closes after the handshake. */
int jz_accept(jz_listener *listener, jz_connection **out,
                struct jz_error *error);
int jz_connect(const struct jz_config *config, jz_connection **out,
                 struct jz_error *error);
int jz_listener_close(jz_listener **listener, struct jz_error *error);

/* Quiesce is terminal: disable the connection, destroy its QP, and only then drop
 * local in-flight references. If QP destruction fails, retain memory and retry cleanup.
 * Close quiesces first, then returns -EBUSY if user regions remain. Therefore a failed
 * close does not imply the connection remains usable. Cleanup order:
 * quiesce -> region_unregister -> free the user buffer -> close. */
int jz_connection_quiesce(jz_connection *connection, struct jz_error *error);
int jz_connection_close(jz_connection **connection, struct jz_error *error);

/* The caller owns the buffer; the region owns registration metadata and its MR.
 * Registration does not copy payload, and unregister does not free the buffer. Keep
 * the buffer valid until unregister succeeds. This prototype binds MRs to a connection
 * PD and has no registration cache. Remotely accessible regions require quiescence:
 * no local pending request does not prove remote rkey users have stopped.
 * Dynamic revocation needs a separate protocol. */
int jz_region_register(jz_connection *connection, void *buffer, size_t length,
                         unsigned access, jz_region **out,
                         struct jz_error *error);
int jz_region_unregister(jz_region **region, struct jz_error *error);
int jz_region_export(const jz_region *region, struct jz_remote_region *out,
                       struct jz_error *error);
int jz_descriptor_encode(const struct jz_remote_region *region,
                           uint8_t out[JZ_DESCRIPTOR_SIZE]);
int jz_descriptor_decode(const uint8_t *bytes, size_t length,
                           struct jz_remote_region *out);

/* Prepost one control receive at connection setup and replenish it after consumption.
 * Control buffers are separate from payload. receive_message requires capacity >=
 * JZ_MESSAGE_MAX even for shorter messages. Applications define message semantics,
 * such as region exchange; this is not a KV cache protocol. */
int jz_send_message(jz_connection *connection, const void *data, size_t length,
                      struct jz_error *error);
int jz_receive_message(jz_connection *connection, void *data, size_t capacity,
                         size_t *length, struct jz_error *error);

/* Success means the local WR completed, not that the remote application consumed
 * or validated the data. Each call posts one signaled WR within the device size limit.
 * There is no slicing or automatic replay: a timeout leaves the remote outcome uncertain. */
int jz_write(jz_connection *connection, jz_region *local, size_t local_offset,
               const struct jz_remote_region *remote, uint64_t remote_offset,
               size_t length, struct jz_error *error);
int jz_read(jz_connection *connection, jz_region *local, size_t local_offset,
              const struct jz_remote_region *remote, uint64_t remote_offset,
              size_t length, struct jz_error *error);

#ifdef __cplusplus
}
#endif
#endif
