#include "runtime/engine_internal.h"
#include "check.h"
#include <rdma/rdma_cma.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jz_engine *prepare(unsigned modes, const char *name)
{
    const char *names[] = {name};
    struct jz_engine_config config;
    jz_engine_config_init(&config);
    config.setup_modes = modes; config.devices = names; config.device_count = 1;
    jz_engine *engine = NULL;
    struct jz_error error;
    CHECK(jz_engine_create(&config, &engine, &error) == 0);
    int rc = jz_engine_prepare(engine, &error);
    if (rc) fprintf(stderr, "prepare: %s: %d\n", error.stage, error.system_errno);
    CHECK(rc == 0);
    struct jz_rdma_device_info info;
    CHECK(jz_rdma_query_device(engine, 0, &info, NULL) == 0);
    CHECK(!strcmp(info.name, name) && info.port_count);
    CHECK(engine->devices[0].pd->context == engine->devices[0].verbs);
    return engine;
}

int main(void)
{
    const char *ip = getenv("JZ_TEST_IP");
    if (!ip || !*ip) { puts("SKIP: set JZ_TEST_IP for real RDMA device validation"); return 77; }
    const char *name = getenv("JZ_TEST_DEVICE");
    if (!name || !*name) name = "rxe0";
    struct sockaddr_in addr = {.sin_family = AF_INET};
    CHECK(inet_pton(AF_INET, ip, &addr.sin_addr) == 1);
    for (int repeat = 0; repeat < 16; ++repeat) {
        jz_engine *first = prepare(JZ_RDMA_SETUP_CM | JZ_RDMA_SETUP_OOB, name);
        jz_engine *second = prepare(JZ_RDMA_SETUP_CM | JZ_RDMA_SETUP_OOB, name);
        CHECK(first->devices[0].verbs == second->devices[0].verbs);
        CHECK(first->devices[0].pd != second->devices[0].pd);
        struct rdma_cm_id *id = NULL;
        CHECK(rdma_create_id(NULL, &id, NULL, RDMA_PS_TCP) == 0);
        CHECK(rdma_bind_addr(id, (struct sockaddr *)&addr) == 0);
        CHECK(id->verbs == first->devices[0].pd->context);
        CHECK(rdma_destroy_id(id) == 0);
        CHECK(jz_engine_destroy(&first, NULL) == 0);
        /* Releasing one engine CM list must not invalidate another engine borrowed context. */
        struct ibv_device_attr actual;
        CHECK(ibv_query_device(second->devices[0].verbs, &actual) == 0);
        CHECK(jz_engine_destroy(&second, NULL) == 0);
        jz_engine *oob = prepare(JZ_RDMA_SETUP_OOB, name);
        CHECK(jz_engine_destroy(&oob, NULL) == 0);
    }
    puts("PASS: engine lifetimes, independent PDs, CM context identity, OOB-only");
    return 0;
}
