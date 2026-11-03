#include "rdma/internal.h"

#include <errno.h>
#include <poll.h>
#include <string.h>

/* Private data binds peer descriptors to this connection incarnation.
 * It is copied before acknowledging its CM event; no event pointer escapes. */
static void private_encode(jz_connection *c, uint8_t out[16])
{
    jz_put32(out, UINT32_C(0x4a434d31));
    jz_put32(out + 4, 1);
    jz_put64(out + 8, c->session);
}

static int private_decode(jz_connection *c, const struct rdma_cm_event *event,
                           struct jz_error *error)
{
    const uint8_t *bytes = event->param.conn.private_data;
    if (!bytes || event->param.conn.private_data_len < 16 ||
        jz_get32(bytes) != UINT32_C(0x4a434d31) || jz_get32(bytes + 4) != 1 ||
        !jz_get64(bytes + 8))
        return jz_fail(error, -EPROTO, "cm-peer-protocol", 0);
    c->peer_session = jz_get64(bytes + 8);
    return 0;
}

static int new_channel(struct rdma_event_channel **out, struct jz_error *error)
{
    *out = rdma_create_event_channel();
    if (!*out) return jz_fail(error, -errno, "cm-event-channel", errno);
    int rc = jz_nonblock((*out)->fd);
    return rc ? jz_fail(error, rc, "cm-event-nonblock", -rc) : 0;
}

static int next_event(struct rdma_event_channel *channel, int64_t deadline,
                       struct rdma_cm_event **out, struct jz_error *error)
{
    for (;;) {
        int rc = jz_wait_fd(channel->fd, POLLIN, deadline);
        if (rc) return jz_fail(error, rc, "cm-event-wait", -rc);
        if (!rdma_get_cm_event(channel, out)) return 0;
        if (errno != EAGAIN && errno != EINTR)
            return jz_fail(error, -errno, "cm-event-read", errno);
    }
}

static int expect_event(jz_connection *c, enum rdma_cm_event_type expected,
                         int64_t deadline, struct jz_error *error)
{
    struct rdma_cm_event *event = NULL;
    int rc = next_event(c->channel, deadline, &event, error);
    if (rc) return rc;
    if (event->event != expected || event->id != c->id || event->status)
        rc = jz_fail(error, -ECONNABORTED, "cm-unexpected-event", event->status);
    else if (expected == RDMA_CM_EVENT_ESTABLISHED && !c->peer_session)
        rc = private_decode(c, event, error);
    if (rdma_ack_cm_event(event) && !rc) rc = jz_fail(error, -errno, "cm-event-ack", errno);
    if (!rc && expected == RDMA_CM_EVENT_ESTABLISHED) c->established = true;
    return rc;
}

int jz_cm_listen(jz_listener *l, struct jz_error *error)
{
    int rc = new_channel(&l->channel, error);
    if (rc) return rc;
    if (rdma_create_id(l->channel, &l->id, NULL, RDMA_PS_TCP))
        return jz_fail(error, -errno, "cm-listener-id", errno);
    if (rdma_bind_addr(l->id, (struct sockaddr *)&l->options.local))
        return jz_fail(error, -errno, "cm-bind", errno);
    if (rdma_listen(l->id, 16)) return jz_fail(error, -errno, "cm-listen", errno);
    l->port = ntohs(rdma_get_src_port(l->id));
    return 0;
}

int jz_cm_connect(jz_connection *c, int64_t deadline, struct jz_error *error)
{
    int rc = new_channel(&c->channel, error);
    if (rc) return rc;
    if (rdma_create_id(c->channel, &c->id, NULL, RDMA_PS_TCP))
        return jz_fail(error, -errno, "cm-connection-id", errno);
    int remaining = jz_remaining(deadline);
    if (remaining <= 0) return jz_fail(error, -ETIMEDOUT, "cm-resolve-deadline", 0);
    if (rdma_resolve_addr(c->id, (struct sockaddr *)&c->options.local,
                          (struct sockaddr *)&c->options.peer, remaining))
        return jz_fail(error, -errno, "cm-resolve-address", errno);
    rc = expect_event(c, RDMA_CM_EVENT_ADDR_RESOLVED, deadline, error);
    if (rc) return rc;
    remaining = jz_remaining(deadline);
    if (remaining <= 0) return jz_fail(error, -ETIMEDOUT, "cm-route-deadline", 0);
    if (rdma_resolve_route(c->id, remaining)) return jz_fail(error, -errno, "cm-resolve-route", errno);
    rc = expect_event(c, RDMA_CM_EVENT_ROUTE_RESOLVED, deadline, error);
    if (rc) return rc;
    c->context = c->id->verbs;
    c->ib_port = c->id->port_num;
    rc = jz_validate_port(c, error);
    if (!rc) rc = jz_resources(c, true, error);
    if (rc) return rc;
    uint8_t bytes[16];
    private_encode(c, bytes);
    struct rdma_conn_param param = { .private_data = bytes, .private_data_len = sizeof(bytes),
        .responder_resources = 1, .initiator_depth = 1, .retry_count = 3, .rnr_retry_count = 7 };
    if (rdma_connect(c->id, &param)) return jz_fail(error, -errno, "cm-connect", errno);
    return expect_event(c, RDMA_CM_EVENT_ESTABLISHED, deadline, error);
}

int jz_cm_accept(jz_listener *l, jz_connection *c, int64_t deadline,
                 struct jz_error *error)
{
    struct rdma_cm_event *event = NULL;
    int rc = next_event(l->channel, deadline, &event, error);
    if (rc) return rc;
    if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST || event->status) {
        rc = jz_fail(error, -ECONNABORTED, "cm-listener-event", event->status);
        (void)rdma_ack_cm_event(event);
        return rc;
    }
    c->id = event->id;
    rc = private_decode(c, event, error);
    if (rdma_ack_cm_event(event) && !rc) rc = jz_fail(error, -errno, "cm-request-ack", errno);
    if (rc) { (void)rdma_reject(c->id, NULL, 0); return rc; }
    rc = new_channel(&c->channel, error);
    if (rc) return rc;
    if (rdma_migrate_id(c->id, c->channel)) return jz_fail(error, -errno, "cm-migrate-child", errno);
    c->context = c->id->verbs;
    c->ib_port = c->id->port_num;
    rc = jz_validate_port(c, error);
    if (!rc) rc = jz_resources(c, true, error);
    if (rc) { (void)rdma_reject(c->id, NULL, 0); return rc; }
    uint8_t bytes[16];
    private_encode(c, bytes);
    struct rdma_conn_param param = { .private_data = bytes, .private_data_len = sizeof(bytes),
        .responder_resources = 1, .initiator_depth = 1, .retry_count = 3, .rnr_retry_count = 7 };
    if (rdma_accept(c->id, &param)) return jz_fail(error, -errno, "cm-accept", errno);
    return expect_event(c, RDMA_CM_EVENT_ESTABLISHED, deadline, error);
}
