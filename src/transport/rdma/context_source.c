#include "transport/rdma/internal.h"
#include "common/error.h"
#include <errno.h>
#include <string.h>

static int cm_prepare(struct jz_rdma_context_source *source, struct jz_error *error)
{
    return jz_rdma_driver_cm_list(source->driver, &source->cm_contexts, &source->count, error);
}
static int verbs_prepare(struct jz_rdma_context_source *source, struct jz_error *error)
{
    return jz_rdma_driver_verbs_list(source->driver, &source->verbs_devices, &source->count, error);
}
static int cm_acquire(struct jz_rdma_context_source *source, const char *name,
                       struct jz_rdma_context_lease *lease, struct jz_error *error)
{
    for (int i = 0; i < source->count; ++i) {
        struct ibv_context *context = source->cm_contexts[i];
        if (strcmp(name, source->driver->calls->name(context->device))) continue;
        lease->verbs = context;
        ++source->leases;
        return 0;
    }
    return jz_fail(error, ENODEV, "device-not-found");
}
static int verbs_acquire(struct jz_rdma_context_source *source, const char *name,
                          struct jz_rdma_context_lease *lease, struct jz_error *error)
{
    for (int i = 0; i < source->count; ++i) {
        struct ibv_device *device = source->verbs_devices[i];
        if (strcmp(name, source->driver->calls->name(device))) continue;
        int rc = jz_rdma_driver_open(source->driver, device, &lease->verbs, error);
        if (!rc) ++source->leases;
        return rc;
    }
    return jz_fail(error, ENODEV, "device-not-found");
}
static int cm_return(struct jz_rdma_context_source *source,
                       struct jz_rdma_context_lease *lease, struct jz_error *error)
{
    (void)error;
    /* Borrowed CM contexts are closed by librdmacm, never by this lease. */
    lease->verbs = NULL;
    --source->leases;
    return 0;
}
static int verbs_return(struct jz_rdma_context_source *source,
                          struct jz_rdma_context_lease *lease, struct jz_error *error)
{
    int rc = jz_rdma_driver_close(source->driver, lease->verbs, error);
    if (rc) return rc; /* Retain both lease and list until a successful retry. */
    lease->verbs = NULL;
    --source->leases;
    return 0;
}
static void cm_release_list(struct jz_rdma_context_source *source)
{
    if (source->cm_contexts) source->driver->calls->cm_free(source->cm_contexts);
    source->cm_contexts = NULL;
}
static void verbs_release_list(struct jz_rdma_context_source *source)
{
    if (source->verbs_devices) source->driver->calls->verbs_free(source->verbs_devices);
    source->verbs_devices = NULL;
}
static const struct jz_rdma_source_ops cm_ops = {
    cm_prepare, cm_acquire, cm_return, cm_release_list
};
static const struct jz_rdma_source_ops verbs_ops = {
    verbs_prepare, verbs_acquire, verbs_return, verbs_release_list
};
void jz_rdma_source_init(struct jz_rdma_context_source *source, unsigned modes,
                          const struct jz_rdma_driver *driver)
{
    /* Dual setup uses CM-owned contexts so later CM IDs share PD context identity.
     * Context policy is selected once; QP setup policy is a separate concern. */
    *source = (struct jz_rdma_context_source){
        .ops = (modes & JZ_RDMA_SETUP_CM) ? &cm_ops : &verbs_ops, .driver = driver };
}
int jz_rdma_source_release(struct jz_rdma_context_source *source, struct jz_error *error)
{
    if (source->leases) return jz_fail(error, EBUSY, "context-leases-live");
    source->ops->release_list(source);
    source->count = 0;
    return 0;
}
