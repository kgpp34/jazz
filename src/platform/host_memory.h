#ifndef JZ_HOST_MEMORY_H
#define JZ_HOST_MEMORY_H
#include "jz/memory.h"
#include <stdio.h>

struct jz_host_chunk {
    size_t offset; /* Relative to the exact caller range; no permission expansion. */
    size_t length; /* At most the common MR limit, never zero. */
};
/* Reads mapping page information without touching payload. Caller keeps mapping
 * stable; this is not a memory-map lock or a substitute for provider validation. */
int jz_host_plan(const struct jz_memory_buffer *, size_t limit,
    struct jz_host_chunk **, size_t *, struct jz_error *);
/* Stream seam permits deterministic normal/HugeTLB/mixed mapping tests. */
int jz_host_plan_stream(FILE *, const struct jz_memory_buffer *, size_t limit,
    struct jz_host_chunk **, size_t *, struct jz_error *);
void jz_host_location(const struct jz_memory_buffer *, const struct jz_host_chunk *, size_t,
    const char *requested, char out[JZ_MEMORY_LOCATION_MAX]);
#endif
