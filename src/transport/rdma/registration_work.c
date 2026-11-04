#include "transport/rdma/internal.h"
#include <pthread.h>

struct work {
    struct jz_rdma_backend *backend; /* Lifetime extends through every join. */
    atomic_size_t next;              /* Each work index executes exactly once. */
    size_t count;
    jz_rdma_work_fn function;
    void *argument;
};
static void consume(struct work *work)
{
    for (;;) {
        size_t index = atomic_fetch_add(&work->next, 1);
        if (index >= work->count)
            return;
        work->function(work->argument, index);
    }
}
static void *helper(void *argument)
{
    struct work *work = argument;
    consume(work);
    (void)atomic_fetch_sub(&work->backend->helpers_in_use, 1);
    return NULL;
}
static bool take_helper(struct jz_rdma_backend *backend)
{
    unsigned used = atomic_load(&backend->helpers_in_use);
    while (used < backend->max_registration_threads)
        if (atomic_compare_exchange_weak(&backend->helpers_in_use, &used, used + 1))
            return true;
    return false;
}
void jz_rdma_run_work(struct jz_rdma_backend *backend, size_t count, bool parallel,
                      jz_rdma_work_fn function, void *argument)
{
    if (!count)
        return;
    if (!parallel) {
        for (size_t i = 0; i < count; ++i)
            function(argument, i);
        return;
    }
    struct work work = {
        .backend = backend, .count = count, .function = function, .argument = argument};
    atomic_init(&work.next, 0);
    pthread_t threads[64];
    size_t started = 0;
    while (started < backend->max_registration_threads && started + 1 < count &&
           take_helper(backend)) {
        int rc = pthread_create(&threads[started], NULL, helper, &work);
        if (rc) {
            (void)atomic_fetch_sub(&backend->helpers_in_use, 1);
            break; /* Optional helpers exhausted: caller still executes all work. */
        }
        ++started;
    }
    consume(&work);
    for (size_t i = 0; i < started; ++i)
        (void)pthread_join(threads[i], NULL);
}
