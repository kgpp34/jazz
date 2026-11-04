#include "jz/memory.h"
#include "jz/rdma.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int positive(const char *text, size_t *out)
{
    if (!*text || *text == '-')
        return -1;
    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno || *end || !value || value > SIZE_MAX)
        return -1;
    *out = (size_t)value;
    return 0;
}
static uint64_t nanoseconds(clockid_t clock)
{
    struct timespec time;
    if (clock_gettime(clock, &time)) {
        perror("clock_gettime");
        exit(1);
    }
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}
static int compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}
int main(int argc, char **argv)
{
    if (argc < 6 || argc > 7) {
        fprintf(stderr, "Usage: %s DEVICE cm|oob BYTES ITERATIONS serial|auto|parallel [warm]\n",
                argv[0]);
        return 2;
    }
    size_t length, count;
    if (positive(argv[3], &length) || positive(argv[4], &count) ||
        count > SIZE_MAX / sizeof(uint64_t))
        return 2;
    struct jz_rdma_config config;
    jz_rdma_config_init(&config);
    if (!strcmp(argv[2], "cm"))
        config.setup_modes = JZ_RDMA_SETUP_CM | JZ_RDMA_SETUP_OOB;
    else if (!strcmp(argv[2], "oob"))
        config.setup_modes = JZ_RDMA_SETUP_OOB;
    else
        return 2;
    if (!strcmp(argv[5], "serial"))
        config.registration_policy = JZ_RDMA_REG_SERIAL;
    else if (!strcmp(argv[5], "auto"))
        config.registration_policy = JZ_RDMA_REG_AUTO;
    else if (!strcmp(argv[5], "parallel"))
        config.registration_policy = JZ_RDMA_REG_PARALLEL;
    else
        return 2;
    if (argc == 7) {
        if (strcmp(argv[6], "warm"))
            return 2;
        config.pretouch_threshold_bytes = 0;
    }
    const char *devices[] = {argv[1]};
    config.devices = devices;
    config.device_count = 1;
    uint64_t *samples = calloc(count, sizeof(*samples));
    void *buffer = malloc(length);
    if (!samples || !buffer) {
        free(samples);
        free(buffer);
        perror("allocate");
        return 1;
    }
    /* This baseline uses already-touched HOST memory. It does not measure cold
     * page placement or prove a benefit from pre-touch on a multi-NIC host. */
    memset(buffer, 0, length);
    struct jz_memory_buffer description = {buffer, length};
    struct jz_registration_result result;
    struct jz_error error = {0};
    jz_engine *engine = NULL;
    int rc = jz_engine_create(&engine, &error);
    if (!rc)
        rc = jz_rdma_configure(engine, &config, &error);
    if (!rc)
        rc = jz_engine_prepare(engine, &error);
    uint64_t wall = nanoseconds(CLOCK_MONOTONIC), cpu = nanoseconds(CLOCK_PROCESS_CPUTIME_ID);
    for (size_t i = 0; !rc && i < count; ++i) {
        uint64_t start = nanoseconds(CLOCK_MONOTONIC);
        rc = jz_register_local_memory(engine, &description, NULL, &result, &error);
        if (!rc)
            rc = jz_unregister_local_memory(engine, buffer, &error);
        samples[i] = nanoseconds(CLOCK_MONOTONIC) - start;
    }
    cpu = nanoseconds(CLOCK_PROCESS_CPUTIME_ID) - cpu;
    wall = nanoseconds(CLOCK_MONOTONIC) - wall;
    if (!rc) {
        qsort(samples, count, sizeof(*samples), compare);
        puts("device,context_source,policy,bytes,iterations,pretouch_threshold,pair_p50_us,pair_p99_us,wall_"
             "us_per_pair,cpu_us_per_pair");
        printf("%s,%s,%s,%zu,%zu,%" PRIu64 ",%.3f,%.3f,%.3f,%.3f\n", argv[1], argv[2], argv[5],
               length, count, config.pretouch_threshold_bytes,
               (double)samples[(count - 1) / 2] / 1000.0,
               (double)samples[count - count / 100 - 1] / 1000.0,
               (double)wall / (double)count / 1000.0, (double)cpu / (double)count / 1000.0);
    } else
        fprintf(stderr, "registration benchmark: %s: %d\n", error.stage, error.system_errno);
    int cleanup = jz_engine_destroy(&engine, &error);
    if (cleanup) {
        fprintf(stderr, "cleanup retained buffer: %s: %d\n", error.stage, error.system_errno);
        /* Keep the mapping alive until process exit if the provider cannot clean up. */
        free(samples);
        return 1;
    }
    free(samples);
    free(buffer);
    return rc ? 1 : 0;
}
