#ifndef JZ_RDMA_INTERNAL_H
#define JZ_RDMA_INTERNAL_H
#include "jz/rdma.h"
#include "transport/backend.h"
#include <infiniband/verbs.h>

/* Raw provider ABI, replaceable only through the private test constructor.
 * The driver below converts raw errno conventions into the library contract. */
struct jz_rdma_calls {
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
struct jz_rdma_driver {
    const struct jz_rdma_calls *calls; /* Immutable ABI table; outlives backend. */
};
extern const struct jz_rdma_calls jz_rdma_system_calls;
int jz_rdma_driver_cm_list(const struct jz_rdma_driver *, struct ibv_context ***, int *, struct jz_error *);
int jz_rdma_driver_verbs_list(const struct jz_rdma_driver *, struct ibv_device ***, int *, struct jz_error *);
int jz_rdma_driver_open(const struct jz_rdma_driver *, struct ibv_device *, struct ibv_context **, struct jz_error *);
int jz_rdma_driver_close(const struct jz_rdma_driver *, struct ibv_context *, struct jz_error *);
int jz_rdma_driver_query_device(const struct jz_rdma_driver *, struct ibv_context *, struct ibv_device_attr *, struct jz_error *);
int jz_rdma_driver_query_port(const struct jz_rdma_driver *, struct ibv_context *, uint8_t, struct ibv_port_attr *, struct jz_error *);
int jz_rdma_driver_query_gid(const struct jz_rdma_driver *, struct ibv_context *, uint32_t, uint32_t, struct ibv_gid_entry *, struct jz_error *);
int jz_rdma_driver_alloc_pd(const struct jz_rdma_driver *, struct ibv_context *, struct ibv_pd **, struct jz_error *);
int jz_rdma_driver_dealloc_pd(const struct jz_rdma_driver *, struct ibv_pd *, struct jz_error *);

struct jz_rdma_context_source;
struct jz_rdma_context_lease {
    struct ibv_context *verbs; /* Live borrowed/owned context; NULL after return. */
};
struct jz_rdma_source_ops {
    int (*prepare)(struct jz_rdma_context_source *, struct jz_error *);
    int (*acquire)(struct jz_rdma_context_source *, const char *,
                   struct jz_rdma_context_lease *, struct jz_error *);
    int (*return_lease)(struct jz_rdma_context_source *,
                        struct jz_rdma_context_lease *, struct jz_error *);
    void (*release_list)(struct jz_rdma_context_source *);
};
struct jz_rdma_context_source {
    const struct jz_rdma_source_ops *ops; /* CM borrowing or verbs ownership policy. */
    const struct jz_rdma_driver *driver;  /* Borrowed from owning RDMA backend. */
    struct ibv_context **cm_contexts;     /* Retained CM reference array, if CM. */
    struct ibv_device **verbs_devices;   /* Retained enumeration array, if verbs. */
    int count;                          /* Entries in the active provider list. */
    size_t leases;                      /* Contexts not yet safely returned. */
};
void jz_rdma_source_init(struct jz_rdma_context_source *, unsigned,
                          const struct jz_rdma_driver *);
int jz_rdma_source_release(struct jz_rdma_context_source *, struct jz_error *);

struct jz_rdma_device {
    char name[JZ_RDMA_DEVICE_NAME_MAX]; /* Copied configuration; survives release. */
    struct jz_rdma_context_lease lease; /* Must outlive this device's PD. */
    struct ibv_pd *pd;                 /* Backend-owned; never the CM default PD. */
    struct jz_rdma_device_info info;    /* Valid only after successful prepare. */
    struct jz_rdma_port_info *ports;    /* Owned array, indexed by port minus one. */
};
struct jz_rdma_backend {
    struct jz_backend base;              /* First member: composition for dispatch. */
    struct jz_rdma_driver driver;        /* Raw calls remain below this boundary. */
    struct jz_rdma_context_source source; /* Parent reference for every device. */
    struct jz_rdma_device *devices;      /* Owned table, in configured order. */
    size_t device_count;                 /* Fixed until backend disposal. */
    unsigned setup_modes;               /* Copied CM/OOB policy for later QP setup. */
};
enum { JZ_BACKEND_RDMA = 1 };
int jz_rdma_device_prepare(struct jz_rdma_device *, struct jz_rdma_context_source *, struct jz_error *);
int jz_rdma_device_release(struct jz_rdma_device *, struct jz_rdma_context_source *, struct jz_error *);
/* The test seam never becomes part of the public ABI. */
int jz_rdma_configure_with_calls(jz_engine *, const struct jz_rdma_config *,
                                 const struct jz_rdma_calls *, struct jz_error *);
struct jz_rdma_backend *jz_rdma_backend_find(const jz_engine *);
#endif
