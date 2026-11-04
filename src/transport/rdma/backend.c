#include "transport/rdma/internal.h"
#include "runtime/engine_internal.h"
#include "common/error.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The embedded base is the first member; type recovery stays in this module. */
static struct jz_rdma_backend *from_base(struct jz_backend *base)
{
    return (struct jz_rdma_backend *)base;
}
struct jz_rdma_backend *jz_rdma_backend_find(const jz_engine *engine)
{
    return from_base(jz_engine_find(engine, JZ_BACKEND_RDMA));
}
static int backend_prepare(struct jz_backend *base, struct jz_error *error)
{
    struct jz_rdma_backend *backend = from_base(base);
    int rc = backend->source.ops->prepare(&backend->source, error);
    if (rc) return rc;
    if (backend->source.count <= 0) return jz_fail(error, ENODEV, "no-devices");
    for (size_t i = 0; i < backend->device_count; ++i) {
        rc = jz_rdma_device_prepare(&backend->devices[i], &backend->source, error);
        if (rc) return rc;
    }
    return 0;
}
static int backend_release(struct jz_backend *base, struct jz_error *error)
{
    struct jz_rdma_backend *backend = from_base(base);
    if (atomic_load(&backend->registration_count))
        return jz_fail(error, EBUSY, "rdma-registrations-live");
    int first = 0;
    for (size_t i = backend->device_count; i > 0; --i) {
        struct jz_error detail = {0};
        int rc = jz_rdma_device_release(&backend->devices[i - 1], &backend->source, &detail);
        if (rc && !first) { first = rc; if (error) *error = detail; }
    }
    /* Never drop a provider list while even one device retains a lease. */
    if (first) return first;
    return jz_rdma_source_release(&backend->source, error);
}
static void backend_dispose(struct jz_backend *base)
{
    struct jz_rdma_backend *backend = from_base(base);
    /* Runtime resources have already been released. No fallible work here. */
    free(backend->devices);
    free(backend);
}
static const struct jz_backend_ops backend_ops = {
    .prepare = backend_prepare, .release = backend_release, .dispose = backend_dispose
};
void jz_rdma_config_init(struct jz_rdma_config *config)
{
    if (config) *config = (struct jz_rdma_config){
        .setup_modes = JZ_RDMA_SETUP_CM | JZ_RDMA_SETUP_OOB };
}
static int validate_config(const struct jz_rdma_config *config, struct jz_error *error)
{
    const unsigned valid_modes = JZ_RDMA_SETUP_CM | JZ_RDMA_SETUP_OOB;
    if (!config || !config->devices || !config->device_count ||
        config->device_count > JZ_RDMA_DEVICE_LIMIT || !config->setup_modes ||
        (config->setup_modes & ~valid_modes))
        return jz_fail(error, EINVAL, "rdma-config");
    for (size_t i = 0; i < config->device_count; ++i) {
        const char *name = config->devices[i];
        if (!name || !*name || strnlen(name, JZ_RDMA_DEVICE_NAME_MAX) == JZ_RDMA_DEVICE_NAME_MAX)
            return jz_fail(error, EINVAL, "device-name");
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(name, config->devices[j]))
                return jz_fail(error, EEXIST, "duplicate-device");
    }
    return 0;
}
int jz_rdma_configure_with_calls(jz_engine *engine, const struct jz_rdma_config *config,
                                 const struct jz_rdma_calls *calls, struct jz_error *error)
{
    jz_error_clear(error);
    if (!engine || !calls) return jz_fail(error, EINVAL, "rdma-arguments");
    if (engine->state != JZ_ENGINE_CREATED) return jz_fail(error, EBUSY, "engine-state");
    if (jz_rdma_backend_find(engine)) return jz_fail(error, EALREADY, "backend-duplicate");
    int rc = validate_config(config, error);
    if (rc) return rc;
    struct jz_rdma_backend *backend = calloc(1, sizeof(*backend));
    if (!backend) return jz_fail(error, ENOMEM, "rdma-backend-allocate");
    backend->base = (struct jz_backend){ .kind = JZ_BACKEND_RDMA, .ops = &backend_ops,
        .memory_ops = &jz_rdma_memory_ops };
    backend->driver.calls = calls;
    backend->device_count = config->device_count;
    backend->setup_modes = config->setup_modes;
    backend->mr_chunk_bytes = config->mr_chunk_bytes;
    atomic_init(&backend->registration_count, 0);
    backend->devices = calloc(config->device_count, sizeof(*backend->devices));
    if (!backend->devices) {
        backend_dispose(&backend->base);
        return jz_fail(error, ENOMEM, "device-table-allocate");
    }
    for (size_t i = 0; i < backend->device_count; ++i)
        memcpy(backend->devices[i].name, config->devices[i], strlen(config->devices[i]) + 1);
    jz_rdma_source_init(&backend->source, config->setup_modes, &backend->driver);
    /* Publish only a fully constructed backend. Attach failure transfers nothing. */
    rc = jz_engine_attach(engine, &backend->base, error);
    if (rc) backend_dispose(&backend->base);
    return rc;
}
int jz_rdma_configure(jz_engine *engine, const struct jz_rdma_config *config,
                      struct jz_error *error)
{
    return jz_rdma_configure_with_calls(engine, config, &jz_rdma_system_calls, error);
}
static int query_check(const jz_engine *engine, size_t index, const void *out,
                        struct jz_rdma_backend **backend, struct jz_error *error)
{
    jz_error_clear(error);
    if (!engine || !out) return jz_fail(error, EINVAL, "query-arguments");
    if (engine->state != JZ_ENGINE_READY) return jz_fail(error, EAGAIN, "device-not-ready");
    *backend = jz_rdma_backend_find(engine);
    if (!*backend) return jz_fail(error, ENODEV, "rdma-not-configured");
    if (index >= (*backend)->device_count) return jz_fail(error, ERANGE, "device-index");
    return 0;
}
int jz_rdma_query_device(const jz_engine *engine, size_t index,
                         struct jz_rdma_device_info *out, struct jz_error *error)
{
    struct jz_rdma_backend *backend = NULL;
    int rc = query_check(engine, index, out, &backend, error);
    if (!rc) *out = backend->devices[index].info;
    return rc;
}
int jz_rdma_query_port(const jz_engine *engine, size_t index, uint8_t port,
                       struct jz_rdma_port_info *out, struct jz_error *error)
{
    struct jz_rdma_backend *backend = NULL;
    int rc = query_check(engine, index, out, &backend, error);
    if (rc) return rc;
    const struct jz_rdma_device *device = &backend->devices[index];
    if (!port || port > device->info.port_count) return jz_fail(error, ERANGE, "port-index");
    *out = device->ports[port - 1u];
    return 0;
}
