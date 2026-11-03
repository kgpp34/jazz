#include "rdma/internal.h"

#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

int jz_wait_send(jz_connection *c, uint64_t id, enum ibv_wc_opcode opcode,
                 int64_t deadline, struct jz_error *error)
{
    for (;;) {
        struct ibv_wc wc;
        int n = ibv_poll_cq(c->send_cq, 1, &wc);
        if (n < 0) return jz_fail(error, -EIO, "poll-send-cq", 0);
        if (n) {
            /* Only status, wr_id, qp_num and vendor_err are valid on error. */
            if (wc.status != IBV_WC_SUCCESS) {
                int rc = jz_fail(error, -EIO, "send-work-completion", 0);
                if (error) error->wc_status = (uint32_t)wc.status;
                return rc;
            }
            if (wc.wr_id != id || wc.opcode != opcode || wc.qp_num != c->qp->qp_num)
                return jz_fail(error, -EPROTO, "unexpected-send-completion", 0);
            return 0;
        }
        int remaining = jz_remaining(deadline);
        if (remaining <= 0) return jz_fail(error, remaining < 0 ? remaining : -ETIMEDOUT,
                                          "send-completion-deadline", 0);
        sched_yield();
    }
}

int jz_region_register(jz_connection *c, void *buffer, size_t length,
                         unsigned access, jz_region **out, struct jz_error *error)
{
    jz_clear_error(error);
    if (!c || !c->ready || !buffer || !length || !out || *out ||
        (uintptr_t)buffer > UINTPTR_MAX - length ||
        (access & ~(unsigned)(JZ_REMOTE_READ | JZ_REMOTE_WRITE)))
        return jz_fail(error, -EINVAL, "register-arguments", 0);
    if (c->region_count >= JZ_MAX_REGIONS) return jz_fail(error, -ENOSPC, "region-budget", 0);
    jz_region *r = calloc(1, sizeof(*r));
    if (!r) return jz_fail(error, -ENOMEM, "region-allocate", errno);
    unsigned flags = IBV_ACCESS_LOCAL_WRITE;
    if (access & JZ_REMOTE_READ) flags |= IBV_ACCESS_REMOTE_READ;
    if (access & JZ_REMOTE_WRITE) flags |= IBV_ACCESS_REMOTE_WRITE;
    r->mr = ibv_reg_mr(c->pd, buffer, length, flags);
    if (!r->mr) {
        int saved = errno;
        free(r);
        return jz_fail(error, -saved, "register-memory", saved);
    }
    r->connection = c;
    r->length = length;
    r->access = access;
    r->next = c->regions;
    c->regions = r;
    ++c->region_count;
    *out = r;
    return 0;
}

int jz_region_unregister(jz_region **region, struct jz_error *error)
{
    jz_clear_error(error);
    if (!region) return jz_fail(error, -EINVAL, "unregister-arguments", 0);
    jz_region *r = *region;
    if (!r) return 0;
    if (r->busy) return jz_fail(error, -EBUSY, "region-in-flight", 0);
    if (r->access && r->connection->qp)
        return jz_fail(error, -EBUSY, "quiesce-before-remote-region-release", 0);
    int rc = ibv_dereg_mr(r->mr);
    if (rc) return jz_fail(error, -rc, "deregister-memory", rc);
    jz_connection *c = r->connection;
    jz_region **entry = &c->regions;
    while (*entry != r) entry = &(*entry)->next;
    *entry = r->next;
    --c->region_count;
    free(r);
    *region = NULL;
    return 0;
}

int jz_region_export(const jz_region *r, struct jz_remote_region *out,
                       struct jz_error *error)
{
    jz_clear_error(error);
    if (!r || !out || !r->connection->ready) return jz_fail(error, -EINVAL, "export-arguments", 0);
    *out = (struct jz_remote_region){ .address = (uint64_t)(uintptr_t)r->mr->addr,
        .length = r->length, .rkey = r->mr->rkey, .access = r->access,
        .session = r->connection->session };
    return 0;
}

int jz_send_message(jz_connection *c, const void *data, size_t length,
                      struct jz_error *error)
{
    jz_clear_error(error);
    if (!c || !c->ready || (length && !data)) return jz_fail(error, -EINVAL, "send-message-arguments", 0);
    if (length > JZ_MESSAGE_MAX) return jz_fail(error, -EMSGSIZE, "control-message-limit", 0);
    (void)jz_frame_encode(c->send_buffer, JZ_MESSAGE, data, length);
    struct ibv_sge sge = { .addr = (uint64_t)(uintptr_t)c->send_buffer,
                           .length = JZ_FRAME_SIZE, .lkey = c->send_mr->lkey };
    struct ibv_send_wr wr = { .wr_id = JZ_WR_SEND, .sg_list = &sge, .num_sge = 1,
                              .opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED };
    struct ibv_send_wr *bad = NULL;
    int rc = ibv_post_send(c->qp, &wr, &bad);
    if (rc) return jz_abort(c, jz_fail(error, -rc, "post-control-send", rc));
    rc = jz_wait_send(c, JZ_WR_SEND, IBV_WC_SEND,
                      jz_now_ms() + c->options.timeout_ms, error);
    return rc ? jz_abort(c, rc) : 0;
}

int jz_receive_message(jz_connection *c, void *data, size_t capacity,
                         size_t *length, struct jz_error *error)
{
    jz_clear_error(error);
    if (!c || !c->ready || !data || !length || capacity < JZ_MESSAGE_MAX)
        return jz_fail(error, -EINVAL, "receive-message-arguments", 0);
    int64_t deadline = jz_now_ms() + c->options.timeout_ms;
    for (;;) {
        struct ibv_wc wc;
        int n = ibv_poll_cq(c->recv_cq, 1, &wc);
        if (n < 0) return jz_abort(c, jz_fail(error, -EIO, "poll-receive-cq", 0));
        if (n) {
            if (wc.status != IBV_WC_SUCCESS) {
                int rc = jz_fail(error, -EIO, "receive-work-completion", 0);
                if (error) error->wc_status = (uint32_t)wc.status;
                return jz_abort(c, rc);
            }
            if (wc.wr_id != JZ_WR_RECV || wc.opcode != IBV_WC_RECV ||
                wc.qp_num != c->qp->qp_num || wc.byte_len != JZ_FRAME_SIZE)
                return jz_abort(c, jz_fail(error, -EPROTO, "unexpected-receive-completion", 0));
            break;
        }
        int remaining = jz_remaining(deadline);
        if (remaining <= 0) return jz_abort(c, jz_fail(error,
            remaining < 0 ? remaining : -ETIMEDOUT, "receive-completion-deadline", 0));
        sched_yield();
    }
    const uint8_t *payload;
    int rc = jz_frame_decode(c->recv_buffer, sizeof(c->recv_buffer), JZ_MESSAGE, &payload, length);
    if (rc) return jz_abort(c, jz_fail(error, rc, "control-message-protocol", 0));
    memcpy(data, payload, *length);
    rc = jz_post_receive(c, error);
    return rc ? jz_abort(c, rc) : 0;
}

static int transfer(jz_connection *c, jz_region *local, size_t local_offset,
                      const struct jz_remote_region *remote, uint64_t remote_offset,
                      size_t length, bool read, struct jz_error *error)
{
    jz_clear_error(error);
    if (!c || !c->ready || !local || local->connection != c || !remote || !length || local->busy)
        return jz_fail(error, -EINVAL, "transfer-arguments", 0);
    if (remote->session != c->peer_session) return jz_fail(error, -ESTALE, "remote-session", 0);
    unsigned permission = read ? JZ_REMOTE_READ : JZ_REMOTE_WRITE;
    if (!(remote->access & permission)) return jz_fail(error, -EACCES, "remote-permission", 0);
    if (remote->address > UINT64_MAX - remote->length ||
        !jz_range_valid(local->length, local_offset, length) ||
        !jz_range_valid(remote->length, remote_offset, length))
        return jz_fail(error, -ERANGE, "transfer-range", 0);
    if (length > UINT32_MAX || length > c->max_message)
        return jz_fail(error, -EMSGSIZE, "single-wr-limit", 0);
    struct ibv_sge sge = { .addr = (uint64_t)(uintptr_t)local->mr->addr + local_offset,
                           .length = (uint32_t)length, .lkey = local->mr->lkey };
    struct ibv_send_wr wr = { .wr_id = JZ_WR_DATA, .sg_list = &sge, .num_sge = 1,
        .opcode = read ? IBV_WR_RDMA_READ : IBV_WR_RDMA_WRITE, .send_flags = IBV_SEND_SIGNALED,
        .wr.rdma = { .remote_addr = remote->address + remote_offset, .rkey = remote->rkey } };
    struct ibv_send_wr *bad = NULL;
    local->busy = true;
    int rc = ibv_post_send(c->qp, &wr, &bad);
    if (rc) {
        local->busy = false; /* A single rejected WR has no posted prefix. */
        return jz_abort(c, jz_fail(error, -rc, "post-rdma", rc));
    }
    rc = jz_wait_send(c, JZ_WR_DATA, read ? IBV_WC_RDMA_READ : IBV_WC_RDMA_WRITE,
                      jz_now_ms() + c->options.timeout_ms, error);
    if (rc) return jz_abort(c, rc);
    local->busy = false;
    return 0;
}

int jz_write(jz_connection *c, jz_region *local, size_t local_offset,
               const struct jz_remote_region *remote, uint64_t remote_offset,
               size_t length, struct jz_error *error)
{
    return transfer(c, local, local_offset, remote, remote_offset, length, false, error);
}

int jz_read(jz_connection *c, jz_region *local, size_t local_offset,
              const struct jz_remote_region *remote, uint64_t remote_offset,
              size_t length, struct jz_error *error)
{
    return transfer(c, local, local_offset, remote, remote_offset, length, true, error);
}
