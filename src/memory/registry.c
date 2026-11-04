#include "memory/internal.h"
#include "runtime/engine_internal.h"
#include "common/error.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct operation {
    struct jz_memory_record **records; /* Input order, not address-sorted order. */
    struct jz_backend **backends;      /* Stable during all registration API calls. */
    struct jz_memory_job *jobs;        /* Backend-major matrix, allocated before HW. */
    size_t count, backend_count;
};

int jz_memory_registry_create(struct jz_memory_registry **out, struct jz_error *error)
{
    struct jz_memory_registry *registry = calloc(1, sizeof(*registry));
    if (!registry) return jz_fail(error, ENOMEM, "memory-registry-allocate");
    int rc = pthread_rwlock_init(&registry->lock, NULL);
    if (rc) { free(registry); return jz_fail(error, rc, "memory-registry-lock"); }
    *out = registry;
    return 0;
}
void jz_memory_registry_dispose(struct jz_memory_registry *registry)
{
    if (!registry) return;
    /* Engine teardown must empty the registry before destroying parent objects. */
    (void)pthread_rwlock_destroy(&registry->lock);
    free(registry);
}
void jz_memory_options_init(struct jz_memory_options *options)
{
    if (options) *options = (struct jz_memory_options){
        .location = "*", .remote_accessible = true };
}
static void record_free(struct jz_memory_record *record)
{
    if (!record) return;
    free(record->bindings);
    free(record);
}
static void operation_dispose(struct operation *op)
{
    free(op->records); free(op->backends); free(op->jobs);
}
static int operation_create(jz_engine *engine, size_t count, struct operation *op,
                             struct jz_error *error)
{
    *op = (struct operation){.count = count};
    for (size_t b = 0; b < engine->backend_count; ++b)
        if (engine->backends[b] && engine->backends[b]->memory_ops) ++op->backend_count;
    if (!op->backend_count) return jz_fail(error, ENODEV, "memory-no-backend");
    if (count > SIZE_MAX / sizeof(*op->records) ||
        count > SIZE_MAX / op->backend_count / sizeof(*op->jobs))
        return jz_fail(error, EOVERFLOW, "memory-batch-size");
    op->records = calloc(count, sizeof(*op->records));
    op->backends = calloc(op->backend_count, sizeof(*op->backends));
    op->jobs = calloc(count * op->backend_count, sizeof(*op->jobs));
    if (!op->records || !op->backends || !op->jobs) {
        operation_dispose(op); *op = (struct operation){0};
        return jz_fail(error, ENOMEM, "memory-operation-allocate");
    }
    size_t index = 0;
    for (size_t b = 0; b < engine->backend_count; ++b)
        if (engine->backends[b] && engine->backends[b]->memory_ops)
            op->backends[index++] = engine->backends[b];
    return 0;
}
static int request_check(jz_engine *engine, size_t count,
    struct jz_registration_result *results, bool registering, struct jz_error *error)
{
    jz_error_clear(error);
    if (!count || !results) return jz_fail(error, EINVAL, "memory-batch-arguments");
    if (count > SIZE_MAX / sizeof(*results))
        return jz_fail(error, EOVERFLOW, "memory-result-size");
    for (size_t i = 0; i < count; ++i) results[i].state = JZ_REGISTRATION_NONE;
    if (!engine) return jz_fail(error, EINVAL, "engine-null");
    if (engine->state != JZ_ENGINE_READY &&
        (registering || engine->state != JZ_ENGINE_CLEANUP_REQUIRED))
        return jz_fail(error, EBUSY, "engine-state");
    return 0;
}
static int record_compare(const void *a, const void *b)
{
    const struct jz_memory_record *x = *(struct jz_memory_record *const *)a;
    const struct jz_memory_record *y = *(struct jz_memory_record *const *)b;
    return x->index.start < y->index.start ? -1 : x->index.start > y->index.start;
}
static int reserve(struct jz_memory_registry *registry, struct operation *op,
                     struct jz_error *error)
{
    struct jz_memory_record **sorted = malloc(op->count * sizeof(*sorted));
    if (!sorted) return jz_fail(error, ENOMEM, "memory-sort-allocate");
    memcpy(sorted, op->records, op->count * sizeof(*sorted));
    qsort(sorted, op->count, sizeof(*sorted), record_compare);
    for (size_t i = 1; i < op->count; ++i) {
        if (sorted[i - 1]->index.end > sorted[i]->index.start) {
            free(sorted); return jz_fail(error, EEXIST, "memory-batch-overlap");
        }
    }
    int rc = pthread_rwlock_wrlock(&registry->lock);
    if (rc) { free(sorted); return jz_fail(error, rc, "memory-registry-lock"); }
    if (op->count > SIZE_MAX - registry->count) rc = EOVERFLOW;
    for (size_t i = 0; !rc && i < op->count; ++i)
        if (jz_interval_overlap(registry->root, sorted[i]->index.start, sorted[i]->index.end))
            rc = EEXIST;
    if (!rc) {
        for (size_t i = 0; i < op->count; ++i)
            registry->root = jz_interval_insert(registry->root, &sorted[i]->index);
        registry->count += op->count;
    }
    (void)pthread_rwlock_unlock(&registry->lock);
    free(sorted);
    return rc ? jz_fail(error, rc, "memory-range-reservation") : 0;
}
static void jobs_from_records(struct operation *op)
{
    for (size_t b = 0; b < op->backend_count; ++b)
        for (size_t i = 0; i < op->count; ++i) {
            struct jz_memory_record *record = op->records[i];
            if (!record) continue;
            struct jz_memory_job *job = &op->jobs[b * op->count + i];
            *job = (struct jz_memory_job){ .buffer = record->info.buffer,
                .location = record->info.location,
                .remote_accessible = record->info.remote_accessible,
                .resource = record->bindings[b].resource };
        }
}
static void release_jobs(struct operation *op, size_t attempted)
{
    for (size_t b = attempted; b > 0; --b)
        op->backends[b - 1]->memory_ops->release_batch(op->backends[b - 1],
            &op->jobs[(b - 1) * op->count], op->count);
}
/* Publish only after backend workers have joined. Queries read copied info,
 * never tokens concurrently being changed by a provider operation. */
static int finish(struct jz_memory_registry *registry, struct operation *op,
    bool active, const struct jz_error *original, struct jz_registration_result *results,
    struct jz_error *error)
{
    int first = 0;
    (void)pthread_rwlock_wrlock(&registry->lock);
    for (size_t i = 0; i < op->count; ++i) {
        struct jz_memory_record *record = op->records[i];
        if (!record) continue;
        bool live = false;
        struct jz_error cleanup = {0};
        for (size_t b = 0; b < op->backend_count; ++b) {
            struct jz_memory_job *job = &op->jobs[b * op->count + i];
            record->bindings[b].resource = job->resource;
            if (job->resource) {
                live = true;
                if (!active && !cleanup.code) {
                    cleanup = job->cleanup_error;
                    if (!cleanup.code) (void)jz_fail(&cleanup, EIO, "memory-backend-cleanup");
                }
            }
            if (active && job->resolved_location[0])
                memcpy(record->info.location, job->resolved_location, sizeof(record->info.location));
        }
        if (active) {
            record->info.state = JZ_MEMORY_ACTIVE;
            results[i].state = JZ_REGISTRATION_ACTIVE;
        } else if (live) {
            record->info.state = JZ_MEMORY_CLEANUP_REQUIRED;
            if (original) record->info.registration_error = *original;
            record->info.cleanup_error = cleanup;
            results[i].state = JZ_REGISTRATION_CLEANUP_REQUIRED;
            if (!first) { first = cleanup.code; if (error) *error = cleanup; }
        } else {
            registry->root = jz_interval_remove(registry->root, record->index.start);
            --registry->count;
            results[i].state = JZ_REGISTRATION_NONE;
            record_free(record);
        }
    }
    (void)pthread_rwlock_unlock(&registry->lock);
    return first;
}
int jz_register_local_memory_batch(jz_engine *engine, const struct jz_memory_buffer *buffers,
    size_t count, const struct jz_memory_options *options,
    struct jz_registration_result *results, struct jz_error *error)
{
    int rc = request_check(engine, count, results, true, error);
    if (rc) return rc;
    struct jz_memory_options defaults;
    jz_memory_options_init(&defaults);
    if (!options) options = &defaults;
    if (!buffers || !options->location || !*options->location)
        return jz_fail(error, EINVAL, "memory-description");
    size_t label_length = strnlen(options->location, JZ_MEMORY_LOCATION_MAX);
    if (label_length == JZ_MEMORY_LOCATION_MAX)
        return jz_fail(error, ENAMETOOLONG, "memory-location");
    struct operation op;
    rc = operation_create(engine, count, &op, error);
    if (rc) return rc;
    for (size_t i = 0; i < count; ++i) {
        uintptr_t start = (uintptr_t)buffers[i].addr;
        if (!start || !buffers[i].length) { rc = jz_fail(error, EINVAL, "memory-range"); break; }
        if (buffers[i].length > UINTPTR_MAX - start) {
            rc = jz_fail(error, EOVERFLOW, "memory-address-overflow"); break;
        }
        struct jz_memory_record *record = calloc(1, sizeof(*record));
        if (!record) { rc = jz_fail(error, ENOMEM, "memory-record-allocate"); break; }
        op.records[i] = record;
        record->bindings = calloc(op.backend_count, sizeof(*record->bindings));
        if (!record->bindings) { rc = jz_fail(error, ENOMEM, "memory-bindings-allocate"); break; }
        record->binding_count = op.backend_count;
        record->index.start = start; record->index.end = start + buffers[i].length;
        record->info.buffer = buffers[i]; record->info.state = JZ_MEMORY_REGISTERING;
        record->info.remote_accessible = options->remote_accessible;
        memcpy(record->info.location, options->location, label_length + 1);
        for (size_t b = 0; b < op.backend_count; ++b) record->bindings[b].backend = op.backends[b];
    }
    if (!rc) rc = reserve(engine->memory, &op, error);
    if (rc) {
        for (size_t i = 0; i < count; ++i) record_free(op.records[i]);
        operation_dispose(&op); return rc;
    }
    jobs_from_records(&op);
    struct jz_error original = {0};
    size_t attempted = 0;
    for (size_t b = 0; b < op.backend_count; ++b) {
        attempted = b + 1;
        rc = op.backends[b]->memory_ops->register_batch(op.backends[b],
            &op.jobs[b * count], count, &original);
        if (rc) {
            if (!original.code) (void)jz_fail(&original, -rc, "memory-backend-register");
            break;
        }
    }
    if (rc) release_jobs(&op, attempted);
    (void)finish(engine->memory, &op, !rc, rc ? &original : NULL, results, NULL);
    if (rc && error) *error = original;
    operation_dispose(&op);
    return rc;
}
int jz_register_local_memory(jz_engine *engine, const struct jz_memory_buffer *buffer,
    const struct jz_memory_options *options, struct jz_registration_result *result,
    struct jz_error *error)
{
    return jz_register_local_memory_batch(engine, buffer, 1, options, result, error);
}
static int address_compare(const void *a, const void *b)
{
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return x < y ? -1 : x > y;
}
int jz_unregister_local_memory_batch(jz_engine *engine, void *const *bases, size_t count,
    struct jz_registration_result *results, struct jz_error *error)
{
    int rc = request_check(engine, count, results, false, error);
    if (rc) return rc;
    if (!bases) return jz_fail(error, EINVAL, "memory-bases");
    struct operation op;
    rc = operation_create(engine, count, &op, error);
    if (rc) return rc;
    uintptr_t *sorted = malloc(count * sizeof(*sorted));
    if (!sorted) { operation_dispose(&op); return jz_fail(error, ENOMEM, "memory-sort-allocate"); }
    for (size_t i = 0; i < count; ++i) {
        if (!bases[i]) { rc = EINVAL; break; }
        sorted[i] = (uintptr_t)bases[i];
    }
    if (!rc) {
        qsort(sorted, count, sizeof(*sorted), address_compare);
        for (size_t i = 1; i < count; ++i) if (sorted[i] == sorted[i - 1]) rc = EINVAL;
    }
    free(sorted);
    if (rc) { operation_dispose(&op); return jz_fail(error, rc, "memory-bases"); }
    struct jz_memory_registry *registry = engine->memory;
    rc = pthread_rwlock_wrlock(&registry->lock);
    if (rc) { operation_dispose(&op); return jz_fail(error, rc, "memory-registry-lock"); }
    int first = 0;
    for (size_t i = 0; i < count; ++i) {
        struct jz_memory_record *record = (struct jz_memory_record *)
            jz_interval_find(registry->root, (uintptr_t)bases[i]);
        if (!record) { if (!first) first = jz_fail(error, ENOENT, "memory-base-not-found"); continue; }
        if (record->info.state == JZ_MEMORY_REGISTERING || record->info.state == JZ_MEMORY_UNREGISTERING) {
            results[i].state = JZ_REGISTRATION_ACTIVE; /* Retain the busy mapping. */
            if (!first) first = jz_fail(error, EBUSY, "memory-operation-busy");
            continue;
        }
        record->info.state = JZ_MEMORY_UNREGISTERING;
        op.records[i] = record;
    }
    (void)pthread_rwlock_unlock(&registry->lock);
    jobs_from_records(&op);
    release_jobs(&op, op.backend_count);
    struct jz_error cleanup = {0};
    int cleanup_rc = finish(registry, &op, false, NULL, results, &cleanup);
    if (!first && cleanup_rc) { first = cleanup_rc; if (error) *error = cleanup; }
    operation_dispose(&op);
    return first;
}
int jz_unregister_local_memory(jz_engine *engine, void *base, struct jz_error *error)
{
    struct jz_registration_result result;
    return jz_unregister_local_memory_batch(engine, &base, 1, &result, error);
}
int jz_query_local_memory(const jz_engine *engine, void *base,
    struct jz_local_memory_info *out, struct jz_error *error)
{
    jz_error_clear(error);
    if (!engine || !base || !out) return jz_fail(error, EINVAL, "memory-query-arguments");
    struct jz_memory_registry *registry = engine->memory;
    int rc = pthread_rwlock_rdlock(&registry->lock);
    if (rc) return jz_fail(error, rc, "memory-registry-lock");
    struct jz_memory_record *record = (struct jz_memory_record *)
        jz_interval_find(registry->root, (uintptr_t)base);
    if (record) *out = record->info;
    (void)pthread_rwlock_unlock(&registry->lock);
    return record ? 0 : jz_fail(error, ENOENT, "memory-base-not-found");
}
static void collect_bases(struct jz_interval *node, void **bases, size_t *count)
{
    if (!node) return;
    collect_bases(node->left, bases, count);
    bases[(*count)++] = (void *)node->start;
    collect_bases(node->right, bases, count);
}
int jz_memory_release_all(jz_engine *engine, struct jz_error *error)
{
    /* Destroy has external exclusion, so this snapshot cannot race an API call. */
    size_t count = engine->memory->count;
    if (!count) return 0;
    if (count > SIZE_MAX / sizeof(void *) || count > SIZE_MAX / sizeof(struct jz_registration_result))
        return jz_fail(error, EOVERFLOW, "memory-teardown-size");
    void **bases = calloc(count, sizeof(*bases));
    struct jz_registration_result *results = calloc(count, sizeof(*results));
    if (!bases || !results) {
        free(bases); free(results); return jz_fail(error, ENOMEM, "memory-teardown-allocate");
    }
    size_t used = 0;
    collect_bases(engine->memory->root, bases, &used);
    int rc = jz_unregister_local_memory_batch(engine, bases, used, results, error);
    free(bases); free(results);
    return rc;
}
