#include "runtime/engine_internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int jz_engine_fail(struct jz_error *error, int errnum, const char *stage)
{
    if (error) *error = (struct jz_error){ .code = -errnum,
        .stage = stage, .system_errno = errnum };
    return -errnum;
}

void jz_engine_config_init(struct jz_engine_config *config)
{
    if (config) *config = (struct jz_engine_config){
        .setup_modes = JZ_RDMA_SETUP_CM | JZ_RDMA_SETUP_OOB };
}

int jz_engine_create_with_ops(const struct jz_engine_config *config,
                      const struct jz_rdma_device_ops *ops,
                      jz_engine **out, struct jz_error *error)
{
    if (error) *error = (struct jz_error){0};
    if (!config || !ops || !out || *out || !config->devices ||
        !config->device_count || config->device_count > JZ_RDMA_DEVICE_LIMIT ||
        !config->setup_modes || (config->setup_modes & ~3u))
        return jz_engine_fail(error, EINVAL, "engine-config");
    /* Validate the entire name set before allocation; do not borrow caller strings. */
    for (size_t i = 0; i < config->device_count; ++i) {
        const char *name = config->devices[i];
        if (!name || !*name || strnlen(name, JZ_RDMA_DEVICE_NAME_MAX) == JZ_RDMA_DEVICE_NAME_MAX)
            return jz_engine_fail(error, EINVAL, "device-name");
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(name, config->devices[j]))
                return jz_engine_fail(error, EEXIST, "duplicate-device");
    }
    jz_engine *engine = calloc(1, sizeof(*engine));
    if (!engine) return jz_engine_fail(error, ENOMEM, "engine-allocate");
    engine->devices = calloc(config->device_count, sizeof(*engine->devices));
    if (!engine->devices) {
        free(engine);
        return jz_engine_fail(error, ENOMEM, "device-table-allocate");
    }
    engine->state = JZ_ENGINE_CREATED;
    engine->setup_modes = config->setup_modes;
    engine->device_count = config->device_count;
    engine->ops = ops;
    for (size_t i = 0; i < engine->device_count; ++i)
        memcpy(engine->devices[i].name, config->devices[i], strlen(config->devices[i]) + 1);
    *out = engine;
    return 0;
}

int jz_engine_create(const struct jz_engine_config *config,
                       jz_engine **out, struct jz_error *error)
{
    return jz_engine_create_with_ops(config, &jz_rdma_system_ops, out, error);
}

int jz_engine_prepare(jz_engine *engine, struct jz_error *error)
{
    if (error) *error = (struct jz_error){0};
    if (!engine) return jz_engine_fail(error, EINVAL, "engine-null");
    if (engine->state != JZ_ENGINE_CREATED)
        return jz_engine_fail(error, engine->state == JZ_ENGINE_READY ? EALREADY : EBUSY,
                        "engine-state");
    engine->state = JZ_ENGINE_PREPARING;
    int rc = jz_rdma_devices_prepare(engine, error);
    if (!rc) {
        engine->state = JZ_ENGINE_READY;
        return 0;
    }
    /* Rollback must not overwrite the first preparation error. State and destroy
 * retries expose cleanup failures. */
    engine->state = jz_rdma_devices_release(engine, NULL) ?
        JZ_ENGINE_CLEANUP_REQUIRED : JZ_ENGINE_CREATED;
    return rc;
}

int jz_engine_get_state(const jz_engine *engine, enum jz_engine_state *out)
{
    if (!engine || !out) return -EINVAL;
    *out = engine->state;
    return 0;
}

static int query_check(const jz_engine *engine, size_t index,
                        const void *out, struct jz_error *error)
{
    if (error) *error = (struct jz_error){0};
    if (!engine || !out) return jz_engine_fail(error, EINVAL, "query-arguments");
    if (engine->state != JZ_ENGINE_READY)
        return jz_engine_fail(error, EAGAIN, "device-not-ready");
    if (index >= engine->device_count) return jz_engine_fail(error, ERANGE, "device-index");
    return 0;
}

int jz_rdma_query_device(const jz_engine *engine, size_t index,
                             struct jz_rdma_device_info *out, struct jz_error *error)
{
    int rc = query_check(engine, index, out, error);
    if (!rc) *out = engine->devices[index].info;
    return rc;
}

int jz_rdma_query_port(const jz_engine *engine, size_t index, uint8_t port,
                           struct jz_rdma_port_info *out, struct jz_error *error)
{
    int rc = query_check(engine, index, out, error);
    if (rc) return rc;
    const struct jz_rdma_device *device = &engine->devices[index];
    if (!port || port > device->info.port_count)
        return jz_engine_fail(error, ERANGE, "port-index");
    *out = device->ports[port - 1u];
    return 0;
}

int jz_engine_destroy(jz_engine **engine, struct jz_error *error)
{
    if (error) *error = (struct jz_error){0};
    if (!engine) return jz_engine_fail(error, EINVAL, "engine-pointer");
    if (!*engine) return 0;
    int rc = jz_rdma_devices_release(*engine, error);
    if (rc) {
        (*engine)->state = JZ_ENGINE_CLEANUP_REQUIRED;
        return rc;
    }
    free((*engine)->devices);
    free(*engine);
    *engine = NULL;
    return 0;
}
