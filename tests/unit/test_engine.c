#include "runtime/engine_internal.h"
#include "check.h"
#include <endian.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Two fake devices validate ownership and failures, not real verbs or multi-NIC transfers. */
static struct ibv_device devices[2];
static struct ibv_context contexts[2];
static struct ibv_device *device_list[] = { &devices[0], &devices[1], NULL };
static struct ibv_context *context_list[] = { &contexts[0], &contexts[1], NULL };
static int enumerations, list_releases, opens, closes, live_pds, allocations;
static int fail_alloc_at, fail_query, fail_close, fail_pd_free, fail_open, empty;
static const char *const names[] = { "fake0", "fake1" };

static void reset(void)
{
    CHECK(live_pds == 0 && opens == closes);
    enumerations = list_releases = opens = closes = allocations = 0;
    fail_alloc_at = fail_query = fail_close = fail_pd_free = fail_open = empty = 0;
    memset(contexts, 0, sizeof(contexts));
    for (size_t i = 0; i < 2; ++i) contexts[i].device = &devices[i];
}

static struct ibv_context **cm_list(int *count)
{ ++enumerations; *count = empty ? 0 : 2; return context_list; }
static void cm_free(struct ibv_context **list)
{ CHECK(list == context_list && live_pds == 0); ++list_releases; }
static struct ibv_device **verbs_list(int *count)
{ ++enumerations; *count = empty ? 0 : 2; return device_list; }
static void verbs_free(struct ibv_device **list)
{ CHECK(list == device_list && live_pds == 0 && opens == closes); ++list_releases; }
static const char *device_name(struct ibv_device *device)
{ return names[device == &devices[0] ? 0 : 1]; }
static struct ibv_context *open_device(struct ibv_device *device)
{
    if (fail_open) { errno = EACCES; return NULL; }
    ++opens;
    return &contexts[device == &devices[0] ? 0 : 1];
}
static int close_device(struct ibv_context *context)
{
    CHECK(context == &contexts[0] || context == &contexts[1]);
    if (fail_close) return EIO;
    ++closes;
    return 0;
}
static int query_device(struct ibv_context *context, struct ibv_device_attr *attr)
{
    (void)context;
    if (fail_query == 1) { errno = EACCES; return -1; }
    *attr = (struct ibv_device_attr){ .node_guid = htobe64(123),
        .phys_port_cnt = 1, .max_mr_size = UINT64_MAX,
        .max_qp_wr = 64, .max_cqe = 256, .max_sge = 4,
        .max_qp_rd_atom = 8, .max_qp_init_rd_atom = 4 };
    if (fail_query == 4) attr->max_qp_rd_atom = 0;
    return 0;
}
static int query_port(struct ibv_context *context, uint8_t port, struct ibv_port_attr *attr)
{
    (void)context;
    CHECK(port == 1);
    if (fail_query == 2) return EIO;
    *attr = (struct ibv_port_attr){ .state = IBV_PORT_ACTIVE,
        .link_layer = IBV_LINK_LAYER_ETHERNET, .active_mtu = IBV_MTU_1024,
        .max_msg_sz = 65536, .gid_tbl_len = 3 };
    return 0;
}
static int query_gid(struct ibv_context *context, uint32_t port, uint32_t gid,
                       struct ibv_gid_entry *entry, uint32_t flags)
{
    (void)context; (void)flags;
    CHECK(port == 1 && gid < 3);
    if (fail_query == 3) return EACCES;
    if (!gid) return ENODATA; /* An empty GID slot must not fail the entire device query. */
    entry->gid_type = gid == 1 || fail_query == 5 ? IBV_GID_TYPE_ROCE_V1 : IBV_GID_TYPE_ROCE_V2;
    entry->gid.global.interface_id = 1;
    return 0;
}
static struct ibv_pd *alloc_pd(struct ibv_context *context)
{
    ++allocations;
    if (allocations == fail_alloc_at) { errno = ENOMEM; return NULL; }
    struct ibv_pd *pd = calloc(1, sizeof(*pd));
    CHECK(pd);
    pd->context = context;
    ++live_pds;
    return pd;
}
static int dealloc_pd(struct ibv_pd *pd)
{
    CHECK(live_pds > 0 && pd->context);
    if (fail_pd_free) return EBUSY;
    --live_pds;
    free(pd);
    return 0;
}
static const struct jz_rdma_device_ops fake_ops = {
    .cm_list = cm_list, .cm_free = cm_free, .verbs_list = verbs_list,
    .verbs_free = verbs_free, .name = device_name, .open = open_device,
    .close = close_device, .query_device = query_device, .query_port = query_port,
    .query_gid = query_gid, .alloc_pd = alloc_pd, .dealloc_pd = dealloc_pd
};

static jz_engine *create(unsigned modes)
{
    struct jz_engine_config config;
    jz_engine_config_init(&config);
    config.setup_modes = modes;
    config.devices = names;
    config.device_count = 2;
    jz_engine *engine = NULL;
    CHECK(jz_engine_create_with_ops(&config, &fake_ops, &engine, NULL) == 0);
    CHECK(enumerations == 0 && allocations == 0);
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
    CHECK(state(engine) == JZ_ENGINE_READY && live_pds == 2);
    CHECK(engine->devices[0].pd->context == &contexts[0]);
    CHECK(engine->devices[0].pd != engine->devices[1].pd);
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
    /* Preparation failures roll back to CREATED and allow another prepare. */
    for (int fault = 1; fault <= 5; ++fault) {
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

int main(void)
{
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
    struct jz_engine_config config;
    jz_engine_config_init(&config);
    CHECK(jz_engine_create(&config, &engine, NULL) == -EINVAL && !engine);
    const char *duplicates[] = { "fake0", "fake0" };
    config.devices = duplicates; config.device_count = 2;
    CHECK(jz_engine_create(&config, &engine, NULL) == -EEXIST && !engine);
    const char *missing[] = { "fake0", "missing" };
    config.devices = missing;
    CHECK(jz_engine_create_with_ops(&config, &fake_ops, &engine, NULL) == 0);
    CHECK(jz_engine_prepare(engine, NULL) == -ENODEV && live_pds == 0);
    CHECK(jz_engine_destroy(&engine, NULL) == 0);

    reset();
    char name[] = "fake0";
    const char *copied[] = {name};
    config.devices = copied; config.device_count = 1;
    CHECK(jz_engine_create_with_ops(&config, &fake_ops, &engine, NULL) == 0);
    name[0] = 'x';
    CHECK(jz_engine_prepare(engine, NULL) == 0);
    CHECK(jz_engine_destroy(&engine, NULL) == 0);
    reset();
    return 0;
}
