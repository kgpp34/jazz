#include "transport/rdma/internal.h"
#include "check.h"
#include <pthread.h>

struct request {
    struct jz_rdma_backend *backend;
    atomic_int visits[2000];
};
static void visit(void *argument, size_t index)
{
    struct request *request = argument;
    CHECK(atomic_load(&request->backend->helpers_in_use) <=
          request->backend->max_registration_threads);
    CHECK(atomic_fetch_add(&request->visits[index], 1) == 0);
}
static void *run(void *argument)
{
    struct request *request = argument;
    jz_rdma_run_work(request->backend, 2000, true, visit, request);
    return NULL;
}
int main(void)
{
    struct jz_rdma_backend backend = {0};
    atomic_init(&backend.helpers_in_use, 0);
    for (unsigned limit = 0; limit <= 4; ++limit) {
        backend.max_registration_threads = limit;
        struct request requests[4] = {0};
        pthread_t callers[4];
        for (size_t i = 0; i < 4; ++i) {
            requests[i].backend = &backend;
            for (size_t j = 0; j < 2000; ++j)
                atomic_init(&requests[i].visits[j], 0);
            CHECK(pthread_create(&callers[i], NULL, run, &requests[i]) == 0);
        }
        for (size_t i = 0; i < 4; ++i) {
            CHECK(pthread_join(callers[i], NULL) == 0);
            for (size_t j = 0; j < 2000; ++j)
                CHECK(atomic_load(&requests[i].visits[j]) == 1);
        }
        CHECK(atomic_load(&backend.helpers_in_use) == 0);
    }
    return 0;
}
