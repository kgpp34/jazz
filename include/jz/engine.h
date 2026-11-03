#ifndef JZ_ENGINE_H
#define JZ_ENGINE_H
#include <stddef.h>
#include <stdint.h>
#include "jz/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jz_engine jz_engine;
enum { JZ_RDMA_DEVICE_NAME_MAX = 64, JZ_RDMA_DEVICE_LIMIT = 64 };
enum jz_rdma_setup { JZ_RDMA_SETUP_CM = 1, JZ_RDMA_SETUP_OOB = 2 };
enum jz_engine_state {
    JZ_ENGINE_CREATED,
    JZ_ENGINE_PREPARING,
    JZ_ENGINE_READY,
    JZ_ENGINE_CLEANUP_REQUIRED
};

struct jz_engine_config {
    unsigned setup_modes; /* Default: CM | OOB. OOB-only does not use the CM context provider. */
    const char *const *devices;
    size_t device_count; /* Explicit device set, 1..DEVICE_LIMIT. Duplicate names are rejected. */
};

/* Owned value snapshots; no ibv_context or PD pointers are exposed.
 * READY means device resources are prepared, not that a peer is connected. */
struct jz_rdma_device_info {
    char name[JZ_RDMA_DEVICE_NAME_MAX];
    uint64_t guid;
    uint64_t max_mr_size;
    uint32_t max_qp_wr;
    uint32_t max_cqe;
    uint32_t max_sge;
    uint32_t max_read_outstanding;
    uint32_t max_read_responder;
    uint8_t port_count;
};

struct jz_rdma_port_info {
    uint8_t port;
    uint8_t active;
    uint8_t ethernet;
    uint32_t active_mtu; /* Bytes, not an ibv_mtu enumeration. */
    uint32_t max_message_size;
    uint32_t roce_v2_gid_count;
};

/* Serialize all API and lifecycle calls in this stage.
 * There are no background threads, MRs or QPs; the ABI is experimental.
 * Initialize the output handle to NULL before create. Configuration and names
 * are copied; create does not open devices. Prepare cannot be repeated or silently degraded.
 * On prepare failure, error preserves the first preparation error; state reports pending cleanup.
 * Destroy preserves the handle and resources on failure so cleanup can be retried;
 * it sets *engine=NULL only on success. Copying a pointer does not extend its lifetime. */
void jz_engine_config_init(struct jz_engine_config *config);
int jz_engine_create(const struct jz_engine_config *config,
                       jz_engine **out, struct jz_error *error);
int jz_engine_prepare(jz_engine *engine, struct jz_error *error);
int jz_engine_get_state(const jz_engine *engine, enum jz_engine_state *out);
int jz_rdma_query_device(const jz_engine *engine, size_t index,
                             struct jz_rdma_device_info *out, struct jz_error *error);
int jz_rdma_query_port(const jz_engine *engine, size_t device_index,
                           uint8_t port, struct jz_rdma_port_info *out,
                           struct jz_error *error);
int jz_engine_destroy(jz_engine **engine, struct jz_error *error);

#ifdef __cplusplus
}
#endif
#endif
