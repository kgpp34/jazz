#include "runtime/engine_internal.h"
#include <rdma/rdma_cma.h>
#include <errno.h>
#include <endian.h>
#include <stdlib.h>
#include <string.h>

static int system_query_port(struct ibv_context *context, uint8_t port,
                              struct ibv_port_attr *attr)
{
    return ibv_query_port(context, port, attr);
}

const struct jze_device_ops jze_system_ops = {
    .cm_list = rdma_get_devices, .cm_free = rdma_free_devices,
    .verbs_list = ibv_get_device_list, .verbs_free = ibv_free_device_list,
    .name = ibv_get_device_name, .open = ibv_open_device, .close = ibv_close_device,
    .query_device = ibv_query_device, .query_port = system_query_port,
    .query_gid = ibv_query_gid_ex, .alloc_pd = ibv_alloc_pd, .dealloc_pd = ibv_dealloc_pd
};

/* 不同 verbs 调用可能返回正 errno 或 -1/errno，保留真实原因。 */
static int call_error(int rc)
{
    return rc > 0 ? rc : (rc == -1 ? (errno ? errno : EIO) : -rc);
}

static int inspect_device(jazz_engine *engine, struct jze_device *device,
                           struct jazz_error *error)
{
    const struct jze_device_ops *ops = engine->ops;
    struct ibv_device_attr attr = {0};
    errno = 0;
    int rc = ops->query_device(device->verbs, &attr);
    if (rc) return jze_fail(error, call_error(rc), "query-device");
    if (!attr.phys_port_cnt || attr.max_qp_wr <= 0 || attr.max_cqe <= 0 ||
        attr.max_sge <= 0 || attr.max_qp_rd_atom <= 0 || attr.max_qp_init_rd_atom <= 0)
        return jze_fail(error, EOPNOTSUPP, "device-capabilities");
    device->info = (struct jazz_device_info){
        .guid = be64toh(attr.node_guid), .max_mr_size = attr.max_mr_size,
        .max_qp_wr = (uint32_t)attr.max_qp_wr, .max_cqe = (uint32_t)attr.max_cqe,
        .max_sge = (uint32_t)attr.max_sge,
        .max_read_outstanding = (uint32_t)attr.max_qp_init_rd_atom,
        .max_read_responder = (uint32_t)attr.max_qp_rd_atom,
        .port_count = attr.phys_port_cnt
    };
    memcpy(device->info.name, device->name, strlen(device->name) + 1);
    device->ports = calloc(attr.phys_port_cnt, sizeof(*device->ports));
    if (!device->ports) return jze_fail(error, ENOMEM, "port-table-allocate");
    bool usable = false;
    for (unsigned port = 1; port <= attr.phys_port_cnt; ++port) {
        struct ibv_port_attr pa = {0};
        errno = 0;
        rc = ops->query_port(device->verbs, (uint8_t)port, &pa);
        if (rc) return jze_fail(error, call_error(rc), "query-port");
        struct jazz_port_info *info = &device->ports[port - 1u];
        *info = (struct jazz_port_info){ .port = (uint8_t)port,
            .active = pa.state == IBV_PORT_ACTIVE,
            .ethernet = pa.link_layer == IBV_LINK_LAYER_ETHERNET,
            .max_message_size = pa.max_msg_sz };
        if (pa.active_mtu >= IBV_MTU_256 && pa.active_mtu <= IBV_MTU_4096)
            info->active_mtu = 128u << (unsigned)pa.active_mtu;
        if (!info->active || !info->ethernet) continue;
        if (pa.gid_tbl_len < 0) return jze_fail(error, EIO, "gid-table-length");
        for (int gid = 0; gid < pa.gid_tbl_len; ++gid) {
            struct ibv_gid_entry entry = {0};
            errno = 0;
            rc = ops->query_gid(device->verbs, port, (uint32_t)gid, &entry, 0);
            if (rc) {
                int reason = call_error(rc);
                if (reason == ENODATA || reason == ENOENT) continue;
                return jze_fail(error, reason, "query-gid-type");
            }
            if (entry.gid_type == IBV_GID_TYPE_ROCE_V2 &&
                (entry.gid.global.subnet_prefix || entry.gid.global.interface_id))
                ++info->roce_v2_gid_count;
        }
        usable |= info->roce_v2_gid_count != 0;
    }
    return usable ? 0 : jze_fail(error, EOPNOTSUPP, "no-active-rocev2-port");
}

int jze_devices_prepare(jazz_engine *engine, struct jazz_error *error)
{
    const struct jze_device_ops *ops = engine->ops;
    int count = 0;
    bool cm = (engine->setup_modes & JAZZ_SETUP_CM) != 0;
    errno = 0;
    if (cm) engine->cm_contexts = ops->cm_list(&count);
    else engine->verbs_devices = ops->verbs_list(&count);
    if ((cm && !engine->cm_contexts) || (!cm && !engine->verbs_devices))
        return jze_fail(error, errno ? errno : ENODEV, "enumerate-devices");
    if (count <= 0) return jze_fail(error, ENODEV, "no-devices");
    for (size_t i = 0; i < engine->device_count; ++i) {
        struct jze_device *device = &engine->devices[i];
        for (int j = 0; j < count; ++j) {
            struct ibv_device *candidate = cm ? engine->cm_contexts[j]->device :
                engine->verbs_devices[j];
            if (strcmp(device->name, ops->name(candidate))) continue;
            if (cm) device->verbs = engine->cm_contexts[j];
            else {
                errno = 0;
                device->verbs = ops->open(candidate);
                if (!device->verbs)
                    return jze_fail(error, errno ? errno : EIO, "open-device");
                device->owned_context = true;
            }
            break;
        }
        if (!device->verbs) return jze_fail(error, ENODEV, "device-not-found");
        int rc = inspect_device(engine, device, error);
        if (rc) return rc;
        errno = 0;
        device->pd = ops->alloc_pd(device->verbs);
        if (!device->pd) return jze_fail(error, errno ? errno : ENOMEM, "allocate-pd");
    }
    return 0;
}

int jze_devices_release(jazz_engine *engine, struct jazz_error *error)
{
    const struct jze_device_ops *ops = engine->ops;
    int first = 0;
    const char *stage = NULL;
    for (size_t i = engine->device_count; i > 0; --i) {
        struct jze_device *device = &engine->devices[i - 1];
        if (device->pd) {
            errno = 0;
            int rc = ops->dealloc_pd(device->pd);
            if (rc) {
                if (!first) { first = call_error(rc); stage = "deallocate-pd"; }
                continue; /* PD 尚在，context 及 provider 引用必须保留。 */
            }
            device->pd = NULL;
        }
        if (device->verbs && device->owned_context) {
            errno = 0;
            int rc = ops->close(device->verbs);
            if (rc) {
                if (!first) { first = call_error(rc); stage = "close-device"; }
                continue;
            }
        }
        device->verbs = NULL;
        device->owned_context = false;
        free(device->ports);
        device->ports = NULL;
        device->info = (struct jazz_device_info){0};
    }
    if (first) return jze_fail(error, first, stage);
    if (engine->cm_contexts) ops->cm_free(engine->cm_contexts);
    if (engine->verbs_devices) ops->verbs_free(engine->verbs_devices);
    engine->cm_contexts = NULL;
    engine->verbs_devices = NULL;
    return 0;
}
