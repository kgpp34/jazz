#include "transport/rdma/internal.h"
#include "check.h"
#include "memory/internal.h"
#include "runtime/engine_internal.h"
#include <rdma/rdma_cma.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jz_engine *prepare(unsigned modes, const char *name)
{
    const char *names[] = {name};
    struct jz_rdma_config config;
    jz_rdma_config_init(&config);
    config.mr_chunk_bytes = 4096;
    config.registration_policy = JZ_RDMA_REG_PARALLEL;
    config.pretouch_threshold_bytes = 0;
    config.max_registration_threads = 2;
    config.setup_modes = modes;
    config.devices = names;
    config.device_count = 1;
    jz_engine *engine = NULL;
    struct jz_error error;
    CHECK(jz_engine_create(&engine, &error) == 0);
    CHECK(jz_rdma_configure(engine, &config, &error) == 0);
    int rc = jz_engine_prepare(engine, &error);
    if (rc)
        fprintf(stderr, "prepare: %s: %d\n", error.stage, error.system_errno);
    CHECK(rc == 0);
    struct jz_rdma_device_info info;
    CHECK(jz_rdma_query_device(engine, 0, &info, NULL) == 0);
    CHECK(!strcmp(info.name, name) && info.port_count);
    CHECK(jz_rdma_backend_find(engine)->devices[0].pd->context ==
          jz_rdma_backend_find(engine)->devices[0].lease.verbs);
    return engine;
}

int main(void)
{
    const char *ip = getenv("JZ_TEST_IP");
    if (!ip || !*ip) {
        puts("SKIP: set JZ_TEST_IP for real RDMA device validation");
        return 77;
    }
    const char *name = getenv("JZ_TEST_DEVICE");
    if (!name || !*name)
        name = "rxe0";
    struct sockaddr_in addr = {.sin_family = AF_INET};
    CHECK(inet_pton(AF_INET, ip, &addr.sin_addr) == 1);
    for (int repeat = 0; repeat < 16; ++repeat) {
        jz_engine *first = prepare(JZ_RDMA_SETUP_CM | JZ_RDMA_SETUP_OOB, name);
        jz_engine *second = prepare(JZ_RDMA_SETUP_CM | JZ_RDMA_SETUP_OOB, name);
        CHECK(jz_rdma_backend_find(first)->devices[0].lease.verbs ==
              jz_rdma_backend_find(second)->devices[0].lease.verbs);
        CHECK(jz_rdma_backend_find(first)->devices[0].pd !=
              jz_rdma_backend_find(second)->devices[0].pd);
        struct rdma_cm_id *id = NULL;
        CHECK(rdma_create_id(NULL, &id, NULL, RDMA_PS_TCP) == 0);
        CHECK(rdma_bind_addr(id, (struct sockaddr *)&addr) == 0);
        CHECK(id->verbs == jz_rdma_backend_find(first)->devices[0].pd->context);
        CHECK(rdma_destroy_id(id) == 0);
        char *a = malloc(8205), *b = malloc(5000);
        CHECK(a && b);
        struct jz_memory_buffer buffers[] = {{a, 8205}, {b, 5000}};
        struct jz_registration_result results[2];
        CHECK(jz_register_local_memory_batch(first, buffers, 2, NULL, results, NULL) == 0);
        CHECK(jz_register_local_memory_batch(second, buffers, 2, NULL, results, NULL) == 0);
        struct jz_memory_record *record =
            (struct jz_memory_record *)jz_interval_find(first->memory->root, (uintptr_t)a);
        struct jz_rdma_registration *registration = record->bindings[0].resource;
        CHECK(registration->chunk_count >= 3 && registration->warm_count > 0);
        for (size_t c = 0; c < registration->warm_count; ++c)
            CHECK(!registration->temporary[c]);
        size_t total = 0;
        for (size_t c = 0; c < registration->chunk_count; ++c) {
            CHECK(registration->mrs[c]->pd == jz_rdma_backend_find(first)->devices[0].pd);
            CHECK(registration->mrs[c]->length <= 4096);
            total += registration->mrs[c]->length;
        }
        CHECK(total == 8205);
        CHECK(jz_engine_destroy(&first, NULL) == 0);
        /* Releasing one engine CM list must not invalidate another engine borrowed context. */
        struct ibv_device_attr actual;
        CHECK(ibv_query_device(jz_rdma_backend_find(second)->devices[0].lease.verbs, &actual) == 0);
        CHECK(jz_unregister_local_memory(second, a, NULL) == 0);
        CHECK(jz_engine_destroy(&second, NULL) == 0);
        free(a);
        free(b);
        jz_engine *oob = prepare(JZ_RDMA_SETUP_OOB, name);
        char local[32];
        struct jz_memory_buffer only = {local, sizeof(local)};
        CHECK(jz_register_local_memory(oob, &only, NULL, results, NULL) == 0);
        CHECK(jz_engine_destroy(&oob, NULL) == 0);
    }
    puts("PASS: engine lifetimes, independent PDs, CM context identity, OOB-only, real MR batches "
         "and teardown");
    return 0;
}
