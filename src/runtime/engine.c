#include "runtime/engine_internal.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int jze_fail(struct jazz_error *error, int errnum, const char *stage)
{
    if (error) *error = (struct jazz_error){ .code = -errnum,
        .stage = stage, .system_errno = errnum };
    return -errnum;
}

void jazz_engine_config_init(struct jazz_engine_config *config)
{
    if (config) *config = (struct jazz_engine_config){
        .setup_modes = JAZZ_SETUP_CM | JAZZ_SETUP_OOB };
}

int jze_engine_create(const struct jazz_engine_config *config,
                      const struct jze_device_ops *ops,
                      jazz_engine **out, struct jazz_error *error)
{
    if (error) *error = (struct jazz_error){0};
    if (!config || !ops || !out || *out || !config->devices ||
        !config->device_count || config->device_count > JAZZ_ENGINE_DEVICE_LIMIT ||
        !config->setup_modes || (config->setup_modes & ~3u))
        return jze_fail(error, EINVAL, "engine-config");
    /* 在分配资源前验证整个名字集合；不借用调用者字符串。 */
    for (size_t i = 0; i < config->device_count; ++i) {
        const char *name = config->devices[i];
        if (!name || !*name || strnlen(name, JAZZ_DEVICE_NAME_MAX) == JAZZ_DEVICE_NAME_MAX)
            return jze_fail(error, EINVAL, "device-name");
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(name, config->devices[j]))
                return jze_fail(error, EEXIST, "duplicate-device");
    }
    jazz_engine *engine = calloc(1, sizeof(*engine));
    if (!engine) return jze_fail(error, ENOMEM, "engine-allocate");
    engine->devices = calloc(config->device_count, sizeof(*engine->devices));
    if (!engine->devices) {
        free(engine);
        return jze_fail(error, ENOMEM, "device-table-allocate");
    }
    engine->state = JAZZ_ENGINE_CREATED;
    engine->setup_modes = config->setup_modes;
    engine->device_count = config->device_count;
    engine->ops = ops;
    for (size_t i = 0; i < engine->device_count; ++i)
        memcpy(engine->devices[i].name, config->devices[i], strlen(config->devices[i]) + 1);
    *out = engine;
    return 0;
}

int jazz_engine_create(const struct jazz_engine_config *config,
                       jazz_engine **out, struct jazz_error *error)
{
    return jze_engine_create(config, &jze_system_ops, out, error);
}

int jazz_engine_prepare(jazz_engine *engine, struct jazz_error *error)
{
    if (error) *error = (struct jazz_error){0};
    if (!engine) return jze_fail(error, EINVAL, "engine-null");
    if (engine->state != JAZZ_ENGINE_CREATED)
        return jze_fail(error, engine->state == JAZZ_ENGINE_READY ? EALREADY : EBUSY,
                        "engine-state");
    engine->state = JAZZ_ENGINE_PREPARING;
    int rc = jze_devices_prepare(engine, error);
    if (!rc) {
        engine->state = JAZZ_ENGINE_READY;
        return 0;
    }
    /* 回滚不覆盖第一次准备错误；清理失败由 state 和 destroy 重试表达。 */
    engine->state = jze_devices_release(engine, NULL) ?
        JAZZ_ENGINE_CLEANUP_REQUIRED : JAZZ_ENGINE_CREATED;
    return rc;
}

int jazz_engine_get_state(const jazz_engine *engine, enum jazz_engine_state *out)
{
    if (!engine || !out) return -EINVAL;
    *out = engine->state;
    return 0;
}

static int query_check(const jazz_engine *engine, size_t index,
                        const void *out, struct jazz_error *error)
{
    if (error) *error = (struct jazz_error){0};
    if (!engine || !out) return jze_fail(error, EINVAL, "query-arguments");
    if (engine->state != JAZZ_ENGINE_READY)
        return jze_fail(error, EAGAIN, "device-not-ready");
    if (index >= engine->device_count) return jze_fail(error, ERANGE, "device-index");
    return 0;
}

int jazz_engine_query_device(const jazz_engine *engine, size_t index,
                             struct jazz_device_info *out, struct jazz_error *error)
{
    int rc = query_check(engine, index, out, error);
    if (!rc) *out = engine->devices[index].info;
    return rc;
}

int jazz_engine_query_port(const jazz_engine *engine, size_t index, uint8_t port,
                           struct jazz_port_info *out, struct jazz_error *error)
{
    int rc = query_check(engine, index, out, error);
    if (rc) return rc;
    const struct jze_device *device = &engine->devices[index];
    if (!port || port > device->info.port_count)
        return jze_fail(error, ERANGE, "port-index");
    *out = device->ports[port - 1u];
    return 0;
}

int jazz_engine_destroy(jazz_engine **engine, struct jazz_error *error)
{
    if (error) *error = (struct jazz_error){0};
    if (!engine) return jze_fail(error, EINVAL, "engine-pointer");
    if (!*engine) return 0;
    int rc = jze_devices_release(*engine, error);
    if (rc) {
        (*engine)->state = JAZZ_ENGINE_CLEANUP_REQUIRED;
        return rc;
    }
    free((*engine)->devices);
    free(*engine);
    *engine = NULL;
    return 0;
}
