#ifndef JZ_RDMA_H
#define JZ_RDMA_H
#include "jz/engine.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { JZ_RDMA_DEVICE_NAME_MAX = 64, JZ_RDMA_DEVICE_LIMIT = 64 };
enum jz_rdma_setup { JZ_RDMA_SETUP_CM = 1, JZ_RDMA_SETUP_OOB = 2 };
struct jz_rdma_config {
    unsigned setup_modes;       /* CM | OOB by default; selects context ownership now.
                                 * Connection establishment is a subsequent stage. */
    const char *const *devices;  /* Explicit device names, copied during configure. */
    size_t device_count;        /* 1..DEVICE_LIMIT; duplicate names are invalid. */
};

/* Value snapshots taken at prepare; do not expose provider-owned pointers.
 * Capability limits are ceilings, not a promise that combined allocations fit. */
struct jz_rdma_device_info {
    char name[JZ_RDMA_DEVICE_NAME_MAX]; /* Provider name, NUL terminated. */
    uint64_t guid;                     /* Device node GUID in host byte order. */
    uint64_t max_mr_size;              /* Maximum bytes in one registered region. */
    uint32_t max_qp_wr;                /* Maximum work requests per QP queue. */
    uint32_t max_cqe;                  /* Maximum completion entries per CQ. */
    uint32_t max_sge;                  /* General per-WR scatter/gather ceiling;
                                       * READ-specific limits are not represented. */
    uint32_t max_read_outstanding;     /* Initiator READ/atomic ceiling per QP. */
    uint32_t max_read_responder;       /* Incoming READ/atomic ceiling per QP. */
    uint8_t port_count;                /* Physical ports; public indices start at 1. */
};
struct jz_rdma_port_info {
    uint8_t port;                  /* Physical port number, starting at 1. */
    uint8_t active;                /* Port state was ACTIVE at prepare time. */
    uint8_t ethernet;              /* Ethernet link layer, not proof of RoCEv2. */
    uint32_t active_mtu;           /* Bytes; zero if the provider enum is unknown. */
    uint32_t max_message_size;     /* Provider's maximum message size in bytes. */
    uint32_t roce_v2_gid_count;    /* Nonzero typed RoCEv2 GIDs; not a chosen route. */
};

void jz_rdma_config_init(struct jz_rdma_config *config);
/* CREATED only. Validates and copies configuration without opening devices.
 * Only one RDMA backend may be attached; failure leaves the engine unchanged. */
int jz_rdma_configure(jz_engine *engine, const struct jz_rdma_config *config,
                      struct jz_error *error);
/* READY only. index follows the configured device order; port starts at 1.
 * Writes an independent snapshot to *out only on success. */
int jz_rdma_query_device(const jz_engine *engine, size_t index,
                         struct jz_rdma_device_info *out, struct jz_error *error);
int jz_rdma_query_port(const jz_engine *engine, size_t device_index, uint8_t port,
                       struct jz_rdma_port_info *out, struct jz_error *error);

#ifdef __cplusplus
}
#endif
#endif
