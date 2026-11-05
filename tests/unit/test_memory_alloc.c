#include "runtime/engine_internal.h"
#include "memory/internal.h"
#include "common/error.h"
#include "check.h"
#include <errno.h>
#include <stdlib.h>

/* Linker wrapping affects this test executable only, not the library ABI. */
void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void *__wrap_malloc(size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_realloc(void *, size_t);
static size_t fail_at, allocation;
static int live, cookie;
static bool fail(void)
{
    return fail_at && ++allocation == fail_at;
}
void *__wrap_malloc(size_t n)
{
    return fail() ? NULL : __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t size)
{
    return fail() ? NULL : __real_calloc(n, size);
}
void *__wrap_realloc(void *p, size_t n)
{
    return fail() ? NULL : __real_realloc(p, n);
}
static int prepare(struct jz_backend *b, struct jz_error *e)
{
    (void)b;
    (void)e;
    return 0;
}
static int release(struct jz_backend *b, struct jz_error *e)
{
    (void)b;
    (void)e;
    CHECK(!live);
    return 0;
}
static void dispose(struct jz_backend *b)
{
    (void)b;
}
static int reg(struct jz_backend *b, struct jz_memory_job *jobs, size_t count, struct jz_error *e)
{
    (void)b;
    (void)e;
    for (size_t i = 0; i < count; ++i) {
        jobs[i].resource = &cookie;
        ++live;
    }
    return 0;
}
static void unreg(struct jz_backend *b, struct jz_memory_job *jobs, size_t count)
{
    (void)b;
    for (size_t i = 0; i < count; ++i)
        if (jobs[i].resource) {
            jobs[i].resource = NULL;
            --live;
        }
}
static const struct jz_backend_ops lifecycle = {
    .prepare = prepare, .release = release, .dispose = dispose};
static const struct jz_memory_backend_ops memory_ops = {reg, unreg};
int main(void)
{
    struct jz_backend backend = {.kind = 700, .ops = &lifecycle, .memory_ops = &memory_ops};
    jz_engine *engine = NULL;
    CHECK(jz_engine_create(&engine, NULL) == 0 && jz_engine_attach(engine, &backend, NULL) == 0);
    CHECK(jz_engine_prepare(engine, NULL) == 0);
    char bytes[96];
    struct jz_memory_buffer buffers[] = {{bytes, 32}, {bytes + 32, 32}, {bytes + 64, 32}};
    struct jz_registration_result results[3];
    bool completed = false;
    for (size_t n = 1; n < 100; ++n) {
        allocation = 0;
        fail_at = n;
        int rc = jz_register_local_memory_batch(engine, buffers, 3, NULL, results, NULL);
        fail_at = 0;
        if (!rc) {
            completed = true;
            break;
        }
        CHECK(rc == -ENOMEM && !live && !engine->memory->count);
        for (size_t i = 0; i < 3; ++i)
            CHECK(results[i].state == JZ_REGISTRATION_NONE);
    }
    CHECK(completed && live == 3);
    void *bases[] = {bytes, bytes + 32, bytes + 64};
    completed = false;
    for (size_t n = 1; n < 100; ++n) {
        allocation = 0;
        fail_at = n;
        int rc = jz_unregister_local_memory_batch(engine, bases, 3, results, NULL);
        fail_at = 0;
        if (!rc) {
            completed = true;
            break;
        }
        CHECK(rc == -ENOMEM && live == 3 && engine->memory->count == 3);
        for (size_t i = 0; i < 3; ++i)
            CHECK(results[i].state == JZ_REGISTRATION_ACTIVE);
    }
    CHECK(completed && !live);
    CHECK(jz_engine_destroy(&engine, NULL) == 0);
    return 0;
}
