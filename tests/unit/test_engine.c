#include "runtime/engine_internal.h"
#include "common/error.h"
#include "check.h"
#include <errno.h>
#include <string.h>

/* Two different lifecycle strategies exercise the runtime without RDMA headers.
 * Stack-owned fixtures keep counters readable after dispose; production dispose
 * frees its object. Neither fixture is a production transport. */
struct fixture {
    struct jz_backend base;
    int live;
    int prepares, releases, disposals;
    int prepare_error, release_error;
    unsigned token;
};
static unsigned trace[64];
static size_t trace_count;
static int prepare_a(struct jz_backend *base, struct jz_error *error)
{
    struct fixture *f = (struct fixture *)base;
    CHECK(!f->live && !f->disposals);
    ++f->prepares;
    f->live = 1; /* Failure deliberately leaves a partial resource. */
    trace[trace_count++] = f->token;
    return f->prepare_error ? jz_fail(error, f->prepare_error, "fixture-prepare") : 0;
}
static int prepare_b(struct jz_backend *base, struct jz_error *error)
{
    return prepare_a(base, error);
}
static int release_fixture(struct jz_backend *base, struct jz_error *error)
{
    struct fixture *f = (struct fixture *)base;
    CHECK(!f->disposals);
    ++f->releases;
    trace[trace_count++] = f->token + 10;
    if (f->release_error) return jz_fail(error, f->release_error, "fixture-release");
    f->live = 0;
    return 0;
}
static void dispose_fixture(struct jz_backend *base)
{
    struct fixture *f = (struct fixture *)base;
    CHECK(!f->live && !f->disposals);
    ++f->disposals;
}
static const struct jz_backend_ops ops_a = {prepare_a, release_fixture, dispose_fixture};
static const struct jz_backend_ops ops_b = {prepare_b, release_fixture, dispose_fixture};
static enum jz_engine_state state(jz_engine *engine)
{
    enum jz_engine_state result;
    CHECK(jz_engine_get_state(engine, &result) == 0);
    return result;
}
static void lifecycle(int prepare_failure, int rollback_failure)
{
    trace_count = 0;
    struct fixture a = {.base = {101, &ops_a}, .token = 1};
    struct fixture b = {.base = {202, &ops_b}, .token = 2};
    struct fixture c = {.base = {303, &ops_a}, .token = 3};
    jz_engine *engine = NULL;
    struct jz_error error;
    CHECK(jz_engine_create(&engine, &error) == 0 && !error.code);
    CHECK(jz_engine_prepare(engine, &error) == -ENODEV);
    CHECK(jz_engine_attach(engine, &a.base, NULL) == 0);
    CHECK(jz_engine_attach(engine, &b.base, NULL) == 0);
    CHECK(jz_engine_attach(engine, &c.base, NULL) == 0);
    CHECK(jz_engine_attach(engine, &a.base, NULL) == -EALREADY);
    CHECK(jz_engine_find(engine, 202) == &b.base);
    if (prepare_failure) b.prepare_error = ENOMEM;
    if (rollback_failure) a.release_error = EBUSY;
    int rc = jz_engine_prepare(engine, &error);
    if (prepare_failure) {
        CHECK(rc == -ENOMEM && error.code == rc && error.system_errno == ENOMEM);
        CHECK(!strcmp(error.stage, "fixture-prepare"));
        CHECK(trace_count == 4 && trace[0] == 1 && trace[1] == 2 && trace[2] == 12 && trace[3] == 11);
        CHECK(!c.prepares && !b.live && !a.disposals && !b.disposals);
        if (rollback_failure) {
            CHECK(state(engine) == JZ_ENGINE_CLEANUP_REQUIRED && a.live);
            CHECK(jz_engine_prepare(engine, NULL) == -EBUSY);
            CHECK(jz_engine_attach(engine, &c.base, NULL) == -EBUSY);
            CHECK(jz_engine_destroy(&engine, &error) == -EBUSY && engine);
            CHECK(c.disposals == 1 && b.disposals == 1 && !a.disposals);
            CHECK(error.system_errno == EBUSY);
            a.release_error = 0;
            CHECK(jz_engine_destroy(&engine, NULL) == 0 && !engine);
            CHECK(a.disposals == 1 && b.disposals == 1 && c.disposals == 1);
            return;
        }
        CHECK(state(engine) == JZ_ENGINE_CREATED && !a.live);
        b.prepare_error = 0;
        CHECK(jz_engine_prepare(engine, &error) == 0 && !error.code);
    } else CHECK(rc == 0);
    CHECK(state(engine) == JZ_ENGINE_READY && a.live && b.live && c.live);
    CHECK(jz_engine_prepare(engine, NULL) == -EALREADY);
    CHECK(jz_engine_attach(engine, &a.base, NULL) == -EBUSY);
    /* One failure must not block independent backends or double-dispose them. */
    b.release_error = EIO;
    CHECK(jz_engine_destroy(&engine, &error) == -EIO && engine);
    CHECK(a.disposals == 1 && c.disposals == 1 && !b.disposals);
    int a_releases = a.releases, c_releases = c.releases;
    b.release_error = 0;
    CHECK(jz_engine_destroy(&engine, &error) == 0 && !engine && !error.code);
    CHECK(b.disposals == 1 && a.releases == a_releases && c.releases == c_releases);
}
int main(void)
{
    lifecycle(0, 0);
    lifecycle(1, 0);
    lifecycle(1, 1);
    jz_engine *engine = NULL;
    CHECK(jz_engine_create(NULL, NULL) == -EINVAL);
    CHECK(jz_engine_destroy(NULL, NULL) == -EINVAL);
    CHECK(jz_engine_destroy(&engine, NULL) == 0);
    CHECK(jz_engine_prepare(NULL, NULL) == -EINVAL);
    CHECK(jz_engine_get_state(NULL, NULL) == -EINVAL);
    CHECK(jz_engine_create(&engine, NULL) == 0);
    CHECK(jz_engine_create(&engine, NULL) == -EINVAL);
    CHECK(jz_engine_attach(engine, NULL, NULL) == -EINVAL);
    CHECK(jz_engine_destroy(&engine, NULL) == 0);
    /* Destruction of a never-prepared backend also uses the release contract. */
    struct fixture f = {.base = {101, &ops_a}, .token = 1};
    CHECK(jz_engine_create(&engine, NULL) == 0);
    CHECK(jz_engine_attach(engine, &f.base, NULL) == 0);
    CHECK(jz_engine_destroy(&engine, NULL) == 0);
    CHECK(!f.prepares && f.releases == 1 && f.disposals == 1);
    struct fixture many[17] = {0};
    CHECK(jz_engine_create(&engine, NULL) == 0);
    trace_count = 0;
    for (unsigned i = 0; i < 17; ++i) {
        many[i].base = (struct jz_backend){.kind = 1000 + i, .ops = &ops_a};
        int rc = jz_engine_attach(engine, &many[i].base, NULL);
        CHECK(rc == (i < 16 ? 0 : -ENOSPC));
    }
    CHECK(jz_engine_destroy(&engine, NULL) == 0);
    CHECK(!many[16].disposals && !many[16].releases);
    for (unsigned i = 0; i < 16; ++i) CHECK(many[i].disposals == 1);
    return 0;
}
