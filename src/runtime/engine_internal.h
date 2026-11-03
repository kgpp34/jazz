#ifndef JZ_ENGINE_INTERNAL_H
#define JZ_ENGINE_INTERNAL_H
#include "jz/engine.h"
#include <infiniband/verbs.h>
#include <stdbool.h>

/* Narrow test seam for device operations used in this stage; it does not simulate
 * data transfers. Preserve verbs/CM return conventions. Production uses jz_rdma_system_ops. */
struct jz_rdma_device_ops {
    struct ibv_context **(*cm_list)(int *count);
    void (*cm_free)(struct ibv_context **list);
    struct ibv_device **(*verbs_list)(int *count);
    void (*verbs_free)(struct ibv_device **list);
    const char *(*name)(struct ibv_device *device);
    struct ibv_context *(*open)(struct ibv_device *device);
    int (*close)(struct ibv_context *context);
    int (*query_device)(struct ibv_context *, struct ibv_device_attr *);
    int (*query_port)(struct ibv_context *, uint8_t, struct ibv_port_attr *);
    int (*query_gid)(struct ibv_context *, uint32_t, uint32_t,
                     struct ibv_gid_entry *, uint32_t);
    struct ibv_pd *(*alloc_pd)(struct ibv_context *);
    int (*dealloc_pd)(struct ibv_pd *);
};

struct jz_rdma_device {
    char name[JZ_RDMA_DEVICE_NAME_MAX];
    struct ibv_context *verbs;
    struct ibv_pd *pd; /* Engine-owned; never borrow the CM default PD. */
    bool owned_context;
    struct jz_rdma_device_info info;
    struct jz_rdma_port_info *ports;
};

struct jz_engine {
    enum jz_engine_state state;
    unsigned setup_modes;
    size_t device_count;
    struct jz_rdma_device *devices;
    const struct jz_rdma_device_ops *ops;
    /* The CM list pins borrowed contexts until all engine-owned PDs are released. */
    struct ibv_context **cm_contexts;
    struct ibv_device **verbs_devices;
};

extern const struct jz_rdma_device_ops jz_rdma_system_ops;
int jz_engine_fail(struct jz_error *error, int errnum, const char *stage);
int jz_engine_create_with_ops(const struct jz_engine_config *config,
                      const struct jz_rdma_device_ops *ops,
                      jz_engine **out, struct jz_error *error);
int jz_rdma_devices_prepare(jz_engine *engine, struct jz_error *error);
int jz_rdma_devices_release(jz_engine *engine, struct jz_error *error);
#endif
