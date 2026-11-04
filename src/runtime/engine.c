#include "runtime/engine_internal.h"
#include "common/error.h"
#include <errno.h>
#include <stdlib.h>

enum { JZ_BACKEND_LIMIT = 16 };

int jz_engine_create(jz_engine **out, struct jz_error *error)
{
    jz_error_clear(error);
    if (!out || *out) return jz_fail(error, EINVAL, "engine-output");
    jz_engine *engine = calloc(1, sizeof(*engine));
    if (!engine) return jz_fail(error, ENOMEM, "engine-allocate");
    engine->state = JZ_ENGINE_CREATED;
    *out = engine;
    return 0;
}

struct jz_backend *jz_engine_find(const jz_engine *engine, unsigned kind)
{
    if (!engine) return NULL;
    for (size_t i = 0; i < engine->backend_count; ++i) {
        struct jz_backend *backend = engine->backends[i];
        if (backend && backend->kind == kind) return backend;
    }
    return NULL;
}

int jz_engine_attach(jz_engine *engine, struct jz_backend *backend, struct jz_error *error)
{
    jz_error_clear(error);
    if (!engine || !backend || !backend->ops || !backend->ops->prepare ||
        !backend->ops->release || !backend->ops->dispose)
        return jz_fail(error, EINVAL, "backend-arguments");
    if (engine->state != JZ_ENGINE_CREATED)
        return jz_fail(error, EBUSY, "engine-state");
    if (jz_engine_find(engine, backend->kind))
        return jz_fail(error, EALREADY, "backend-duplicate");
    if (engine->backend_count == JZ_BACKEND_LIMIT)
        return jz_fail(error, ENOSPC, "backend-limit");
    size_t count = engine->backend_count + 1;
    struct jz_backend **slots = realloc(engine->backends, count * sizeof(*slots));
    if (!slots) return jz_fail(error, ENOMEM, "backend-table-allocate");
    slots[engine->backend_count] = backend;
    engine->backends = slots;
    engine->backend_count = count;
    return 0;
}

int jz_engine_prepare(jz_engine *engine, struct jz_error *error)
{
    jz_error_clear(error);
    if (!engine) return jz_fail(error, EINVAL, "engine-null");
    if (engine->state != JZ_ENGINE_CREATED)
        return jz_fail(error, engine->state == JZ_ENGINE_READY ? EALREADY : EBUSY,
                       "engine-state");
    if (!engine->backend_count) return jz_fail(error, ENODEV, "no-backend");
    engine->state = JZ_ENGINE_PREPARING;
    for (size_t i = 0; i < engine->backend_count; ++i) {
        struct jz_backend *backend = engine->backends[i];
        int rc = backend->ops->prepare(backend, error);
        if (!rc) continue;
        /* Include the failing backend: it may already own partial resources.
         * Keep the original prepare error even if rollback also fails. */
        int pending = 0;
        for (size_t j = i + 1; j > 0; --j) {
            struct jz_backend *previous = engine->backends[j - 1];
            if (previous->ops->release(previous, NULL)) pending = 1;
        }
        engine->state = pending ? JZ_ENGINE_CLEANUP_REQUIRED : JZ_ENGINE_CREATED;
        return rc;
    }
    engine->state = JZ_ENGINE_READY;
    return 0;
}

int jz_engine_get_state(const jz_engine *engine, enum jz_engine_state *out)
{
    if (!engine || !out) return -EINVAL;
    *out = engine->state;
    return 0;
}

int jz_engine_destroy(jz_engine **handle, struct jz_error *error)
{
    jz_error_clear(error);
    if (!handle) return jz_fail(error, EINVAL, "engine-pointer");
    jz_engine *engine = *handle;
    if (!engine) return 0;
    int first = 0;
    engine->state = JZ_ENGINE_CLEANUP_REQUIRED;
    for (size_t i = engine->backend_count; i > 0; --i) {
        struct jz_backend *backend = engine->backends[i - 1];
        if (!backend) continue; /* A previous destroy attempt already disposed it. */
        struct jz_error detail = {0};
        int rc = backend->ops->release(backend, &detail);
        if (rc) {
            if (!first) { first = rc; if (error) *error = detail; }
            continue; /* Other independent backends can still be cleaned up. */
        }
        backend->ops->dispose(backend);
        engine->backends[i - 1] = NULL;
    }
    if (first) return first;
    free(engine->backends);
    free(engine);
    *handle = NULL;
    return 0;
}
