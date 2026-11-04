#include "transport/rdma/internal.h"
#include "common/error.h"
#include <errno.h>
#include <stdlib.h>

static int create_records(struct jz_rdma_backend *backend, struct jz_memory_job *jobs,
    size_t count, struct jz_error *error)
{
    size_t limit = backend->mr_chunk_bytes ? backend->mr_chunk_bytes : SIZE_MAX;
    for (size_t d = 0; d < backend->device_count; ++d) {
        uint64_t maximum = backend->devices[d].info.max_mr_size;
        if (!maximum) return jz_fail(error, EOPNOTSUPP, "rdma-mr-size-capability");
        if (maximum < limit) limit = (size_t)maximum;
    }
    /* Allocate every token and matrix first. A later failure leaves zero or
     * reachable partial tokens; the generic transaction always releases them. */
    for (size_t i = 0; i < count; ++i) {
        struct jz_rdma_registration *registration = calloc(1, sizeof(*registration));
        if (!registration) return jz_fail(error, ENOMEM, "rdma-registration-allocate");
        jobs[i].resource = registration;
        (void)atomic_fetch_add(&backend->registration_count, 1);
        int rc = jz_host_plan(&jobs[i].buffer, limit, &registration->chunks,
                               &registration->chunk_count, error);
        if (rc) return rc;
        if (registration->chunk_count > SIZE_MAX / backend->device_count / sizeof(*registration->mrs))
            return jz_fail(error, EOVERFLOW, "rdma-mr-matrix-size");
        registration->mrs = calloc(registration->chunk_count * backend->device_count,
                                     sizeof(*registration->mrs));
        if (!registration->mrs) return jz_fail(error, ENOMEM, "rdma-mr-matrix-allocate");
    }
    return 0;
}
static int register_batch(struct jz_backend *base, struct jz_memory_job *jobs,
    size_t count, struct jz_error *error)
{
    struct jz_rdma_backend *backend = (struct jz_rdma_backend *)base;
    if (!backend->driver.calls->reg_mr || !backend->driver.calls->dereg_mr)
        return jz_fail(error, EOPNOTSUPP, "rdma-registration-driver");
    int rc = create_records(backend, jobs, count, error);
    if (rc) return rc;
    for (size_t d = 0; d < backend->device_count; ++d)
        for (size_t i = 0; i < count; ++i) {
            struct jz_rdma_registration *registration = jobs[i].resource;
            int access = IBV_ACCESS_LOCAL_WRITE;
            if (jobs[i].remote_accessible) access |= IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
            for (size_t c = 0; c < registration->chunk_count; ++c) {
                const struct jz_host_chunk *chunk = &registration->chunks[c];
                rc = jz_rdma_driver_reg_mr(&backend->driver, backend->devices[d].pd,
                    (char *)jobs[i].buffer.addr + chunk->offset, chunk->length, access,
                    &registration->mrs[d*registration->chunk_count+c], error);
                if (rc) return rc;
            }
        }
    for (size_t i = 0; i < count; ++i) {
        const struct jz_rdma_registration *registration = jobs[i].resource;
        jz_host_location(&jobs[i].buffer, registration->chunks, registration->chunk_count,
                          jobs[i].location, jobs[i].resolved_location);
    }
    return 0;
}
static void release_batch(struct jz_backend *base, struct jz_memory_job *jobs, size_t count)
{
    struct jz_rdma_backend *backend = (struct jz_rdma_backend *)base;
    for (size_t i = count; i > 0; --i) {
        struct jz_memory_job *job = &jobs[i-1];
        struct jz_rdma_registration *registration = job->resource;
        if (!registration) continue;
        bool live = false;
        if (registration->mrs)
            for (size_t d = backend->device_count; d > 0; --d)
                for (size_t c = registration->chunk_count; c > 0; --c) {
                    struct ibv_mr **mr = &registration->mrs[(d-1)*registration->chunk_count+c-1];
                    if (!*mr) continue;
                    struct jz_error detail = {0};
                    int rc = jz_rdma_driver_dereg_mr(&backend->driver, *mr, &detail);
                    if (rc) {
                        live = true;
                        if (!job->cleanup_error.code) job->cleanup_error = detail;
                    } else *mr = NULL; /* Never read MR fields after successful dereg. */
                }
        if (live) continue;
        free(registration->mrs); free(registration->chunks); free(registration);
        job->resource = NULL;
        (void)atomic_fetch_sub(&backend->registration_count, 1);
    }
}
const struct jz_memory_backend_ops jz_rdma_memory_ops = {register_batch, release_batch};
