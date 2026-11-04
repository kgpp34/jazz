#include "platform/host_memory.h"
#include "common/error.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

struct plan {
    struct jz_host_chunk *chunks;
    size_t count, capacity;
    uintptr_t base, end, covered;
    size_t limit;
};
static int append(struct plan *plan, uintptr_t start, size_t length, struct jz_error *error)
{
    if (plan->count == plan->capacity) {
        size_t capacity = plan->capacity ? plan->capacity * 2 : 8;
        if (capacity < plan->capacity || capacity > SIZE_MAX / sizeof(*plan->chunks))
            return jz_fail(error, EOVERFLOW, "host-chunk-count");
        struct jz_host_chunk *chunks = realloc(plan->chunks, capacity * sizeof(*chunks));
        if (!chunks) return jz_fail(error, ENOMEM, "host-chunks-allocate");
        plan->chunks = chunks; plan->capacity = capacity;
    }
    plan->chunks[plan->count++] = (struct jz_host_chunk){
        .offset = (size_t)(start - plan->base), .length = length };
    return 0;
}
static int span(struct plan *plan, uintptr_t start, uintptr_t end,
    size_t page_size, bool huge, bool flags_known, struct jz_error *error)
{
    if (end <= plan->base || start >= plan->end) return 0;
    if (start < plan->base) start = plan->base;
    if (end > plan->end) end = plan->end;
    if (start != plan->covered) return jz_fail(error, EFAULT, "host-mapping-hole");
    if (!flags_known || (huge && !page_size))
        return jz_fail(error, EOPNOTSUPP, "host-page-information");
    while (start < end) {
        size_t length = (size_t)(end - start);
        if (length > plan->limit) {
            length = plan->limit;
            if (huge) {
                /* Interior boundaries must not split a HugeTLB page. The
                 * first/last caller addresses are not silently widened. */
                uintptr_t boundary = start + length;
                boundary -= boundary % page_size;
                if (boundary <= start) return jz_fail(error, E2BIG, "host-hugepage-chunk-limit");
                length = (size_t)(boundary - start);
            }
        }
        int rc = append(plan, start, length, error);
        if (rc) return rc;
        start += length;
    }
    plan->covered = end;
    return 0;
}
int jz_host_plan_stream(FILE *stream, const struct jz_memory_buffer *buffer, size_t limit,
    struct jz_host_chunk **out, size_t *count, struct jz_error *error)
{
    if (!stream || !buffer || !buffer->addr || !buffer->length || !limit || !out || !count)
        return jz_fail(error, EINVAL, "host-plan-arguments");
    *out = NULL; *count = 0;
    uintptr_t base = (uintptr_t)buffer->addr;
    if (buffer->length > UINTPTR_MAX - base)
        return jz_fail(error, EOVERFLOW, "host-address-overflow");
    struct plan plan = {.base = base, .end = base+buffer->length, .covered = base, .limit = limit};
    uintptr_t start = 0, end = 0;
    size_t page_size = 0;
    bool huge = false, flags_known = false;
    char line[512];
    int rc = 0;
    while (fgets(line, sizeof(line), stream)) {
        uintptr_t a, b;
        char permissions[5];
        if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &a, &b, permissions) == 3) {
            if (end) {
                rc = span(&plan, start, end, page_size, huge, flags_known, error);
                if (rc || plan.covered == plan.end) break;
            }
            start = a; end = b; page_size = 0; huge = false; flags_known = false;
        } else if (!strncmp(line, "KernelPageSize:", 15)) {
            size_t kb;
            if (sscanf(line+15, "%zu", &kb) == 1 && kb <= SIZE_MAX/1024) page_size = kb*1024;
        } else if (!strncmp(line, "VmFlags:", 8)) {
            flags_known = true;
            /* Token boundaries distinguish ht from unrelated flag names. */
            const char *p = line+8;
            while (*p) {
                p += strspn(p, " \t\r\n");
                size_t n = strcspn(p, " \t\r\n");
                if (n == 2 && p[0] == 'h' && p[1] == 't') huge = true;
                p += n;
            }
        }
        if (!strchr(line, '\n')) {
            int c;
            while ((c = fgetc(stream)) != '\n' && c != EOF) { }
        }
    }
    if (!rc && plan.covered != plan.end && end)
        rc = span(&plan, start, end, page_size, huge, flags_known, error);
    if (!rc && ferror(stream)) rc = jz_fail(error, errno ? errno : EIO, "host-mapping-read");
    if (!rc && plan.covered != plan.end) rc = jz_fail(error, EFAULT, "host-mapping-hole");
    if (rc) { free(plan.chunks); return rc; }
    *out = plan.chunks; *count = plan.count;
    return 0;
}
int jz_host_plan(const struct jz_memory_buffer *buffer, size_t limit,
    struct jz_host_chunk **out, size_t *count, struct jz_error *error)
{
    FILE *stream = fopen("/proc/self/smaps", "r");
    if (!stream) return jz_fail(error, errno, "host-mapping-open");
    int rc = jz_host_plan_stream(stream, buffer, limit, out, count, error);
    (void)fclose(stream);
    return rc;
}
static int page_node(uintptr_t address, size_t page_size)
{
    void *page = (void *)(address - address % page_size);
    int node = -1;
    long rc = syscall(SYS_move_pages, 0, 1ul, &page, NULL, &node, 0);
    return rc == 0 && node >= 0 ? node : -1;
}
void jz_host_location(const struct jz_memory_buffer *buffer, const struct jz_host_chunk *chunks,
    size_t count, const char *requested, char out[JZ_MEMORY_LOCATION_MAX])
{
    if (strcmp(requested, "*")) {
        (void)snprintf(out, JZ_MEMORY_LOCATION_MAX, "%s", requested); return;
    }
    (void)snprintf(out, JZ_MEMORY_LOCATION_MAX, "*");
    long size = sysconf(_SC_PAGESIZE);
    if (size <= 0 || !count) return;
    int first = -1;
    /* Bounded residency sampling is a location hint, not a placement guarantee.
     * Lack of move_pages permission must not turn a valid MR into a failure. */
    size_t samples = count < 64 ? count : 64;
    for (size_t i = 0; i < samples; ++i) {
        int node = page_node((uintptr_t)buffer->addr+chunks[i].offset, (size_t)size);
        if (node < 0) return;
        if (first < 0) first = node;
        else if (node != first) {
            (void)snprintf(out, JZ_MEMORY_LOCATION_MAX, "cpu:mixed"); return;
        }
    }
    (void)snprintf(out, JZ_MEMORY_LOCATION_MAX, "cpu:%d", first);
}
