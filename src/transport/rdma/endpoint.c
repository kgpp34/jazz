#include "transport/rdma/control.h"
#include "common/error.h"
#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <string.h>
#include <sys/random.h>

socklen_t jz_control_address_size(const struct sockaddr_storage *address)
{
    return address->ss_family == AF_INET ? (socklen_t)sizeof(struct sockaddr_in)
                                         : (socklen_t)sizeof(struct sockaddr_in6);
}
void jz_control_set_port(struct sockaddr_storage *address, uint16_t port)
{
    if (address->ss_family == AF_INET)
        ((struct sockaddr_in *)address)->sin_port = htons(port);
    else
        ((struct sockaddr_in6 *)address)->sin6_port = htons(port);
}
uint16_t jz_control_get_port(const struct sockaddr_storage *address)
{
    return address->ss_family == AF_INET ? ntohs(((const struct sockaddr_in *)address)->sin_port)
                                         : ntohs(((const struct sockaddr_in6 *)address)->sin6_port);
}
int jz_control_parse_address(const char *ip, uint16_t port, struct sockaddr_storage *out,
                             struct jz_error *error)
{
    if (!ip || !out)
        return jz_fail(error, EINVAL, "numeric-ip");
    struct sockaddr_storage value = {0};
    struct sockaddr_in *v4 = (struct sockaddr_in *)&value;
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&value;
    if (inet_pton(AF_INET, ip, &v4->sin_addr) == 1) {
        value.ss_family = AF_INET;
        uint32_t host = ntohl(v4->sin_addr.s_addr);
        if (!host || host == INADDR_BROADCAST || IN_MULTICAST(host))
            return jz_fail(error, EADDRNOTAVAIL, "concrete-ip");
    } else if (inet_pton(AF_INET6, ip, &v6->sin6_addr) == 1) {
        value.ss_family = AF_INET6;
        /* Scoped link-local addresses need an explicit scope API; never guess. */
        if (IN6_IS_ADDR_UNSPECIFIED(&v6->sin6_addr) || IN6_IS_ADDR_MULTICAST(&v6->sin6_addr) ||
            IN6_IS_ADDR_LINKLOCAL(&v6->sin6_addr))
            return jz_fail(error, EADDRNOTAVAIL, "concrete-ipv6");
    } else
        return jz_fail(error, EINVAL, "numeric-ip");
    jz_control_set_port(&value, port);
    *out = value;
    return 0;
}

static void address_gid(const struct sockaddr_storage *address, union ibv_gid *gid)
{
    memset(gid, 0, sizeof(*gid));
    if (address->ss_family == AF_INET) {
        gid->raw[10] = 0xff;
        gid->raw[11] = 0xff;
        memcpy(gid->raw + 12, &((const struct sockaddr_in *)address)->sin_addr, 4);
    } else
        memcpy(gid->raw, &((const struct sockaddr_in6 *)address)->sin6_addr, 16);
}
static bool interface_matches(const struct ifaddrs *interfaces,
                              const struct sockaddr_storage *address, uint32_t index)
{
    for (const struct ifaddrs *it = interfaces; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != address->ss_family ||
            if_nametoindex(it->ifa_name) != index)
            continue;
        if (address->ss_family == AF_INET) {
            if (!memcmp(&((const struct sockaddr_in *)it->ifa_addr)->sin_addr,
                        &((const struct sockaddr_in *)address)->sin_addr, 4))
                return true;
        } else if (!memcmp(&((const struct sockaddr_in6 *)it->ifa_addr)->sin6_addr,
                           &((const struct sockaddr_in6 *)address)->sin6_addr, 16))
            return true;
    }
    return false;
}
int jz_control_validate_bind(struct jz_rdma_backend *backend, const char *ip,
                             enum jz_rdma_setup setup, size_t device_index, uint8_t port,
                             int gid_index, uint32_t timeout_ms, struct jz_bind_config *out,
                             struct jz_error *error)
{
    if ((setup != JZ_RDMA_SETUP_CM && setup != JZ_RDMA_SETUP_OOB) ||
        !((unsigned)setup & backend->setup_modes) || device_index >= backend->device_count ||
        !port || !timeout_ms || timeout_ms > INT32_MAX || gid_index < -1 || gid_index > 255)
        return jz_fail(error, EINVAL, "connection-config");
    struct jz_rdma_device *device = &backend->devices[device_index];
    if (port > device->info.port_count)
        return jz_fail(error, ERANGE, "connection-port");
    struct jz_bind_config config = {.setup = setup,
                                    .device = device_index,
                                    .port = port,
                                    .timeout_ms = timeout_ms,
                                    .gid_explicit = gid_index >= 0};
    int rc = jz_control_parse_address(ip, 0, &config.local, error);
    if (rc)
        return rc;
    struct ibv_port_attr attr = {0};
    rc = jz_rdma_driver_query_port(&backend->driver, device->lease.verbs, port, &attr, error);
    if (rc)
        return rc;
    if (attr.state != IBV_PORT_ACTIVE || attr.link_layer != IBV_LINK_LAYER_ETHERNET ||
        attr.gid_tbl_len <= 0 || attr.active_mtu < IBV_MTU_256 || attr.active_mtu > IBV_MTU_4096)
        return jz_fail(error, ENETDOWN, "rocev2-port");
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces))
        return jz_fail(error, errno, "local-interfaces");
    union ibv_gid expected;
    address_gid(&config.local, &expected);
    rc = -EADDRNOTAVAIL;
    for (uint32_t i = 0; i < (uint32_t)attr.gid_tbl_len && i <= 255; ++i) {
        if (gid_index >= 0 && i != (uint32_t)gid_index)
            continue;
        struct ibv_gid_entry entry = {0};
        if (jz_rdma_driver_query_gid(&backend->driver, device->lease.verbs, port, i, &entry, NULL))
            continue;
        if (entry.gid_type != IBV_GID_TYPE_ROCE_V2 || memcmp(entry.gid.raw, expected.raw, 16) ||
            !interface_matches(interfaces, &config.local, entry.ndev_ifindex))
            continue;
        config.gid = entry.gid;
        config.gid_index = i;
        config.mtu = attr.active_mtu;
        rc = 0;
        break;
    }
    freeifaddrs(interfaces);
    if (rc)
        return jz_fail(error, EADDRNOTAVAIL, "rocev2-gid-ip-device");
    *out = config;
    return 0;
}

int jz_control_prepare_qp(struct jz_rdma_control *control, struct jz_rdma_endpoint *ep, bool cm,
                          struct jz_error *error)
{
    struct jz_rdma_device *device = &control->backend->devices[ep->config.device];
    if (cm && (ep->job.id->verbs != device->lease.verbs || ep->job.id->port_num != ep->config.port))
        return jz_fail(error, EXDEV, "cm-selected-device");
    if (!control->cqs[ep->config.device]) {
        size_t entries = control->backend->max_connections * 128;
        if (entries > device->info.max_cqe)
            entries = device->info.max_cqe;
        if (!entries || entries > INT32_MAX)
            return jz_fail(error, ENOSPC, "connection-cq-budget");
        control->cqs[ep->config.device] =
            ibv_create_cq(device->lease.verbs, (int)entries, NULL, NULL, 0);
        if (!control->cqs[ep->config.device])
            return jz_fail(error, errno, "connection-create-cq");
    }
    uint32_t depth = device->info.max_qp_wr < 64 ? device->info.max_qp_wr : 64;
    struct ibv_qp_init_attr attr = {
        .send_cq = control->cqs[ep->config.device],
        .recv_cq = control->cqs[ep->config.device],
        .qp_type = IBV_QPT_RC,
        .cap = {.max_send_wr = depth, .max_recv_wr = depth, .max_send_sge = 1, .max_recv_sge = 1}};
    if (cm) {
        if (rdma_create_qp(ep->job.id, device->pd, &attr))
            return jz_fail(error, errno, "cm-create-qp");
        ep->qp = ep->job.id->qp;
    } else {
        ep->qp = ibv_create_qp(device->pd, &attr);
        if (!ep->qp)
            return jz_fail(error, errno, "oob-create-qp");
        struct ibv_qp_attr init = {.qp_state = IBV_QPS_INIT,
                                   .port_num = ep->config.port,
                                   .qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                                                      IBV_ACCESS_REMOTE_READ |
                                                      IBV_ACCESS_REMOTE_WRITE};
        int rc = ibv_modify_qp(
            ep->qp, &init, IBV_QP_STATE | IBV_QP_PORT | IBV_QP_PKEY_INDEX | IBV_QP_ACCESS_FLAGS);
        if (rc)
            return jz_fail(error, rc, "oob-qp-init");
    }
    uint32_t psn;
    if (getrandom(&psn, sizeof(psn), GRND_NONBLOCK) != (ssize_t)sizeof(psn))
        return jz_fail(error, errno ? errno : EIO, "qp-psn");
    ep->local.qpn = ep->qp->qp_num;
    ep->local.psn = psn & 0xffffff;
    ep->local.gid = ep->config.gid;
    ep->local.mtu = ep->config.mtu;
    ep->local.reads = 1;
    return 0;
}
int jz_control_oob_connect_qp(struct jz_rdma_endpoint *ep, struct jz_error *error)
{
    enum ibv_mtu mtu = ep->local.mtu < ep->remote_msg.mtu ? ep->local.mtu : ep->remote_msg.mtu;
    struct ibv_qp_attr rtr = {.qp_state = IBV_QPS_RTR,
                              .path_mtu = mtu,
                              .dest_qp_num = ep->remote_msg.qpn,
                              .rq_psn = ep->remote_msg.psn,
                              .max_dest_rd_atomic = 1,
                              .min_rnr_timer = 12,
                              .ah_attr = {.is_global = 1,
                                          .port_num = ep->config.port,
                                          .grh = {.dgid = ep->remote_msg.gid,
                                                  .sgid_index = (uint8_t)ep->config.gid_index,
                                                  .hop_limit = 64}}};
    int rc = ibv_modify_qp(ep->qp, &rtr,
                           IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                               IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    if (rc)
        return jz_fail(error, rc, "oob-qp-rtr");
    struct ibv_qp_attr rts = {.qp_state = IBV_QPS_RTS,
                              .sq_psn = ep->local.psn,
                              .timeout = 14,
                              .retry_cnt = 7,
                              .rnr_retry = 7,
                              .max_rd_atomic = 1};
    rc = ibv_modify_qp(ep->qp, &rts,
                       IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                           IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
    if (rc)
        return jz_fail(error, rc, "oob-qp-rts");
    ep->local.mtu = mtu;
    return 0;
}
int jz_control_cm_check(struct jz_rdma_endpoint *ep, struct jz_error *error)
{
    struct ibv_qp_attr attr;
    struct ibv_qp_init_attr init;
    int rc = ibv_query_qp(ep->qp, &attr,
                          IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN, &init);
    if (rc)
        return jz_fail(error, rc, "cm-query-qp");
    struct ibv_gid_entry gid;
    rc = ibv_query_gid_ex(ep->qp->context, ep->config.port, attr.ah_attr.grh.sgid_index, &gid, 0);
    if (rc || gid.gid_type != IBV_GID_TYPE_ROCE_V2 || memcmp(gid.gid.raw, ep->config.gid.raw, 16) ||
        attr.qp_state != IBV_QPS_RTS)
        return jz_fail(error, rc ? rc : EPROTONOSUPPORT, "cm-rocev2-path");
    if (ep->config.gid_explicit && attr.ah_attr.grh.sgid_index != ep->config.gid_index)
        return jz_fail(error, EADDRNOTAVAIL, "cm-explicit-gid");
    /* Explicitly apply the engine policy after librdmacm's transitions. */
    struct ibv_qp_attr access = {.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
                                                    IBV_ACCESS_REMOTE_READ |
                                                    IBV_ACCESS_REMOTE_WRITE};
    rc = ibv_modify_qp(ep->qp, &access, IBV_QP_ACCESS_FLAGS);
    if (rc)
        return jz_fail(error, rc, "cm-remote-access");
    ep->remote_msg.qpn = attr.dest_qp_num;
    ep->local.mtu = attr.path_mtu;
    return 0;
}
