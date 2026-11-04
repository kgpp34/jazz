#ifndef JZ_MEMORY_BACKEND_H
#define JZ_MEMORY_BACKEND_H
#include "jz/memory.h"

struct jz_backend;
struct jz_memory_job {
    struct jz_memory_buffer buffer; /* Immutable caller range for this operation. */
    const char *location;           /* Immutable copied request label. */
    bool remote_accessible;        /* Logical access policy, no verbs flags here. */
    void *resource;                /* Backend-owned token, NULL only after cleanup. */
    char resolved_location[JZ_MEMORY_LOCATION_MAX]; /* Backend result, published under lock. */
    struct jz_error cleanup_error;  /* Per-entry cleanup failure; original error is separate. */
};
struct jz_memory_backend_ops {
    /* May return failure with some job.resource tokens still live. The runtime
     * will call release_batch for every attempted backend, including this one. */
    int (*register_batch)(struct jz_backend *, struct jz_memory_job *, size_t,
                           struct jz_error *);
    /* Must attempt every independent live token. Clear a token only after all
     * of its resources are gone; set the corresponding cleanup_error otherwise.
     * Must not allocate memory just to retry mandatory hardware cleanup. */
    void (*release_batch)(struct jz_backend *, struct jz_memory_job *, size_t);
};
#endif
