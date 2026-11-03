#ifndef JAZZ_JAZZ_H
#define JAZZ_JAZZ_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Experimental S01 API: synchronous, single owner, one operation at a time.
 * All functions return 0 or a negative errno. No stable ABI is promised yet.
 * Addresses are numeric IPv4. CM and OOB peers must select the same mode.
 * Initialize output handles to NULL. Failed setup may return a non-NULL
 * handle when provider cleanup fails; retain it and retry close. */
enum jazz_mode { JAZZ_CM = 1, JAZZ_OOB = 2 };
enum jazz_access { JAZZ_REMOTE_READ = 1, JAZZ_REMOTE_WRITE = 2 };
enum { JAZZ_MESSAGE_MAX = 80, JAZZ_DESCRIPTOR_SIZE = 32 };

struct jazz_error {
    int code;
    const char *stage; /* Static string; valid after the operation returns. */
    int system_errno;
    uint32_t wc_status;
};

struct jazz_config {
    enum jazz_mode mode;
    const char *local_address; /* Required; selects the local IP and RNIC path. */
    const char *peer_address;  /* Required for connect, unused for listen. */
    const char *device;        /* Optional RNIC name; verified on both paths. */
    uint16_t port;             /* Server port; zero requests an ephemeral listener. */
    uint8_t ib_port;           /* OOB only; zero scans active Ethernet ports. */
    int gid_index;            /* OOB only; -1 selects a matching RoCEv2 GID. */
    uint32_t timeout_ms;       /* Each call has one monotonic deadline. */
};

struct jazz_remote_region {
    uint64_t address;
    uint64_t length;
    uint32_t rkey;
    uint32_t access;
    uint64_t session;
};

typedef struct jazz_listener jazz_listener;
typedef struct jazz_connection jazz_connection;
typedef struct jazz_region jazz_region;

void jazz_config_init(struct jazz_config *config);
int jazz_listen(const struct jazz_config *config, jazz_listener **out,
                struct jazz_error *error);
uint16_t jazz_listener_port(const jazz_listener *listener);
int jazz_accept(jazz_listener *listener, jazz_connection **out,
                struct jazz_error *error);
int jazz_connect(const struct jazz_config *config, jazz_connection **out,
                 struct jazz_error *error);
int jazz_listener_close(jazz_listener **listener, struct jazz_error *error);

/* Quiesce destroys the QP before releasing any DMA references. On failure,
 * retain registered buffers and retry cleanup. A quiesced connection is terminal.
 * Close quiesces, then returns EBUSY until all regions have been unregistered. */
int jazz_connection_quiesce(jazz_connection *connection, struct jazz_error *error);
int jazz_connection_close(jazz_connection **connection, struct jazz_error *error);

/* The caller owns the buffer. Keep it alive until unregister succeeds.
 * Regions are tied to this connection/PD; there is no registration cache yet.
 * Remotely accessible regions require successful quiesce before unregister,
 * since local CQ state cannot prove the peer has stopped using their rkey. */
int jazz_region_register(jazz_connection *connection, void *buffer, size_t length,
                         unsigned access, jazz_region **out,
                         struct jazz_error *error);
int jazz_region_unregister(jazz_region **region, struct jazz_error *error);
int jazz_region_export(const jazz_region *region, struct jazz_remote_region *out,
                       struct jazz_error *error);
int jazz_descriptor_encode(const struct jazz_remote_region *region,
                           uint8_t out[JAZZ_DESCRIPTOR_SIZE]);
int jazz_descriptor_decode(const uint8_t *bytes, size_t length,
                           struct jazz_remote_region *out);

/* One receive is preposted during setup and replenished after consumption.
 * These bounded control messages are separate from the RDMA payload. */
int jazz_send_message(jazz_connection *connection, const void *data, size_t length,
                      struct jazz_error *error);
int jazz_receive_message(jazz_connection *connection, void *data, size_t capacity,
                         size_t *length, struct jazz_error *error);

/* A successful call retires the local WR. It does not acknowledge remote
 * application consumption. A timed out operation is never automatically replayed.
 * S01 supports one WR per call (up to the device's max message size). */
int jazz_write(jazz_connection *connection, jazz_region *local, size_t local_offset,
               const struct jazz_remote_region *remote, uint64_t remote_offset,
               size_t length, struct jazz_error *error);
int jazz_read(jazz_connection *connection, jazz_region *local, size_t local_offset,
              const struct jazz_remote_region *remote, uint64_t remote_offset,
              size_t length, struct jazz_error *error);

#ifdef __cplusplus
}
#endif
#endif
