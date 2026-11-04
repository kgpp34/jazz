#include "transport/rdma/internal.h"
#include "common/error.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

struct warm_task {
    struct jz_memory_job *job; /* Immutable job and independently owned temporary slot. */
    size_t index;
};
struct registration_operation {
    struct jz_rdma_backend *backend;
    struct jz_memory_job *jobs;
    size_t count;
    struct warm_task *warm_tasks;
    pthread_mutex_t error_lock; /* Only protects failure publication, never provider calls. */
    struct jz_error first_error;
    atomic_bool cancelled; /* Workers stop issuing new registrations after a failure. */
};
static void fail_operation(struct registration_operation *op, const struct jz_error *error)
{
    (void)pthread_mutex_lock(&op->error_lock);
    if (!op->first_error.code)
        op->first_error = *error;
    atomic_store(&op->cancelled, true);
    (void)pthread_mutex_unlock(&op->error_lock);
}
static bool wants_pretouch(const struct jz_rdma_backend *backend, size_t length, long cpus)
{
    return backend->pretouch && (uint64_t)length >= backend->pretouch_threshold_bytes &&
           (cpus >= 4 || backend->registration_policy == JZ_RDMA_REG_PARALLEL);
}
static int create_records(struct registration_operation *op, size_t *warm_count,
                          struct jz_error *error)
{
    struct jz_rdma_backend *backend = op->backend;
    size_t limit = backend->mr_chunk_bytes ? backend->mr_chunk_bytes : SIZE_MAX;
    for (size_t d = 0; d < backend->device_count; ++d) {
        uint64_t maximum = backend->devices[d].info.max_mr_size;
        if (!maximum)
            return jz_fail(error, EOPNOTSUPP, "rdma-mr-size-capability");
        if (maximum < limit)
            limit = (size_t)maximum;
    }
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    unsigned parts = cpus > 64 ? 16 : (cpus > 8 ? 8u : (cpus > 0 ? (unsigned)cpus : 1u));
    /* Preallocate permanent and temporary MR records before any hardware work.
     * Even a temporary pre-touch MR must remain reachable if deregistration fails. */
    *warm_count = 0;
    for (size_t i = 0; i < op->count; ++i) {
        struct jz_memory_job *job = &op->jobs[i];
        struct jz_rdma_registration *registration = calloc(1, sizeof(*registration));
        if (!registration)
            return jz_fail(error, ENOMEM, "rdma-registration-allocate");
        job->resource = registration;
        (void)atomic_fetch_add(&backend->registration_count, 1);
        registration->device_errors =
            calloc(backend->device_count, sizeof(*registration->device_errors));
        if (!registration->device_errors)
            return jz_fail(error, ENOMEM, "rdma-cleanup-errors-allocate");
        int rc = jz_host_plan(&job->buffer, limit, &registration->chunks,
                              &registration->chunk_count, error);
        if (rc)
            return rc;
        if (registration->chunk_count >
            SIZE_MAX / backend->device_count / sizeof(*registration->mrs))
            return jz_fail(error, EOVERFLOW, "rdma-mr-matrix-size");
        registration->mrs =
            calloc(registration->chunk_count * backend->device_count, sizeof(*registration->mrs));
        if (!registration->mrs)
            return jz_fail(error, ENOMEM, "rdma-mr-matrix-allocate");
        if (!wants_pretouch(backend, job->buffer.length, cpus))
            continue;
        rc = jz_host_warm_plan(&job->buffer, registration->chunks, registration->chunk_count, parts,
                               &registration->warm_chunks, &registration->warm_count, error);
        if (rc)
            return rc;
        if (registration->warm_count > SIZE_MAX / sizeof(*registration->temporary) ||
            registration->warm_count > SIZE_MAX - *warm_count)
            return jz_fail(error, EOVERFLOW, "rdma-warm-count");
        registration->temporary =
            calloc(registration->warm_count, sizeof(*registration->temporary));
        if (!registration->temporary)
            return jz_fail(error, ENOMEM, "rdma-warm-mrs-allocate");
        *warm_count += registration->warm_count;
    }
    if (*warm_count) {
        if (*warm_count > SIZE_MAX / sizeof(*op->warm_tasks))
            return jz_fail(error, EOVERFLOW, "rdma-warm-tasks-size");
        op->warm_tasks = calloc(*warm_count, sizeof(*op->warm_tasks));
        if (!op->warm_tasks)
            return jz_fail(error, ENOMEM, "rdma-warm-tasks-allocate");
        size_t index = 0;
        for (size_t i = 0; i < op->count; ++i) {
            struct jz_rdma_registration *registration = op->jobs[i].resource;
            for (size_t c = 0; c < registration->warm_count; ++c)
                op->warm_tasks[index++] = (struct warm_task){.job = &op->jobs[i], .index = c};
        }
    }
    return 0;
}
static void touch_chunk(void *argument, size_t index)
{
    struct registration_operation *op = argument;
    if (atomic_load(&op->cancelled))
        return;
    struct warm_task *task = &op->warm_tasks[index];
    struct jz_rdma_registration *registration = task->job->resource;
    const struct jz_host_chunk *chunk = &registration->warm_chunks[task->index];
    struct ibv_mr **mr = &registration->temporary[task->index];
    struct jz_error error = {0};
    int rc = jz_rdma_driver_reg_mr(&op->backend->driver, op->backend->devices[0].pd,
                                   (char *)task->job->buffer.addr + chunk->offset, chunk->length,
                                   IBV_ACCESS_LOCAL_WRITE, mr, &error);
    if (!rc) {
        rc = jz_rdma_driver_dereg_mr(&op->backend->driver, *mr, &error);
        if (!rc)
            *mr = NULL;
    }
    if (rc)
        fail_operation(op, &error);
}
static void register_device(void *argument, size_t device)
{
    struct registration_operation *op = argument;
    for (size_t i = 0; i < op->count; ++i) {
        struct jz_memory_job *job = &op->jobs[i];
        struct jz_rdma_registration *registration = job->resource;
        int access = IBV_ACCESS_LOCAL_WRITE;
        if (job->remote_accessible)
            access |= IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
        for (size_t c = 0; c < registration->chunk_count; ++c) {
            if (atomic_load(&op->cancelled))
                return;
            const struct jz_host_chunk *chunk = &registration->chunks[c];
            struct jz_error error = {0};
            int rc = jz_rdma_driver_reg_mr(
                &op->backend->driver, op->backend->devices[device].pd,
                (char *)job->buffer.addr + chunk->offset, chunk->length, access,
                &registration->mrs[device * registration->chunk_count + c], &error);
            if (rc) {
                fail_operation(op, &error);
                return;
            }
        }
    }
}
static int register_batch(struct jz_backend *base, struct jz_memory_job *jobs, size_t count,
                          struct jz_error *error)
{
    struct jz_rdma_backend *backend = (struct jz_rdma_backend *)base;
    if (!backend->driver.calls->reg_mr || !backend->driver.calls->dereg_mr)
        return jz_fail(error, EOPNOTSUPP, "rdma-registration-driver");
    struct registration_operation op = {.backend = backend, .jobs = jobs, .count = count};
    atomic_init(&op.cancelled, false);
    int rc = pthread_mutex_init(&op.error_lock, NULL);
    if (rc)
        return jz_fail(error, rc, "rdma-registration-lock");
    size_t warm_count = 0;
    rc = create_records(&op, &warm_count, error);
    if (!rc) {
        jz_rdma_run_work(backend, warm_count, backend->registration_policy != JZ_RDMA_REG_SERIAL,
                         touch_chunk, &op);
        bool parallel = backend->registration_policy == JZ_RDMA_REG_PARALLEL ||
                        (backend->registration_policy == JZ_RDMA_REG_AUTO && warm_count != 0);
        if (!atomic_load(&op.cancelled))
            jz_rdma_run_work(backend, backend->device_count, parallel, register_device, &op);
        /* Every helper has joined before the generic runtime can roll back. */
        if (op.first_error.code) {
            rc = op.first_error.code;
            if (error)
                *error = op.first_error;
        }
    }
    free(op.warm_tasks);
    (void)pthread_mutex_destroy(&op.error_lock);
    if (rc)
        return rc;
    for (size_t i = 0; i < count; ++i) {
        const struct jz_rdma_registration *registration = jobs[i].resource;
        jz_host_location(&jobs[i].buffer, registration->chunks, registration->chunk_count,
                         jobs[i].location, jobs[i].resolved_location);
    }
    return 0;
}
struct cleanup_operation {
    struct jz_rdma_backend *backend;
    struct jz_memory_job *jobs;
    size_t count;
};
static void release_one(struct jz_rdma_backend *backend, struct ibv_mr **mr, struct jz_error *error)
{
    if (!*mr)
        return;
    struct jz_error detail = {0};
    if (jz_rdma_driver_dereg_mr(&backend->driver, *mr, &detail)) {
        if (!error->code)
            *error = detail;
    } else
        *mr = NULL; /* Never read MR fields after successful deregistration. */
}
static void release_device(void *argument, size_t device)
{
    struct cleanup_operation *op = argument;
    for (size_t i = op->count; i > 0; --i) {
        struct jz_rdma_registration *registration = op->jobs[i - 1].resource;
        if (!registration)
            continue;
        /* Each worker owns a device column and its own error cell. Temporary
         * MRs belong to device zero; no two workers write the same MR slot. */
        if (registration->mrs)
            for (size_t c = registration->chunk_count; c > 0; --c)
                release_one(op->backend,
                            &registration->mrs[device * registration->chunk_count + c - 1],
                            &registration->device_errors[device]);
        if (device == 0 && registration->temporary)
            for (size_t c = registration->warm_count; c > 0; --c)
                release_one(op->backend, &registration->temporary[c - 1],
                            &registration->device_errors[0]);
    }
}
static void release_batch(struct jz_backend *base, struct jz_memory_job *jobs, size_t count)
{
    struct jz_rdma_backend *backend = (struct jz_rdma_backend *)base;
    for (size_t i = 0; i < count; ++i) {
        struct jz_rdma_registration *registration = jobs[i].resource;
        if (registration && registration->device_errors)
            for (size_t d = 0; d < backend->device_count; ++d)
                registration->device_errors[d] = (struct jz_error){0};
    }
    struct cleanup_operation op = {.backend = backend, .jobs = jobs, .count = count};
    jz_rdma_run_work(backend, backend->device_count,
                     backend->registration_policy != JZ_RDMA_REG_SERIAL, release_device, &op);
    for (size_t i = 0; i < count; ++i) {
        struct jz_memory_job *job = &jobs[i];
        struct jz_rdma_registration *registration = job->resource;
        if (!registration)
            continue;
        for (size_t d = 0; registration->device_errors && d < backend->device_count; ++d)
            if (!job->cleanup_error.code && registration->device_errors[d].code)
                job->cleanup_error = registration->device_errors[d];
        if (job->cleanup_error.code)
            continue; /* All failed slots remain reachable. */
        free(registration->mrs);
        free(registration->chunks);
        free(registration->temporary);
        free(registration->warm_chunks);
        free(registration->device_errors);
        free(registration);
        job->resource = NULL;
        (void)atomic_fetch_sub(&backend->registration_count, 1);
    }
}
const struct jz_memory_backend_ops jz_rdma_memory_ops = {register_batch, release_batch};
