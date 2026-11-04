#include "runtime/engine_internal.h"
#include "memory/internal.h"
#include "common/error.h"
#include "check.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* This backend exercises registry transactions only; no RDMA is simulated. */
struct fake {
    struct jz_backend base;
    atomic_int live, calls;
    int fail_at, fail_release;
    pthread_mutex_t gate;
    pthread_cond_t condition;
    void *blocked_base;
    bool entered, proceed;
};
static int prepare(struct jz_backend *base, struct jz_error *error)
{
    (void)base;
    (void)error;
    return 0;
}
static int release(struct jz_backend *base, struct jz_error *error)
{
    (void)error;
    CHECK(atomic_load(&((struct fake *)base)->live) == 0);
    return 0;
}
static void dispose(struct jz_backend *base)
{
    (void)base;
}
static int register_batch(struct jz_backend *base, struct jz_memory_job *jobs, size_t count,
                          struct jz_error *error)
{
    struct fake *fake = (struct fake *)base;
    for (size_t i = 0; i < count; ++i) {
        if (jobs[i].buffer.addr == fake->blocked_base) {
            CHECK(pthread_mutex_lock(&fake->gate) == 0);
            fake->entered = true;
            CHECK(pthread_cond_broadcast(&fake->condition) == 0);
            while (!fake->proceed)
                CHECK(pthread_cond_wait(&fake->condition, &fake->gate) == 0);
            CHECK(pthread_mutex_unlock(&fake->gate) == 0);
        }
        int n = atomic_fetch_add(&fake->calls, 1) + 1;
        if (fake->fail_at == n)
            return jz_fail(error, ENOMEM, "fixture-register");
        jobs[i].resource = malloc(1);
        CHECK(jobs[i].resource);
        (void)atomic_fetch_add(&fake->live, 1);
        memcpy(jobs[i].resolved_location, "cpu:2", 6);
    }
    return 0;
}
static void release_batch(struct jz_backend *base, struct jz_memory_job *jobs, size_t count)
{
    struct fake *fake = (struct fake *)base;
    for (size_t i = 0; i < count; ++i) {
        if (!jobs[i].resource)
            continue;
        if (fake->fail_release) {
            (void)jz_fail(&jobs[i].cleanup_error, EBUSY, "fixture-cleanup");
            continue;
        }
        free(jobs[i].resource);
        jobs[i].resource = NULL;
        (void)atomic_fetch_sub(&fake->live, 1);
    }
}
static const struct jz_backend_ops lifecycle = {prepare, release, dispose};
static const struct jz_memory_backend_ops memory_ops = {register_batch, release_batch};
static jz_engine *create(struct fake *fake, unsigned kind)
{
    fake->base = (struct jz_backend){.kind = kind, .ops = &lifecycle, .memory_ops = &memory_ops};
    CHECK(pthread_mutex_init(&fake->gate, NULL) == 0);
    CHECK(pthread_cond_init(&fake->condition, NULL) == 0);
    jz_engine *engine = NULL;
    CHECK(jz_engine_create(&engine, NULL) == 0);
    CHECK(jz_engine_attach(engine, &fake->base, NULL) == 0);
    CHECK(jz_engine_prepare(engine, NULL) == 0);
    return engine;
}
static void fixture_dispose(struct fake *fake)
{
    CHECK(atomic_load(&fake->live) == 0);
    CHECK(pthread_mutex_destroy(&fake->gate) == 0);
    CHECK(pthread_cond_destroy(&fake->condition) == 0);
}
static void transaction(void)
{
    char buffer[96];
    struct jz_memory_buffer buffers[] = {{buffer, 32}, {buffer + 32, 32}, {buffer + 64, 32}};
    struct fake fake = {0};
    jz_engine *engine = create(&fake, 901);
    struct jz_registration_result results[3];
    fake.fail_at = 3;
    fake.fail_release = 1;
    struct jz_error error;
    CHECK(jz_register_local_memory_batch(engine, buffers, 3, NULL, results, &error) == -ENOMEM);
    CHECK(!strcmp(error.stage, "fixture-register"));
    CHECK(results[0].state == JZ_REGISTRATION_CLEANUP_REQUIRED);
    CHECK(results[1].state == JZ_REGISTRATION_CLEANUP_REQUIRED &&
          results[2].state == JZ_REGISTRATION_NONE);
    struct jz_local_memory_info info;
    CHECK(jz_query_local_memory(engine, buffer, &info, NULL) == 0);
    CHECK(info.state == JZ_MEMORY_CLEANUP_REQUIRED && info.registration_error.code == -ENOMEM);
    CHECK(info.cleanup_error.code == -EBUSY);
    CHECK(jz_register_local_memory(engine, buffers, NULL, results, NULL) == -EEXIST);
    CHECK(results[0].state == JZ_REGISTRATION_NONE && atomic_load(&fake.calls) == 3);
    CHECK(jz_unregister_local_memory(engine, buffer + 1, NULL) == -ENOENT);
    CHECK(jz_engine_destroy(&engine, NULL) == -EBUSY && engine);
    CHECK(engine->state == JZ_ENGINE_CLEANUP_REQUIRED && atomic_load(&fake.live) == 2);
    CHECK(jz_register_local_memory(engine, buffers, NULL, results, NULL) == -EBUSY);
    fake.fail_release = 0;
    CHECK(jz_engine_destroy(&engine, NULL) == 0 && !engine);
    fixture_dispose(&fake);

    fake = (struct fake){0};
    engine = create(&fake, 902);
    fake.fail_at = 2;
    CHECK(jz_register_local_memory_batch(engine, buffers, 3, NULL, results, NULL) == -ENOMEM);
    for (size_t i = 0; i < 3; ++i)
        CHECK(results[i].state == JZ_REGISTRATION_NONE);
    CHECK(atomic_load(&fake.live) == 0 && !engine->memory->count);
    fake.fail_at = 0;
    CHECK(jz_register_local_memory_batch(engine, buffers, 3, NULL, results, NULL) == 0);
    CHECK(jz_query_local_memory(engine, buffer, &info, NULL) == 0);
    CHECK(info.remote_accessible && !strcmp(info.location, "cpu:2"));
    struct jz_memory_buffer overlap = {buffer + 1, 2};
    CHECK(jz_register_local_memory(engine, &overlap, NULL, results, NULL) == -EEXIST);
    void *bases[] = {buffer + 1, buffer + 64, buffer};
    CHECK(jz_unregister_local_memory_batch(engine, bases, 3, results, NULL) == -ENOENT);
    CHECK(results[1].state == JZ_REGISTRATION_NONE && results[2].state == JZ_REGISTRATION_NONE);
    CHECK(atomic_load(&fake.live) == 1);
    CHECK(jz_engine_destroy(&engine, NULL) == 0);
    fixture_dispose(&fake);
}
struct thread_request {
    jz_engine *engine;
    struct jz_memory_buffer buffer;
    int rc;
};
static void *thread_register(void *arg)
{
    struct thread_request *request = arg;
    struct jz_registration_result result;
    request->rc = jz_register_local_memory(request->engine, &request->buffer, NULL, &result, NULL);
    return NULL;
}
static void concurrency(void)
{
    char buffers[32][32];
    struct fake fake = {0};
    jz_engine *engine = create(&fake, 903);
    fake.blocked_base = buffers[0];
    struct thread_request first = {engine, {buffers[0], 32}, -1};
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, thread_register, &first) == 0);
    CHECK(pthread_mutex_lock(&fake.gate) == 0);
    while (!fake.entered)
        CHECK(pthread_cond_wait(&fake.condition, &fake.gate) == 0);
    CHECK(pthread_mutex_unlock(&fake.gate) == 0);
    struct jz_local_memory_info info;
    CHECK(jz_query_local_memory(engine, buffers[0], &info, NULL) == 0 &&
          info.state == JZ_MEMORY_REGISTERING);
    CHECK(jz_unregister_local_memory(engine, buffers[0], NULL) == -EBUSY);
    struct jz_registration_result result;
    CHECK(jz_register_local_memory(engine, &first.buffer, NULL, &result, NULL) == -EEXIST);
    /* A provider blocked on one address must not hold the global index lock. */
    struct jz_memory_buffer independent = {buffers[1], 32};
    CHECK(jz_register_local_memory(engine, &independent, NULL, &result, NULL) == 0);
    CHECK(pthread_mutex_lock(&fake.gate) == 0);
    fake.proceed = true;
    CHECK(pthread_cond_broadcast(&fake.condition) == 0);
    CHECK(pthread_mutex_unlock(&fake.gate) == 0);
    CHECK(pthread_join(thread, NULL) == 0 && first.rc == 0);
    pthread_t threads[30];
    struct thread_request requests[30];
    for (size_t i = 0; i < 30; ++i) {
        requests[i] = (struct thread_request){engine, {buffers[i + 2], 32}, -1};
        CHECK(pthread_create(&threads[i], NULL, thread_register, &requests[i]) == 0);
    }
    for (size_t i = 0; i < 30; ++i)
        CHECK(pthread_join(threads[i], NULL) == 0 && requests[i].rc == 0);
    CHECK(engine->memory->count == 32 && atomic_load(&fake.live) == 32);
    CHECK(jz_engine_destroy(&engine, NULL) == 0);
    fixture_dispose(&fake);
}
static int check_tree(struct jz_interval *node, uintptr_t low, uintptr_t high)
{
    if (!node)
        return 0;
    CHECK(node->start >= low && node->end <= high && node->start < node->end);
    int a = check_tree(node->left, low, node->start);
    int b = check_tree(node->right, node->end, high);
    CHECK(abs(a - b) <= 1 && node->height == 1 + (a > b ? a : b));
    return node->height;
}
static void index_test(void)
{
    struct jz_interval nodes[1000] = {0}, *root = NULL;
    for (size_t i = 0; i < 1000; ++i) {
        nodes[i].start = i * 4 + 4;
        nodes[i].end = i * 4 + 6;
        root = jz_interval_insert(root, &nodes[i]);
        (void)check_tree(root, 0, UINTPTR_MAX);
    }
    for (size_t i = 0; i < 1000; ++i) {
        size_t k = (i * 617) % 1000;
        CHECK(jz_interval_find(root, nodes[k].start) == &nodes[k]);
        CHECK(jz_interval_overlap(root, nodes[k].start, nodes[k].end) == &nodes[k]);
        root = jz_interval_remove(root, nodes[k].start);
        (void)check_tree(root, 0, UINTPTR_MAX);
    }
    CHECK(!root);
}
static void invalid(void)
{
    struct fake fake = {0};
    jz_engine *engine = create(&fake, 904);
    char bytes[8];
    struct jz_registration_result result[2];
    struct jz_memory_buffer buffers[] = {{bytes, 8}, {bytes + 1, 1}};
    CHECK(jz_register_local_memory_batch(engine, buffers, 2, NULL, result, NULL) == -EEXIST);
    CHECK(atomic_load(&fake.calls) == 0);
    buffers[0] = (struct jz_memory_buffer){(void *)(UINTPTR_MAX - 1), 4};
    CHECK(jz_register_local_memory(engine, buffers, NULL, result, NULL) == -EOVERFLOW);
    buffers[0] = (struct jz_memory_buffer){bytes, 0};
    CHECK(jz_register_local_memory(engine, buffers, NULL, result, NULL) == -EINVAL);
    buffers[0].length = 8;
    struct jz_memory_options options;
    jz_memory_options_init(&options);
    options.remote_accessible = false;
    CHECK(jz_register_local_memory(engine, buffers, &options, result, NULL) == 0);
    struct jz_local_memory_info info;
    CHECK(jz_query_local_memory(engine, bytes, &info, NULL) == 0 && !info.remote_accessible);
    void *duplicates[] = {bytes, bytes};
    CHECK(jz_unregister_local_memory_batch(engine, duplicates, 2, result, NULL) == -EINVAL);
    CHECK(atomic_load(&fake.live) == 1);
    CHECK(jz_engine_destroy(&engine, NULL) == 0);
    fixture_dispose(&fake);
}
static void multiple_backends(void)
{
    struct fake a = {0}, b = {0};
    a.base = (struct jz_backend){.kind = 910, .ops = &lifecycle, .memory_ops = &memory_ops};
    b.base = (struct jz_backend){.kind = 911, .ops = &lifecycle, .memory_ops = &memory_ops};
    CHECK(pthread_mutex_init(&a.gate, NULL) == 0 && pthread_cond_init(&a.condition, NULL) == 0);
    CHECK(pthread_mutex_init(&b.gate, NULL) == 0 && pthread_cond_init(&b.condition, NULL) == 0);
    jz_engine *engine = NULL;
    CHECK(jz_engine_create(&engine, NULL) == 0);
    CHECK(jz_engine_attach(engine, &a.base, NULL) == 0 &&
          jz_engine_attach(engine, &b.base, NULL) == 0);
    CHECK(jz_engine_prepare(engine, NULL) == 0);
    char bytes[64];
    struct jz_memory_buffer buffers[] = {{bytes, 32}, {bytes + 32, 32}};
    struct jz_registration_result results[2];
    b.fail_at = 2;
    b.fail_release = 1;
    CHECK(jz_register_local_memory_batch(engine, buffers, 2, NULL, results, NULL) == -ENOMEM);
    CHECK(atomic_load(&a.live) == 0 && atomic_load(&b.live) == 1);
    CHECK(results[0].state == JZ_REGISTRATION_CLEANUP_REQUIRED &&
          results[1].state == JZ_REGISTRATION_NONE);
    b.fail_release = 0;
    CHECK(jz_engine_destroy(&engine, NULL) == 0);
    fixture_dispose(&a);
    fixture_dispose(&b);
}

int main(void)
{
    index_test();
    transaction();
    concurrency();
    invalid();
    multiple_backends();
    return 0;
}
