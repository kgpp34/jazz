#include "rdma/internal.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static int select_device(jazz_connection *c, struct jazz_error *error)
{
    int count = 0;
    struct ibv_device **devices = ibv_get_device_list(&count);
    if (!devices) return jz_fail(error, -errno, "oob-enumerate-devices", errno);
    int rc = -ENODEV;
    for (int i = 0; i < count; ++i) {
        if (c->options.device[0] && strcmp(c->options.device, ibv_get_device_name(devices[i]))) continue;
        struct ibv_context *context = ibv_open_device(devices[i]);
        if (!context) continue;
        struct ibv_device_attr device;
        if (ibv_query_device(context, &device)) { (void)ibv_close_device(context); continue; }
        c->context = context;
        unsigned first = c->options.ib_port ? c->options.ib_port : 1;
        unsigned last = c->options.ib_port ? first : device.phys_port_cnt;
        for (unsigned port = first; port <= last; ++port) {
            c->ib_port = (uint8_t)port;
            rc = jz_validate_port(c, NULL);
            if (!rc) { c->own_context = true; break; }
        }
        if (!rc) break;
        (void)ibv_close_device(context);
        c->context = NULL;
    }
    ibv_free_device_list(devices);
    if (rc) return jz_fail(error, rc, "oob-select-rocev2-device", 0);
    return jz_resources(c, false, error);
}

static int exchange(int fd, enum jz_kind kind, void *outgoing, size_t out_length,
                     uint8_t incoming[JZ_FRAME_SIZE], const uint8_t **payload,
                     size_t *length, int64_t deadline)
{
    uint8_t frame[JZ_FRAME_SIZE];
    int rc = jz_frame_encode(frame, kind, outgoing, out_length);
    if (!rc) rc = jz_socket_io(fd, frame, sizeof(frame), 1, deadline);
    if (!rc) rc = jz_socket_io(fd, incoming, JZ_FRAME_SIZE, 0, deadline);
    if (!rc) rc = jz_frame_decode(incoming, JZ_FRAME_SIZE, kind, payload, length);
    return rc;
}

static int handshake(jazz_connection *c, int fd, int64_t deadline, struct jazz_error *error)
{
    struct jz_hello local = { .qpn = c->qp->qp_num,
        .psn = (uint32_t)c->session & UINT32_C(0xffffff),
        .mtu = (uint32_t)c->port_attr.active_mtu, .read_depth = 1, .session = c->session };
    memcpy(local.gid, c->gid_entry.gid.raw, sizeof(local.gid));
    uint8_t hello[JZ_HELLO_SIZE], incoming[JZ_FRAME_SIZE];
    int rc = jz_hello_encode(&local, hello);
    const uint8_t *payload = NULL;
    size_t length = 0;
    if (!rc) rc = exchange(fd, JZ_HELLO, hello, sizeof(hello), incoming, &payload, &length, deadline);
    struct jz_hello peer;
    if (!rc) rc = jz_hello_decode(payload, length, &peer);
    if (rc) return jz_fail(error, rc, "oob-hello", rc == -EPROTO ? 0 : -rc);
    struct ibv_qp_attr attr = { .qp_state = IBV_QPS_RTR,
        .path_mtu = (enum ibv_mtu)(local.mtu < peer.mtu ? local.mtu : peer.mtu),
        .dest_qp_num = peer.qpn, .rq_psn = peer.psn, .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12, .ah_attr = { .is_global = 1, .port_num = c->ib_port } };
    memcpy(attr.ah_attr.grh.dgid.raw, peer.gid, 16);
    attr.ah_attr.grh.sgid_index = (uint8_t)c->gid_index;
    attr.ah_attr.grh.hop_limit = 64;
    rc = ibv_modify_qp(c->qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
    if (rc) return jz_fail(error, -rc, "oob-qp-rtr", rc);
    attr = (struct ibv_qp_attr){ .qp_state = IBV_QPS_RTS, .sq_psn = local.psn,
        .timeout = 14, .retry_cnt = 3, .rnr_retry = 7, .max_rd_atomic = 1 };
    rc = ibv_modify_qp(c->qp, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
    if (rc) return jz_fail(error, -rc, "oob-qp-rts", rc);
    uint8_t ready[16];
    jz_put64(ready, local.session);
    jz_put64(ready + 8, peer.session);
    rc = exchange(fd, JZ_READY, ready, sizeof(ready), incoming, &payload, &length, deadline);
    if (!rc && (length != sizeof(ready) || jz_get64(payload) != peer.session ||
                jz_get64(payload + 8) != local.session)) rc = -EPROTO;
    if (rc) return jz_fail(error, rc, "oob-ready-confirmation", rc == -EPROTO ? 0 : -rc);
    c->peer_session = peer.session;
    return 0;
}

int jz_oob_connect(jazz_connection *c, int64_t deadline, struct jazz_error *error)
{
    int rc = select_device(c, error);
    if (rc) return rc;
    int fd = jz_socket_connect(&c->options.local, &c->options.peer, deadline);
    if (fd < 0) return jz_fail(error, fd, "oob-tcp-connect", -fd);
    rc = handshake(c, fd, deadline, error);
    close(fd);
    return rc;
}

int jz_oob_accept(jazz_listener *l, jazz_connection *c, int64_t deadline,
                  struct jazz_error *error)
{
    int fd = jz_socket_accept(l->fd, deadline);
    if (fd < 0) return jz_fail(error, fd, "oob-tcp-accept", -fd);
    int rc = select_device(c, error);
    if (!rc) rc = handshake(c, fd, deadline, error);
    close(fd);
    return rc;
}
