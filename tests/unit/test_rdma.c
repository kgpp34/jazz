#include "transport/rdma/internal.h"
#include "check.h"
#include "memory/internal.h"
#include "runtime/engine_internal.h"
#include <endian.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Two fake devices validate ownership and failures, not real verbs or multi-NIC transfers. */
static struct ibv_device devices[2];
static struct ibv_context contexts[2];
static struct ibv_device *device_list[] = {&devices[0], &devices[1], NULL};
static struct ibv_context *context_list[] = {&contexts[0], &contexts[1], NULL};
static int enumerations, list_releases, opens, closes, live_pds, allocations;
static atomic_int live_mrs, mr_calls, mr_frees;
static int fail_mr_at, fail_mr_free, small_mr_caps, short_mr;
static atomic_int remote_mrs, local_mrs;
static size_t chunk_limit;
static bool force_warm;
static enum jz_rdma_registration_policy policy;
static struct jz_rdma_backend *observed_backend;
static int fail_alloc_at, fail_query, fail_close, fail_pd_free, fail_open, empty, fail_list;
static const char *const names[] = {"fake0", "fake1"};

static void reset(void)
{
    CHECK(live_pds == 0 && opens == closes && atomic_load(&live_mrs) == 0);
    atomic_store(&mr_calls, 0);
    atomic_store(&mr_frees, 0);
    fail_mr_at = fail_mr_free = small_mr_caps = short_mr = 0;
    chunk_limit = 0;
    atomic_store(&remote_mrs, 0);
    atomic_store(&local_mrs, 0);
    force_warm = false;
    policy = JZ_RDMA_REG_AUTO;
    observed_backend = NULL;
    enumerations = list_releases = opens = closes = allocations = 0;
    fail_alloc_at = fail_query = fail_close = fail_pd_free = fail_open = empty = fail_list = 0;
    memset(contexts, 0, sizeof(contexts));
    for (size_t i = 0; i < 2; ++i)
        contexts[i].device = &devices[i];
}

static struct ibv_context **cm_list(int *count)
{
    ++enumerations;
    if (fail_list) {
        errno = EACCES;
        return NULL;
    }
    *count = empty ? 0 : 2;
    return context_list;
}
static void cm_free(struct ibv_context **list)
{
    CHECK(list == context_list && live_pds == 0);
    ++list_releases;
}
static struct ibv_device **verbs_list(int *count)
{
    ++enumerations;
    if (fail_list) {
        errno = EACCES;
        return NULL;
    }
    *count = empty ? 0 : 2;
    return device_list;
}
static void verbs_free(struct ibv_device **list)
{
    CHECK(list == device_list && live_pds == 0 && opens == closes);
    ++list_releases;
}
static const char *device_name(struct ibv_device *device)
{
    return names[device == &devices[0] ? 0 : 1];
}
static struct ibv_context *open_device(struct ibv_device *device)
{
    if (fail_open) {
        errno = EACCES;
        return NULL;
    }
    ++opens;
    return &contexts[device == &devices[0] ? 0 : 1];
}
static int close_device(struct ibv_context *context)
{
    CHECK(context == &contexts[0] || context == &contexts[1]);
    if (fail_close)
        return EIO;
    ++closes;
    return 0;
}
static int query_device(struct ibv_context *context, struct ibv_device_attr *attr)
{
    if (fail_query == 6)
        return -ENOSPC;
    if (fail_query == 7)
        return -1; /* Driver must supply EIO without errno. */
    if (fail_query == 1) {
        errno = EACCES;
        return -1;
    }
    *attr = (struct ibv_device_attr){.node_guid = htobe64(123),
                                     .phys_port_cnt = 1,
                                     .max_mr_size = UINT64_MAX,
                                     .max_qp_wr = 64,
                                     .max_cqe = 256,
                                     .max_sge = 4,
                                     .max_qp_rd_atom = 8,
                                     .max_qp_init_rd_atom = 4};
    if (small_mr_caps)
        attr->max_mr_size = context == &contexts[0] ? 4 : 8;
    if (fail_query == 4)
        attr->max_qp_rd_atom = 0;
    return 0;
}
static int query_port(struct ibv_context *context, uint8_t port, struct ibv_port_attr *attr)
{
    (void)context;
    CHECK(port == 1);
    if (fail_query == 2)
        return EIO;
    *attr = (struct ibv_port_attr){.state = IBV_PORT_ACTIVE,
                                   .link_layer = IBV_LINK_LAYER_ETHERNET,
                                   .active_mtu = IBV_MTU_1024,
                                   .max_msg_sz = 65536,
                                   .gid_tbl_len = 3};
    return 0;
}
static int query_gid(struct ibv_context *context, uint32_t port, uint32_t gid,
                     struct ibv_gid_entry *entry, uint32_t flags)
{
    (void)context;
    (void)flags;
    CHECK(port == 1 && gid < 3);
    if (fail_query == 3)
        return EACCES;
    if (!gid)
        return ENODATA; /* An empty GID slot must not fail the entire device query. */
    entry->gid_type = gid == 1 || fail_query == 5 ? IBV_GID_TYPE_ROCE_V1 : IBV_GID_TYPE_ROCE_V2;
    entry->gid.global.interface_id = 1;
    return 0;
}
static struct ibv_pd *alloc_pd(struct ibv_context *context)
{
    ++allocations;
    if (allocations == fail_alloc_at) {
        errno = ENOMEM;
        return NULL;
    }
    struct ibv_pd *pd = calloc(1, sizeof(*pd));
    CHECK(pd);
    pd->context = context;
    ++live_pds;
    return pd;
}
static int dealloc_pd(struct ibv_pd *pd)
{
    CHECK(live_pds > 0 && pd->context && atomic_load(&live_mrs) == 0);
    if (fail_pd_free == 1 || (fail_pd_free == 2 && pd->context == &contexts[1]))
        return EBUSY;
    --live_pds;
    free(pd);
    return 0;
}
static struct ibv_mr *register_mr(struct ibv_pd *pd, void *addr, size_t length, int access)
{
    if (observed_backend)
        CHECK(atomic_load(&observed_backend->helpers_in_use) <=
              observed_backend->max_registration_threads);
    int n = atomic_fetch_add(&mr_calls, 1) + 1;
    if (n == fail_mr_at) {
        errno = ENOMEM;
        return NULL;
    }
    CHECK(access == IBV_ACCESS_LOCAL_WRITE ||
          access == (IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE));
    if (access & IBV_ACCESS_REMOTE_READ)
        (void)atomic_fetch_add(&remote_mrs, 1);
    else
        (void)atomic_fetch_add(&local_mrs, 1);
    struct ibv_mr *mr = calloc(1, sizeof(*mr));
    CHECK(mr);
    *mr = (struct ibv_mr){.context = pd->context,
                          .pd = pd,
                          .addr = addr,
                          .length = length,
                          .lkey = (uint32_t)n,
                          .rkey = (uint32_t)n};
    if (short_mr)
        --mr->length;
    (void)atomic_fetch_add(&live_mrs, 1);
    return mr;
}
static int deregister_mr(struct ibv_mr *mr)
{
    CHECK(mr && mr->pd && atomic_load(&live_mrs) > 0);
    if (fail_mr_free)
        return EBUSY;
    (void)atomic_fetch_add(&mr_frees, 1);
    (void)atomic_fetch_sub(&live_mrs, 1);
    free(mr);
    return 0;
}
static const struct jz_rdma_calls fake_ops = {.cm_list = cm_list,
                                              .cm_free = cm_free,
                                              .verbs_list = verbs_list,
                                              .verbs_free = verbs_free,
                                              .name = device_name,
                                              .open = open_device,
                                              .close = close_device,
                                              .query_device = query_device,
                                              .query_port = query_port,
                                              .query_gid = query_gid,
                                              .alloc_pd = alloc_pd,
                                              .dealloc_pd = dealloc_pd,
                                              .reg_mr = register_mr,
                                              .dereg_mr = deregister_mr};

static jz_engine *create(unsigned modes)
{
    struct jz_rdma_config config;
    jz_rdma_config_init(&config);
    config.setup_modes = modes;
    config.mr_chunk_bytes = chunk_limit;
    config.registration_policy = policy;
    config.max_registration_threads = 2;
    if (force_warm)
        config.pretouch_threshold_bytes = 0;
    config.devices = names;
    config.device_count = 2;
    jz_engine *engine = NULL;
    CHECK(jz_engine_create(&engine, NULL) == 0);
    CHECK(jz_rdma_configure_with_calls(engine, &config, &fake_ops, NULL) == 0);
    CHECK(enumerations == 0 && allocations == 0);
    CHECK(jz_rdma_configure_with_calls(engine, &config, &fake_ops, NULL) == -EALREADY);
    return engine;
}
static enum jz_engine_state state(jz_engine *engine)
{
    enum jz_engine_state result;
    CHECK(jz_engine_get_state(engine, &result) == 0);
    return result;
}

static void normal(unsigned modes)
{
    reset();
    jz_engine *engine = create(modes);
    struct jz_rdma_device_info info = {0};
    CHECK(jz_rdma_query_device(engine, 0, &info, NULL) == -EAGAIN);
    CHECK(jz_engine_prepare(engine, NULL) == 0);
    CHECK(jz_rdma_source_release(&jz_rdma_backend_find(engine)->source, NULL) == -EBUSY);
    CHECK(state(engine) == JZ_ENGINE_READY && live_pds == 2);
    CHECK(jz_rdma_configure(engine, NULL, NULL) == -EBUSY);
    CHECK(jz_rdma_backend_find(engine)->devices[0].pd->context == &contexts[0]);
    CHECK(jz_rdma_backend_find(engine)->devices[0].pd !=
          jz_rdma_backend_find(engine)->devices[1].pd);
    CHECK(jz_engine_prepare(engine, NULL) == -EALREADY);
    CHECK(jz_rdma_query_device(engine, 0, &info, NULL) == 0);
    CHECK(!strcmp(info.name, "fake0") && info.guid == 123 && info.max_read_outstanding == 4);
    info.name[0] = 'x';
    CHECK(jz_rdma_query_device(engine, 0, &info, NULL) == 0 && info.name[0] == 'f');
    CHECK(jz_rdma_query_device(engine, 2, &info, NULL) == -ERANGE);
    struct jz_rdma_port_info port;
    CHECK(jz_rdma_query_port(engine, 0, 1, &port, NULL) == 0);
    CHECK(port.active_mtu == 1024 && port.roce_v2_gid_count == 1);
    CHECK(jz_rdma_query_port(engine, 0, 0, &port, NULL) == -ERANGE);
    CHECK(jz_engine_destroy(&engine, NULL) == 0 && !engine);
    CHECK(jz_engine_destroy(&engine, NULL) == 0);
    CHECK(!live_pds && list_releases == 1);
    CHECK(opens == ((modes & JZ_RDMA_SETUP_CM) ? 0 : 2) && closes == opens);
}

static void failures(unsigned modes)
{
    reset();
    jz_engine *unlisted = create(modes);
    fail_list = 1;
    struct jz_error list_error;
    CHECK(jz_engine_prepare(unlisted, &list_error) == -EACCES);
    CHECK(!strcmp(list_error.stage, "enumerate-devices") && list_releases == 0);
    CHECK(state(unlisted) == JZ_ENGINE_CREATED);
    fail_list = 0;
    CHECK(jz_engine_destroy(&unlisted, NULL) == 0);
    /* Preparation failures roll back to CREATED and allow another prepare. */
    for (int fault = 1; fault <= 7; ++fault) {
        reset();
        jz_engine *engine = create(modes);
        fail_query = fault;
        struct jz_error error;
        CHECK(jz_engine_prepare(engine, &error) < 0 && error.code < 0);
        CHECK(state(engine) == JZ_ENGINE_CREATED && !live_pds && list_releases == 1);
        fail_query = 0;
        CHECK(jz_engine_prepare(engine, NULL) == 0);
        CHECK(jz_engine_destroy(&engine, NULL) == 0);
        CHECK(list_releases == 2 && opens == closes);
    }
    reset();
    jz_engine *engine = create(modes);
    fail_alloc_at = 2;
    fail_pd_free = 1;
    struct jz_error error;
    CHECK(jz_engine_prepare(engine, &error) == -ENOMEM);
    CHECK(!strcmp(error.stage, "allocate-pd"));
    CHECK(state(engine) == JZ_ENGINE_CLEANUP_REQUIRED && live_pds == 1);
    CHECK(list_releases == 0); /* Do not release the parent provider reference first. */
    CHECK(jz_engine_prepare(engine, NULL) == -EBUSY);
    CHECK(jz_engine_destroy(&engine, &error) == -EBUSY && engine);
    CHECK(!strcmp(error.stage, "deallocate-pd"));
    fail_pd_free = 0;
    CHECK(jz_engine_destroy(&engine, NULL) == 0 && !engine);
    CHECK(list_releases == 1 && !live_pds && opens == closes);

    reset();
    engine = create(modes);
    empty = 1;
    CHECK(jz_engine_prepare(engine, &error) == -ENODEV);
    CHECK(state(engine) == JZ_ENGINE_CREATED && list_releases == 1);
    CHECK(jz_engine_destroy(&engine, NULL) == 0);
}

static void memory_registration(unsigned modes)
{
    char bytes[32];
    struct jz_memory_buffer buffers[] = {{bytes, 16}, {bytes + 16, 16}};
    struct jz_registration_result results[2];
    reset();
    small_mr_caps = 1;
    jz_engine *engine = create(modes);
    CHECK(jz_engine_prepare(engine, NULL) == 0);
    CHECK(jz_register_local_memory_batch(engine, buffers, 2, NULL, results, NULL) == 0);
    CHECK(atomic_load(&live_mrs) == 16); /* Common 4-byte chunks on both devices. */
    struct jz_memory_record *record =
        (struct jz_memory_record *)jz_interval_find(engine->memory->root, (uintptr_t)bytes);
    struct jz_rdma_registration *registration = record->bindings[0].resource;
    CHECK(registration->chunk_count == 4);
    for (size_t d = 0; d < 2; ++d)
        for (size_t c = 0; c < 4; ++c) {
            struct ibv_mr *mr = registration->mrs[d * 4 + c];
            CHECK(mr->length == 4 && mr->addr == bytes + c * 4);
            CHECK(mr->pd == jz_rdma_backend_find(engine)->devices[d].pd);
        }
    CHECK(jz_unregister_local_memory(engine, bytes + 1, NULL) == -ENOENT);
    CHECK(jz_engine_destroy(&engine, NULL) == 0);
    CHECK(atomic_load(&mr_frees) == 16 && !atomic_load(&live_mrs));

    /* Every creation position must roll back all prior device/chunk resources. */
    for (int failure = 1; failure <= 16; ++failure) {
        reset();
        chunk_limit = 4;
        fail_mr_at = failure;
        engine = create(modes);
        CHECK(jz_engine_prepare(engine, NULL) == 0);
        struct jz_error error;
        CHECK(jz_register_local_memory_batch(engine, buffers, 2, NULL, results, &error) == -ENOMEM);
        CHECK(error.system_errno == ENOMEM && !strcmp(error.stage, "register-mr"));
        CHECK(!atomic_load(&live_mrs) && engine->memory->count == 0);
        CHECK(results[0].state == JZ_REGISTRATION_NONE && results[1].state == JZ_REGISTRATION_NONE);
        CHECK(jz_engine_destroy(&engine, NULL) == 0);
    }
    reset();
    short_mr = 1;
    engine = create(modes);
    CHECK(jz_engine_prepare(engine, NULL) == 0);
    CHECK(jz_register_local_memory(engine, buffers, NULL, results, NULL) == -EIO);
    CHECK(!atomic_load(&live_mrs) && results[0].state == JZ_REGISTRATION_NONE);
    CHECK(jz_engine_destroy(&engine, NULL) == 0);
    reset();
    engine = create(modes);
    CHECK(jz_engine_prepare(engine, NULL) == 0);
    struct jz_memory_options local_options;
    jz_memory_options_init(&local_options);
    local_options.remote_accessible = false;
    CHECK(jz_register_local_memory(engine, buffers, &local_options, results, NULL) == 0);
    CHECK(atomic_load(&local_mrs) > 0 && !atomic_load(&remote_mrs));
    CHECK(jz_engine_destroy(&engine, NULL) == 0);
    reset();
    chunk_limit = 4;
    fail_mr_at = 6;
    fail_mr_free = 1;
    engine = create(modes);
    CHECK(jz_engine_prepare(engine, NULL) == 0);
    struct jz_error error;
    CHECK(jz_register_local_memory_batch(engine, buffers, 2, NULL, results, &error) == -ENOMEM);
    CHECK(atomic_load(&live_mrs) == 5 && live_pds == 2);
    CHECK(jz_engine_destroy(&engine, &error) == -EBUSY && engine && live_pds == 2);
    CHECK(!strcmp(error.stage, "deregister-mr"));
    fail_mr_free = 0;
    CHECK(jz_engine_destroy(&engine, NULL) == 0 && !atomic_load(&live_mrs) && !live_pds);
    reset();
}

static void parallel_pretouch(void)
{
    char *bytes = malloc(8205);
    CHECK(bytes);
    memset(bytes, 0x5a, 8205);
    struct jz_memory_buffer buffer = {bytes, 8205};
    struct jz_registration_result result;
    reset();
    chunk_limit = 4096;
    force_warm = true;
    policy = JZ_RDMA_REG_PARALLEL;
    jz_engine *engine = create(JZ_RDMA_SETUP_CM | JZ_RDMA_SETUP_OOB);
    CHECK(jz_engine_prepare(engine, NULL) == 0);
    observed_backend = jz_rdma_backend_find(engine);
    CHECK(jz_register_local_memory(engine, &buffer, NULL, &result, NULL) == 0);
    struct jz_memory_record *record =
        (struct jz_memory_record *)jz_interval_find(engine->memory->root, (uintptr_t)bytes);
    struct jz_rdma_registration *registration = record->bindings[0].resource;
    CHECK(registration->warm_count > 0 && atomic_load(&mr_frees) > 0);
    CHECK(atomic_load(&live_mrs) == (int)(registration->chunk_count * 2));
    for (size_t i = 0; i < 8205; ++i)
        CHECK((unsigned char)bytes[i] == 0x5a);
    CHECK(atomic_load(&observed_backend->helpers_in_use) == 0);
    int calls = atomic_load(&mr_calls);
    CHECK(jz_engine_destroy(&engine, NULL) == 0 && !atomic_load(&live_mrs));
    for (int failure = 1; failure <= calls; ++failure) {
        reset();
        chunk_limit = 4096;
        force_warm = true;
        policy = JZ_RDMA_REG_PARALLEL;
        fail_mr_at = failure;
        engine = create(JZ_RDMA_SETUP_OOB);
        CHECK(jz_engine_prepare(engine, NULL) == 0);
        observed_backend = jz_rdma_backend_find(engine);
        CHECK(jz_register_local_memory(engine, &buffer, NULL, &result, NULL) == -ENOMEM);
        CHECK(!atomic_load(&live_mrs) && result.state == JZ_REGISTRATION_NONE);
        CHECK(atomic_load(&observed_backend->helpers_in_use) == 0);
        CHECK(jz_engine_destroy(&engine, NULL) == 0);
    }
    reset();
    chunk_limit = 4096;
    force_warm = true;
    policy = JZ_RDMA_REG_PARALLEL;
    fail_mr_free = 1;
    engine = create(JZ_RDMA_SETUP_OOB);
    CHECK(jz_engine_prepare(engine, NULL) == 0);
    struct jz_error error;
    CHECK(jz_register_local_memory(engine, &buffer, NULL, &result, &error) == -EBUSY);
    CHECK(!strcmp(error.stage, "deregister-mr") &&
          result.state == JZ_REGISTRATION_CLEANUP_REQUIRED);
    CHECK(atomic_load(&live_mrs) > 0);
    CHECK(jz_engine_destroy(&engine, NULL) == -EBUSY && live_pds == 2);
    fail_mr_free = 0;
    CHECK(jz_engine_destroy(&engine, NULL) == 0 && !atomic_load(&live_mrs));
    free(bytes);
    reset();
}
struct concurrent_mr {
    jz_engine *engine;
    char *base;
    int rc;
};
static void *register_concurrent(void *arg)
{
    struct concurrent_mr *request = arg;
    struct jz_memory_buffer buffer = {request->base, 4096};
    struct jz_registration_result result;
    request->rc = jz_register_local_memory(request->engine, &buffer, NULL, &result, NULL);
    return NULL;
}
static void concurrent_mrs(void)
{
    reset();
    policy = JZ_RDMA_REG_PARALLEL;
    chunk_limit = 1024;
    char *pool = malloc(8 * 4096);
    CHECK(pool);
    jz_engine *engine = create(JZ_RDMA_SETUP_OOB);
    CHECK(jz_engine_prepare(engine, NULL) == 0);
    observed_backend = jz_rdma_backend_find(engine);
    pthread_t threads[8];
    struct concurrent_mr requests[8];
    for (size_t i = 0; i < 8; ++i) {
        requests[i] = (struct concurrent_mr){engine, pool + i * 4096, -1};
        CHECK(pthread_create(&threads[i], NULL, register_concurrent, &requests[i]) == 0);
    }
    for (size_t i = 0; i < 8; ++i)
        CHECK(pthread_join(threads[i], NULL) == 0 && requests[i].rc == 0);
    CHECK(atomic_load(&live_mrs) >= 64 && atomic_load(&observed_backend->helpers_in_use) == 0);
    CHECK(jz_engine_destroy(&engine, NULL) == 0 && !atomic_load(&live_mrs));
    free(pool);
    reset();
}

int main(void)
{
    parallel_pretouch();
    concurrent_mrs();
    memory_registration(JZ_RDMA_SETUP_CM | JZ_RDMA_SETUP_OOB);
    memory_registration(JZ_RDMA_SETUP_OOB);
    normal(JZ_RDMA_SETUP_CM | JZ_RDMA_SETUP_OOB);
    normal(JZ_RDMA_SETUP_CM);
    normal(JZ_RDMA_SETUP_OOB);
    failures(JZ_RDMA_SETUP_CM | JZ_RDMA_SETUP_OOB);
    failures(JZ_RDMA_SETUP_OOB);

    reset();
    jz_engine *engine = create(JZ_RDMA_SETUP_OOB);
    fail_open = 1;
    struct jz_error error;
    CHECK(jz_engine_prepare(engine, &error) == -EACCES);
    CHECK(!strcmp(error.stage, "open-device") && list_releases == 1);
    fail_open = 0;
    CHECK(jz_engine_prepare(engine, NULL) == 0);
    fail_close = 1;
    CHECK(jz_engine_destroy(&engine, &error) == -EIO && engine);
    CHECK(live_pds == 0 && list_releases == 1 && state(engine) == JZ_ENGINE_CLEANUP_REQUIRED);
    fail_close = 0;
    CHECK(jz_engine_destroy(&engine, NULL) == 0 && list_releases == 2);

    reset();
    struct jz_rdma_config config;
    jz_rdma_config_init(&config);
    CHECK(jz_engine_create(&engine, NULL) == 0);
    CHECK(jz_rdma_configure(engine, &config, NULL) == -EINVAL);
    const char *duplicates[] = {"fake0", "fake0"};
    config.devices = duplicates;
    config.device_count = 2;
    CHECK(jz_rdma_configure(engine, &config, NULL) == -EEXIST);
    const char *missing[] = {"fake0", "missing"};
    config.devices = missing;
    CHECK(jz_rdma_configure_with_calls(engine, &config, &fake_ops, NULL) == 0);
    CHECK(jz_engine_prepare(engine, NULL) == -ENODEV && live_pds == 0);
    CHECK(jz_engine_destroy(&engine, NULL) == 0);

    reset();
    engine = create(JZ_RDMA_SETUP_OOB);
    CHECK(jz_engine_prepare(engine, NULL) == 0);
    fail_pd_free = 2;
    CHECK(jz_engine_destroy(&engine, NULL) == -EBUSY);
    CHECK(live_pds == 1 && closes == 1 && !list_releases);
    CHECK(jz_rdma_backend_find(engine)->source.leases == 1);
    fail_pd_free = 0;
    CHECK(jz_engine_destroy(&engine, NULL) == 0 && closes == 2 && list_releases == 1);

    reset();
    char name[] = "fake0";
    const char *copied[] = {name};
    config.devices = copied;
    config.device_count = 1;
    CHECK(jz_engine_create(&engine, NULL) == 0);
    CHECK(jz_rdma_configure_with_calls(engine, &config, &fake_ops, NULL) == 0);
    name[0] = 'x';
    CHECK(jz_engine_prepare(engine, NULL) == 0);
    CHECK(jz_engine_destroy(&engine, NULL) == 0);
    reset();
    return 0;
}
