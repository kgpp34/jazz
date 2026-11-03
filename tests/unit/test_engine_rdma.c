#include "runtime/engine_internal.h"
#include "check.h"
#include <rdma/rdma_cma.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jazz_engine *prepare(unsigned modes, const char *name)
{
    const char *names[] = {name};
    struct jazz_engine_config config;
    jazz_engine_config_init(&config);
    config.setup_modes = modes; config.devices = names; config.device_count = 1;
    jazz_engine *engine = NULL;
    struct jazz_error error;
    CHECK(jazz_engine_create(&config, &engine, &error) == 0);
    int rc = jazz_engine_prepare(engine, &error);
    if (rc) fprintf(stderr, "prepare: %s: %d\n", error.stage, error.system_errno);
    CHECK(rc == 0);
    struct jazz_device_info info;
    CHECK(jazz_engine_query_device(engine, 0, &info, NULL) == 0);
    CHECK(!strcmp(info.name, name) && info.port_count);
    CHECK(engine->devices[0].pd->context == engine->devices[0].verbs);
    return engine;
}

int main(void)
{
    const char *ip = getenv("JAZZ_TEST_IP");
    if (!ip || !*ip) { puts("SKIP: set JAZZ_TEST_IP for real RDMA device validation"); return 77; }
    const char *name = getenv("JAZZ_TEST_DEVICE");
    if (!name || !*name) name = "rxe0";
    struct sockaddr_in addr = {.sin_family = AF_INET};
    CHECK(inet_pton(AF_INET, ip, &addr.sin_addr) == 1);
    for (int repeat = 0; repeat < 16; ++repeat) {
        jazz_engine *first = prepare(JAZZ_SETUP_CM | JAZZ_SETUP_OOB, name);
        jazz_engine *second = prepare(JAZZ_SETUP_CM | JAZZ_SETUP_OOB, name);
        CHECK(first->devices[0].verbs == second->devices[0].verbs);
        CHECK(first->devices[0].pd != second->devices[0].pd);
        struct rdma_cm_id *id = NULL;
        CHECK(rdma_create_id(NULL, &id, NULL, RDMA_PS_TCP) == 0);
        CHECK(rdma_bind_addr(id, (struct sockaddr *)&addr) == 0);
        CHECK(id->verbs == first->devices[0].pd->context);
        CHECK(rdma_destroy_id(id) == 0);
        CHECK(jazz_engine_destroy(&first, NULL) == 0);
        /* 一个 Engine 归还 CM 列表，不得破坏另一个 Engine 的借用 context。 */
        struct ibv_device_attr actual;
        CHECK(ibv_query_device(second->devices[0].verbs, &actual) == 0);
        CHECK(jazz_engine_destroy(&second, NULL) == 0);
        jazz_engine *oob = prepare(JAZZ_SETUP_OOB, name);
        CHECK(jazz_engine_destroy(&oob, NULL) == 0);
    }
    puts("PASS: engine lifetimes, independent PDs, CM context identity, OOB-only");
    return 0;
}
