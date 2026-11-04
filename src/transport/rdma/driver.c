#include "transport/rdma/internal.h"
#include "common/error.h"
#include <rdma/rdma_cma.h>
#include <errno.h>

static int system_query_port(struct ibv_context *context, uint8_t port,
                              struct ibv_port_attr *attr)
{
    return ibv_query_port(context, port, attr);
}
const struct jz_rdma_calls jz_rdma_system_calls = {
    .cm_list = rdma_get_devices, .cm_free = rdma_free_devices,
    .verbs_list = ibv_get_device_list, .verbs_free = ibv_free_device_list,
    .name = ibv_get_device_name, .open = ibv_open_device, .close = ibv_close_device,
    .query_device = ibv_query_device, .query_port = system_query_port,
    .query_gid = ibv_query_gid_ex, .alloc_pd = ibv_alloc_pd, .dealloc_pd = ibv_dealloc_pd
};

/* Providers use positive errno or -1/errno; tolerate negative errno too.
 * Callers clear errno before every provider call to avoid stale causes. */
static int status(int rc, struct jz_error *error, const char *stage)
{
    if (!rc) return 0;
    int cause = rc > 0 ? rc : (rc == -1 ? (errno ? errno : EIO) : -rc);
    return jz_fail(error, cause, stage);
}

int jz_rdma_driver_close(const struct jz_rdma_driver *driver,
    struct ibv_context *context, struct jz_error *error)
{
    errno = 0;
    int rc = driver->calls->close(context);
    return status(rc, error, "close-device");
}

int jz_rdma_driver_query_device(const struct jz_rdma_driver *driver,
    struct ibv_context *context, struct ibv_device_attr *attr, struct jz_error *error)
{
    errno = 0;
    int rc = driver->calls->query_device(context, attr);
    return status(rc, error, "query-device");
}

int jz_rdma_driver_query_port(const struct jz_rdma_driver *driver,
    struct ibv_context *context, uint8_t port, struct ibv_port_attr *attr, struct jz_error *error)
{
    errno = 0;
    int rc = driver->calls->query_port(context, port, attr);
    return status(rc, error, "query-port");
}

int jz_rdma_driver_query_gid(const struct jz_rdma_driver *driver,
    struct ibv_context *context, uint32_t port, uint32_t gid, struct ibv_gid_entry *entry, struct jz_error *error)
{
    errno = 0;
    int rc = driver->calls->query_gid(context, port, gid, entry, 0);
    return status(rc, error, "query-gid-type");
}

int jz_rdma_driver_dealloc_pd(const struct jz_rdma_driver *driver,
    struct ibv_pd *pd, struct jz_error *error)
{
    errno = 0;
    int rc = driver->calls->dealloc_pd(pd);
    return status(rc, error, "deallocate-pd");
}

int jz_rdma_driver_cm_list(const struct jz_rdma_driver *driver,
    struct ibv_context ***out, int *count, struct jz_error *error)
{
    errno = 0;
    *out = driver->calls->cm_list(count);
    return *out ? 0 : jz_fail(error, errno ? errno : ENODEV, "enumerate-devices");
}

int jz_rdma_driver_verbs_list(const struct jz_rdma_driver *driver,
    struct ibv_device ***out, int *count, struct jz_error *error)
{
    errno = 0;
    *out = driver->calls->verbs_list(count);
    return *out ? 0 : jz_fail(error, errno ? errno : ENODEV, "enumerate-devices");
}

int jz_rdma_driver_open(const struct jz_rdma_driver *driver,
    struct ibv_device *device, struct ibv_context **out, struct jz_error *error)
{
    errno = 0;
    *out = driver->calls->open(device);
    return *out ? 0 : jz_fail(error, errno ? errno : EIO, "open-device");
}

int jz_rdma_driver_alloc_pd(const struct jz_rdma_driver *driver,
    struct ibv_context *context, struct ibv_pd **out, struct jz_error *error)
{
    errno = 0;
    *out = driver->calls->alloc_pd(context);
    return *out ? 0 : jz_fail(error, errno ? errno : ENOMEM, "allocate-pd");
}
