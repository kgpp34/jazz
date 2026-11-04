#include "transport/rdma/internal.h"
#include "common/error.h"
#include <errno.h>
#include <endian.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static int inspect_device(struct jz_rdma_device *device, const struct jz_rdma_driver *driver,
                           struct jz_error *error)
{
    struct ibv_device_attr attr = {0};
    int rc = jz_rdma_driver_query_device(driver, device->lease.verbs, &attr, error);
    if (rc) return rc;
    if (!attr.phys_port_cnt || attr.max_qp_wr <= 0 || attr.max_cqe <= 0 ||
        attr.max_sge <= 0 || attr.max_qp_rd_atom <= 0 || attr.max_qp_init_rd_atom <= 0)
        return jz_fail(error, EOPNOTSUPP, "device-capabilities");
    device->info = (struct jz_rdma_device_info){
        .guid = be64toh(attr.node_guid), .max_mr_size = attr.max_mr_size,
        .max_qp_wr = (uint32_t)attr.max_qp_wr, .max_cqe = (uint32_t)attr.max_cqe,
        .max_sge = (uint32_t)attr.max_sge,
        .max_read_outstanding = (uint32_t)attr.max_qp_init_rd_atom,
        .max_read_responder = (uint32_t)attr.max_qp_rd_atom,
        .port_count = attr.phys_port_cnt
    };
    memcpy(device->info.name, device->name, strlen(device->name) + 1);
    device->ports = calloc(attr.phys_port_cnt, sizeof(*device->ports));
    if (!device->ports) return jz_fail(error, ENOMEM, "port-table-allocate");
    bool usable = false;
    for (unsigned port = 1; port <= attr.phys_port_cnt; ++port) {
        struct ibv_port_attr pa = {0};
        rc = jz_rdma_driver_query_port(driver, device->lease.verbs, (uint8_t)port, &pa, error);
        if (rc) return rc;
        struct jz_rdma_port_info *info = &device->ports[port - 1u];
        *info = (struct jz_rdma_port_info){ .port = (uint8_t)port,
            .active = pa.state == IBV_PORT_ACTIVE,
            .ethernet = pa.link_layer == IBV_LINK_LAYER_ETHERNET,
            .max_message_size = pa.max_msg_sz };
        if (pa.active_mtu >= IBV_MTU_256 && pa.active_mtu <= IBV_MTU_4096)
            info->active_mtu = 128u << (unsigned)pa.active_mtu;
        if (!info->active || !info->ethernet) continue;
        if (pa.gid_tbl_len < 0) return jz_fail(error, EIO, "gid-table-length");
        for (int gid = 0; gid < pa.gid_tbl_len; ++gid) {
            struct ibv_gid_entry entry = {0};
            rc = jz_rdma_driver_query_gid(driver, device->lease.verbs, port, (uint32_t)gid, &entry, NULL);
            if (rc) {
                int reason = -rc;
                if (reason == ENODATA || reason == ENOENT) continue;
                return jz_fail(error, reason, "query-gid-type");
            }
            if (entry.gid_type == IBV_GID_TYPE_ROCE_V2 &&
                (entry.gid.global.subnet_prefix || entry.gid.global.interface_id))
                ++info->roce_v2_gid_count;
        }
        usable |= info->roce_v2_gid_count != 0;
    }
    return usable ? 0 : jz_fail(error, EOPNOTSUPP, "no-active-rocev2-port");
}


int jz_rdma_device_prepare(struct jz_rdma_device *device,
                            struct jz_rdma_context_source *source, struct jz_error *error)
{
    int rc = source->ops->acquire(source, device->name, &device->lease, error);
    if (rc) return rc;
    rc = inspect_device(device, source->driver, error);
    if (rc) return rc;
    return jz_rdma_driver_alloc_pd(source->driver, device->lease.verbs, &device->pd, error);
}
int jz_rdma_device_release(struct jz_rdma_device *device,
                            struct jz_rdma_context_source *source, struct jz_error *error)
{
    if (device->pd) {
        int rc = jz_rdma_driver_dealloc_pd(source->driver, device->pd, error);
        if (rc) return rc; /* PD still depends on the live context lease. */
        device->pd = NULL;
    }
    if (device->lease.verbs) {
        int rc = source->ops->return_lease(source, &device->lease, error);
        if (rc) return rc;
    }
    free(device->ports);
    device->ports = NULL;
    device->info = (struct jz_rdma_device_info){0};
    return 0;
}
