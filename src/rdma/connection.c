#include "rdma/internal.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

void jz_clear_error(struct jazz_error *error)
{
    if (error) *error = (struct jazz_error){0};
}

int jz_fail(struct jazz_error *error, int code, const char *stage, int sys)
{
    if (error) *error = (struct jazz_error){ .code = code, .stage = stage,
                                           .system_errno = sys };
    return code;
}

void jazz_config_init(struct jazz_config *config)
{
    if (config) *config = (struct jazz_config){ .mode = JAZZ_CM,
                                               .gid_index = -1, .timeout_ms = 5000 };
}

int jz_options(const struct jazz_config *config, bool client,
               struct jz_options *out, struct jazz_error *error)
{
    if (!config || !out || (config->mode != JAZZ_CM && config->mode != JAZZ_OOB) ||
        !config->timeout_ms || config->timeout_ms > INT_MAX || config->gid_index < -1 ||
        config->gid_index > 255 || (client && !config->port) ||
        (config->mode == JAZZ_CM && (config->gid_index != -1 || config->ib_port)))
        return jz_fail(error, -EINVAL, "config", 0);
    *out = (struct jz_options){ .mode = config->mode, .ib_port = config->ib_port,
                               .gid_index = config->gid_index,
                               .timeout_ms = config->timeout_ms };
    if (jz_address(config->local_address, client ? 0 : config->port, &out->local) ||
        (client && jz_address(config->peer_address, config->port, &out->peer)))
        return jz_fail(error, -EINVAL, "numeric-ipv4-address", 0);
    if (config->device) {
        size_t n = strlen(config->device);
        if (!n || n >= sizeof(out->device)) return jz_fail(error, -EINVAL, "device-name", 0);
        memcpy(out->device, config->device, n + 1);
    }
    return 0;
}

int jz_new_connection(const struct jz_options *options, jazz_connection **out,
                      struct jazz_error *error)
{
    jazz_connection *c = calloc(1, sizeof(*c));
    if (!c) return jz_fail(error, -ENOMEM, "connection-allocate", errno);
    c->options = *options;
    ssize_t n;
    do { n = getrandom(&c->session, sizeof(c->session), 0); } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(c->session)) {
        free(c);
        return jz_fail(error, -EIO, "session-random", n < 0 ? errno : 0);
    }
    if (!c->session) c->session = 1;
    *out = c;
    return 0;
}

int jz_validate_port(jazz_connection *c, struct jazz_error *error)
{
    if (ibv_query_port(c->context, c->ib_port, &c->port_attr))
        return jz_fail(error, -EIO, "query-port", errno);
    if (c->port_attr.state != IBV_PORT_ACTIVE ||
        c->port_attr.link_layer != IBV_LINK_LAYER_ETHERNET)
        return jz_fail(error, -ENETDOWN, "active-ethernet-port", 0);
    if (c->options.device[0] &&
        strcmp(c->options.device, ibv_get_device_name(c->context->device)))
        return jz_fail(error, -ENODEV, "selected-device-mismatch", 0);
    uint8_t address[16] = {0};
    address[10] = address[11] = 0xff;
    memcpy(address + 12, &c->options.local.sin_addr, 4);
    int start = c->options.gid_index >= 0 ? c->options.gid_index : 0;
    int end = c->options.gid_index >= 0 ? start + 1 : c->port_attr.gid_tbl_len;
    for (int i = start; i < end; ++i) {
        struct ibv_gid_entry entry;
        if (ibv_query_gid_ex(c->context, c->ib_port, (uint32_t)i, &entry, 0)) continue;
        if (entry.gid_type != IBV_GID_TYPE_ROCE_V2 || !entry.ndev_ifindex ||
            memcmp(entry.gid.raw, address, 16)) continue;
        c->gid_entry = entry;
        c->gid_index = i;
        return 0;
    }
    return jz_fail(error, -EADDRNOTAVAIL, "matching-rocev2-gid", 0);
}

int jz_post_receive(jazz_connection *c, struct jazz_error *error)
{
    struct ibv_sge sge = { .addr = (uint64_t)(uintptr_t)c->recv_buffer,
                           .length = JZ_FRAME_SIZE, .lkey = c->recv_mr->lkey };
    struct ibv_recv_wr wr = { .wr_id = JZ_WR_RECV, .sg_list = &sge, .num_sge = 1 };
    struct ibv_recv_wr *bad = NULL;
    int rc = ibv_post_recv(c->qp, &wr, &bad);
    return rc ? jz_fail(error, -rc, "post-control-receive", rc) : 0;
}

int jz_resources(jazz_connection *c, bool cm, struct jazz_error *error)
{
    struct ibv_device_attr device;
    if (ibv_query_device(c->context, &device)) return jz_fail(error, -EIO, "query-device", errno);
    if (device.max_qp_rd_atom < 1 || device.max_qp_init_rd_atom < 1)
        return jz_fail(error, -EOPNOTSUPP, "rdma-read-capability", 0);
    c->max_message = c->port_attr.max_msg_sz;
    c->pd = ibv_alloc_pd(c->context);
    if (!c->pd) return jz_fail(error, -ENOMEM, "allocate-pd", errno);
    c->send_cq = ibv_create_cq(c->context, 8, NULL, NULL, 0);
    c->recv_cq = ibv_create_cq(c->context, 8, NULL, NULL, 0);
    if (!c->send_cq || !c->recv_cq) return jz_fail(error, -ENOMEM, "create-cq", errno);
    struct ibv_qp_init_attr attr = {
        .send_cq = c->send_cq, .recv_cq = c->recv_cq, .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 2, .max_recv_wr = 1,
                 .max_send_sge = 1, .max_recv_sge = 1 }
    };
    if (cm) {
        if (rdma_create_qp(c->id, c->pd, &attr)) return jz_fail(error, -EIO, "cm-create-qp", errno);
        c->qp = c->id->qp;
    } else {
        c->qp = ibv_create_qp(c->pd, &attr);
        if (!c->qp) return jz_fail(error, -EIO, "oob-create-qp", errno);
        struct ibv_qp_attr init = { .qp_state = IBV_QPS_INIT, .port_num = c->ib_port,
            .pkey_index = 0, .qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
            IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE };
        int rc = ibv_modify_qp(c->qp, &init,
            IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
        if (rc) return jz_fail(error, -rc, "oob-qp-init", rc);
    }
    c->send_mr = ibv_reg_mr(c->pd, c->send_buffer, sizeof(c->send_buffer), 0);
    c->recv_mr = ibv_reg_mr(c->pd, c->recv_buffer, sizeof(c->recv_buffer), IBV_ACCESS_LOCAL_WRITE);
    if (!c->send_mr || !c->recv_mr) return jz_fail(error, -ENOMEM, "register-control-memory", errno);
    return jz_post_receive(c, error);
}

static int validate_connected_qp(jazz_connection *c, struct jazz_error *error)
{
    struct ibv_qp_attr attr = {0};
    struct ibv_qp_init_attr init = {0};
    int rc = ibv_query_qp(c->qp, &attr, IBV_QP_STATE | IBV_QP_AV, &init);
    if (rc) return jz_fail(error, -rc, "query-connected-qp", rc);
    if (attr.qp_state != IBV_QPS_RTS || !attr.ah_attr.is_global)
        return jz_fail(error, -EPROTO, "connected-qp-state-or-path", 0);
    struct ibv_gid_entry selected;
    rc = ibv_query_gid_ex(c->context, c->ib_port, attr.ah_attr.grh.sgid_index, &selected, 0);
    if (rc) return jz_fail(error, -rc, "query-selected-gid", rc);
    if (selected.gid_type != IBV_GID_TYPE_ROCE_V2 ||
        selected.ndev_ifindex != c->gid_entry.ndev_ifindex ||
        memcmp(selected.gid.raw, c->gid_entry.gid.raw, 16))
        return jz_fail(error, -EOPNOTSUPP, "selected-path-must-be-rocev2", 0);
    return 0;
}

int jazz_listen(const struct jazz_config *config, jazz_listener **out,
                struct jazz_error *error)
{
    jz_clear_error(error);
    if (!out || *out) return jz_fail(error, -EINVAL, "listener-output", 0);
    struct jz_options options;
    int rc = jz_options(config, false, &options, error);
    if (rc) return rc;
    jazz_listener *l = calloc(1, sizeof(*l));
    if (!l) return jz_fail(error, -ENOMEM, "listener-allocate", errno);
    l->fd = -1;
    l->options = options;
    if (options.mode == JAZZ_CM) rc = jz_cm_listen(l, error);
    else {
        l->fd = jz_socket_listen(&options.local, &l->port);
        if (l->fd < 0) rc = jz_fail(error, l->fd, "oob-listen", -l->fd);
    }
    if (rc) {
        if (jazz_listener_close(&l, NULL)) *out = l;
        return rc;
    }
    *out = l;
    return 0;
}

uint16_t jazz_listener_port(const jazz_listener *listener)
{
    return listener ? listener->port : 0;
}

int jazz_accept(jazz_listener *l, jazz_connection **out, struct jazz_error *error)
{
    jz_clear_error(error);
    if (!l || !out || *out) return jz_fail(error, -EINVAL, "accept-arguments", 0);
    jazz_connection *c = NULL;
    int rc = jz_new_connection(&l->options, &c, error);
    if (rc) return rc;
    int64_t deadline = jz_now_ms() + l->options.timeout_ms;
    rc = l->options.mode == JAZZ_CM ? jz_cm_accept(l, c, deadline, error)
                                   : jz_oob_accept(l, c, deadline, error);
    if (!rc) rc = validate_connected_qp(c, error);
    if (rc) {
        /* Return the handle if provider cleanup fails: ownership is never lost. */
        if (jazz_connection_close(&c, NULL)) *out = c;
        return rc;
    }
    c->ready = true;
    *out = c;
    return 0;
}

int jazz_connect(const struct jazz_config *config, jazz_connection **out,
                 struct jazz_error *error)
{
    jz_clear_error(error);
    if (!out || *out) return jz_fail(error, -EINVAL, "connection-output", 0);
    struct jz_options options;
    int rc = jz_options(config, true, &options, error);
    if (rc) return rc;
    jazz_connection *c = NULL;
    rc = jz_new_connection(&options, &c, error);
    if (rc) return rc;
    int64_t deadline = jz_now_ms() + options.timeout_ms;
    rc = options.mode == JAZZ_CM ? jz_cm_connect(c, deadline, error)
                                : jz_oob_connect(c, deadline, error);
    if (!rc) rc = validate_connected_qp(c, error);
    if (rc) {
        if (jazz_connection_close(&c, NULL)) *out = c;
        return rc;
    }
    c->ready = true;
    *out = c;
    return 0;
}

int jazz_connection_quiesce(jazz_connection *c, struct jazz_error *error)
{
    jz_clear_error(error);
    if (!c) return jz_fail(error, -EINVAL, "quiesce-arguments", 0);
    c->ready = false;
    if (c->established && c->id) {
        (void)rdma_disconnect(c->id);
        c->established = false;
    }
    if (c->qp) {
        int rc = ibv_destroy_qp(c->qp);
        if (rc) return jz_fail(error, -rc, "destroy-qp-retain-memory", rc);
        c->qp = NULL;
        if (c->id) c->id->qp = NULL;
    }
    for (jazz_region *r = c->regions; r; r = r->next) r->busy = false;
    return 0;
}

int jazz_connection_close(jazz_connection **connection, struct jazz_error *error)
{
    jz_clear_error(error);
    if (!connection) return jz_fail(error, -EINVAL, "close-arguments", 0);
    jazz_connection *c = *connection;
    if (!c) return 0;
    int rc = jazz_connection_quiesce(c, error);
    if (rc) return rc;
    if (c->regions) return jz_fail(error, -EBUSY, "registered-regions-remain", 0);
#define JZ_RELEASE(field, call, stage_name) do { \
    if (c->field) { int result = (call); \
        if (result) return jz_fail(error, result > 0 ? -result : -errno, stage_name, \
                                   result > 0 ? result : errno); \
        c->field = NULL; } \
} while (0)
    JZ_RELEASE(send_mr, ibv_dereg_mr(c->send_mr), "deregister-control-send");
    JZ_RELEASE(recv_mr, ibv_dereg_mr(c->recv_mr), "deregister-control-receive");
    JZ_RELEASE(send_cq, ibv_destroy_cq(c->send_cq), "destroy-send-cq");
    JZ_RELEASE(recv_cq, ibv_destroy_cq(c->recv_cq), "destroy-receive-cq");
    JZ_RELEASE(pd, ibv_dealloc_pd(c->pd), "deallocate-pd");
    /* Every CM event read by the library has already been acknowledged. */
    JZ_RELEASE(id, rdma_destroy_id(c->id), "destroy-cm-id");
    if (c->channel) { rdma_destroy_event_channel(c->channel); c->channel = NULL; }
    if (c->own_context) {
        JZ_RELEASE(context, ibv_close_device(c->context), "close-device");
    }
#undef JZ_RELEASE
    free(c);
    *connection = NULL;
    return 0;
}

int jazz_listener_close(jazz_listener **listener, struct jazz_error *error)
{
    jz_clear_error(error);
    if (!listener) return jz_fail(error, -EINVAL, "listener-close-arguments", 0);
    jazz_listener *l = *listener;
    if (!l) return 0;
    if (l->id && rdma_destroy_id(l->id)) return jz_fail(error, -errno, "destroy-listener-id", errno);
    if (l->channel) rdma_destroy_event_channel(l->channel);
    if (l->fd >= 0) close(l->fd);
    free(l);
    *listener = NULL;
    return 0;
}

int jz_abort(jazz_connection *c, int result)
{
    (void)jazz_connection_quiesce(c, NULL);
    return result;
}
