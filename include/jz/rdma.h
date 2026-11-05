#ifndef JZ_RDMA_H
#define JZ_RDMA_H
#include "jz/engine.h"
#include "jz/listener.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { JZ_RDMA_DEVICE_NAME_MAX = 64, JZ_RDMA_DEVICE_LIMIT = 64 };
enum jz_rdma_setup { JZ_RDMA_SETUP_CM = 1, JZ_RDMA_SETUP_OOB = 2 };
enum jz_rdma_registration_policy {
    JZ_RDMA_REG_AUTO,    /* Parallel registration after large HOST pre-touch. */
    JZ_RDMA_REG_SERIAL,  /* No helper threads for registration or cleanup. */
    JZ_RDMA_REG_PARALLEL /* Permit helpers; unavailable slots run in the caller. */
};
struct jz_rdma_config {
    unsigned setup_modes;       /* Enabled setup modes, CM | OOB by default. CM uses
                                 * retained librdmacm contexts and shared device PDs. */
    const char *const *devices; /* Explicit device names, copied during configure. */
    size_t device_count;        /* 1..DEVICE_LIMIT; duplicate names are invalid. */
    size_t mr_chunk_bytes;      /* Optional MR chunk ceiling; zero uses device limits. */
    enum jz_rdma_registration_policy registration_policy; /* Default AUTO. */
    unsigned max_registration_threads; /* Shared helper budget, default 8; 0 disables helpers. */
    bool pretouch;                     /* Default true; temporarily register HOST chunks first. */
    uint64_t pretouch_threshold_bytes; /* Default 4 GiB; 0 forces eligible small tests.
                                        * AUTO additionally requires at least four CPUs. */
    size_t max_connections;            /* Per-Engine live control/endpoint slots; default 128. */
    size_t max_listeners;              /* Per-Engine bound listen sources; default 8. */
};

/* Numeric IPs only. One endpoint currently contains one RC QP; this is not an
 * architectural limit on future QP pools. Strings are consumed before return. */
struct jz_rdma_listen_options {
    const char *local_ip;     /* Required concrete IP belonging to the selected device. */
    uint16_t service_port;    /* Zero requests an ephemeral port. */
    enum jz_rdma_setup setup; /* Exactly CM or OOB, enabled in Engine configuration. */
    size_t device_index;      /* Index in configured device order. */
    uint8_t port;             /* RDMA port, default 1. */
    int gid_index;            /* -1 selects a typed RoCEv2 GID matching local_ip. */
    uint32_t timeout_ms;      /* Total handshake deadline, default 5000. */
    size_t max_pending;       /* Handshake quota, default 16. */
    size_t capacity;          /* Unclaimed/retiring result quota, default 32. */
};
struct jz_rdma_connect_options {
    const char *local_ip;     /* Required concrete source IP, never an arbitrary GID. */
    const char *remote_ip;    /* Numeric destination IP, same address family as local_ip. */
    uint16_t service_port;    /* Destination's CM or OOB service port; nonzero. */
    enum jz_rdma_setup setup; /* Exactly one enabled mode. */
    size_t device_index;      /* Configured local device selector, default 0. */
    uint8_t port;             /* Local RDMA port, default 1. */
    int gid_index;            /* Typed local RoCEv2 GID selector; -1 matches local_ip. */
    uint32_t timeout_ms;      /* Total setup budget, independent of wait_ready timeout. */
};
struct jz_rdma_bound_address {
    char ip[64]; /* Numeric address, copied value. */
    uint16_t service_port;
    enum jz_rdma_setup setup;
};
struct jz_rdma_connection_info {
    enum jz_rdma_setup setup;
    size_t device_index;
    uint32_t local_qpn;
    uint32_t remote_qpn;
    uint32_t mtu;                /* Negotiated bytes. */
    uint8_t local_instance[16];  /* Per-Engine control-service identity, not authentication. */
    uint8_t remote_instance[16]; /* Instance negotiated by this session. */
    uint64_t attempt;            /* Initiator-local unique proposal. */
    uint64_t session;            /* Responder-local unique acceptance. */
};

void jz_rdma_listen_options_init(struct jz_rdma_listen_options *);
void jz_rdma_connect_options_init(struct jz_rdma_connect_options *);
/* READY Engine only. Listen returns a genuinely bound source. Connect returns
 * an admitted CONNECTING handle; background control drives both setup modes.
 * Require *out == NULL. Failed setup after publication is reported by the Peer.
 * Different connect calls create independent sessions, including opposite
 * directions; connection-cache coalescing is not provided in this version. */
int jz_rdma_listen(jz_engine *, const struct jz_rdma_listen_options *, jz_listener **out,
                   struct jz_error *);
int jz_rdma_connect(jz_engine *, const struct jz_rdma_connect_options *, jz_peer **out,
                    struct jz_error *);
int jz_rdma_listener_address(jz_engine *, const jz_listener *, struct jz_rdma_bound_address *,
                             struct jz_error *);
/* Snapshot of a live, protocol-ready endpoint; does not retain a QP. */
int jz_rdma_peer_query(jz_engine *, const jz_peer *, struct jz_rdma_connection_info *,
                       struct jz_error *);

/* Value snapshots taken at prepare; do not expose provider-owned pointers.
 * Capability limits are ceilings, not a promise that combined allocations fit. */
struct jz_rdma_device_info {
    char name[JZ_RDMA_DEVICE_NAME_MAX]; /* Provider name, NUL terminated. */
    uint64_t guid;                      /* Device node GUID in host byte order. */
    uint64_t max_mr_size;               /* Maximum bytes in one registered region. */
    uint32_t max_qp_wr;                 /* Maximum work requests per QP queue. */
    uint32_t max_cqe;                   /* Maximum completion entries per CQ. */
    uint32_t max_sge;                   /* General per-WR scatter/gather ceiling;
                                         * READ-specific limits are not represented. */
    uint32_t max_read_outstanding;      /* Initiator READ/atomic ceiling per QP. */
    uint32_t max_read_responder;        /* Incoming READ/atomic ceiling per QP. */
    uint8_t port_count;                 /* Physical ports; public indices start at 1. */
};
struct jz_rdma_port_info {
    uint8_t port;               /* Physical port number, starting at 1. */
    uint8_t active;             /* Port state was ACTIVE at prepare time. */
    uint8_t ethernet;           /* Ethernet link layer, not proof of RoCEv2. */
    uint32_t active_mtu;        /* Bytes; zero if the provider enum is unknown. */
    uint32_t max_message_size;  /* Provider's maximum message size in bytes. */
    uint32_t roce_v2_gid_count; /* Nonzero typed RoCEv2 GIDs; not a chosen route. */
};

void jz_rdma_config_init(struct jz_rdma_config *config);
/* CREATED only. Validates and copies configuration without opening devices.
 * Only one RDMA backend may be attached; failure leaves the engine unchanged. */
int jz_rdma_configure(jz_engine *engine, const struct jz_rdma_config *config,
                      struct jz_error *error);
/* READY only. index follows the configured device order; port starts at 1.
 * Writes an independent snapshot to *out only on success. */
int jz_rdma_query_device(const jz_engine *engine, size_t index, struct jz_rdma_device_info *out,
                         struct jz_error *error);
int jz_rdma_query_port(const jz_engine *engine, size_t device_index, uint8_t port,
                       struct jz_rdma_port_info *out, struct jz_error *error);

#ifdef __cplusplus
}
#endif
#endif
