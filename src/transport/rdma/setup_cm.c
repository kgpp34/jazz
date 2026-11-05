#include "transport/rdma/control.h"
#include "common/wait.h"
#include <errno.h>
#include <string.h>

int jz_control_start_cm(struct jz_rdma_control *control, struct jz_rdma_endpoint *ep,
                        struct jz_error *error)
{
    if (rdma_create_id(control->channel, &ep->job.id, &ep->owner, RDMA_PS_TCP))
        return jz_fail(error, errno, "cm-create-id");
    ep->phase = JZ_EP_CM_ADDR;
    if (rdma_resolve_addr(ep->job.id, (struct sockaddr *)&ep->config.local,
                          (struct sockaddr *)&ep->remote, (int)ep->config.timeout_ms))
        return jz_fail(error, errno, "cm-resolve-address");
    return 0;
}
static void reject_request(struct jz_rdma_control *control, struct jz_listen_source *source,
                           struct rdma_cm_id *id)
{
    struct jz_rejected_id *target = &source->overflow_reject;
    for (size_t i = 0; i < source->reject_capacity; ++i) {
        struct jz_rejected_id *reject = &source->rejects[i];
        if (reject->job.id)
            continue;
        target = reject;
        break;
    }
    /* The event reader never gets another event while the reserve is occupied. */
    target->owner.kind = JZ_OWNER_REJECT;
    target->job.id = id;
    atomic_store(&target->job.state, JZ_CM_IDLE);
    id->context = &target->owner;
    rdma_reject(id, NULL, 0);
    jz_control_schedule_id(control, &target->job);
}
static void passive_request(struct jz_rdma_control *control, struct jz_listen_source *source,
                            struct rdma_cm_id *id, const uint8_t *data, size_t length)
{
    struct jz_connection_message hello;
    struct jz_listener_info listener_info;
    jz_listener_get_info(source->handle, &listener_info, NULL);
    struct jz_rdma_endpoint *ep = jz_control_allocate_endpoint(control);
    jz_peer *peer = NULL;
    struct jz_listener_ticket ticket;
    if (listener_info.state != JZ_LISTENER_LISTENING || !ep ||
        jz_connection_decode(&hello, data, length, true) || hello.type != JZ_WIRE_HELLO ||
        jz_listener_admit(source->handle, &ticket, &peer, NULL)) {
        reject_request(control, source, id);
        return;
    }
    ep->peer = peer;
    jz_peer_retain(peer);
    ep->job.id = id;
    id->context = &ep->owner;
    ep->config = source->config;
    ep->source = source;
    ep->setup_ticket = ticket;
    ep->passive = true;
    ep->phase = JZ_EP_CM_CONNECT;
    ep->remote_msg = hello;
    ep->local = (struct jz_connection_message){
        .type = JZ_WIRE_OFFER, .setup = JZ_RDMA_SETUP_CM, .attempt = hello.attempt};
    memcpy(ep->local.instance, control->instance, 16);
    struct jz_error error = {0};
    int rc = jz_wait_deadline(ep->config.timeout_ms, &ep->deadline, &error);
    if (!rc && rdma_migrate_id(id, control->channel))
        rc = jz_fail(&error, errno, "cm-migrate");
    if (!rc)
        rc = jz_control_sequence(control, &ep->local.session, &error);
    if (!rc)
        rc = jz_control_prepare_qp(control, ep, true, &error);
    if (!rc) {
        jz_connection_encode(ep->tx, &ep->local, true);
        struct rdma_conn_param param = {.private_data = ep->tx,
                                        .private_data_len = JZ_CM_HELLO_BYTES,
                                        .responder_resources = 1,
                                        .initiator_depth = 1,
                                        .rnr_retry_count = 7};
        if (rdma_accept(id, &param))
            rc = jz_fail(&error, errno, "cm-accept");
    }
    if (rc)
        jz_control_fail(ep, error.code, error.stage);
}
static bool rejection_capacity(struct jz_rdma_control *control, struct rdma_event_channel *channel)
{
    for (size_t i = 0; i < control->backend->max_listeners; ++i) {
        struct jz_listen_source *source = &control->sources[i];
        if (!source->handle || source->channel != channel)
            continue;
        if (source->overflow_reject.job.id)
            return false;
        size_t live = 0;
        for (size_t n = 0; n < source->reject_capacity; ++n)
            live += source->rejects[n].job.id != NULL;
        struct jz_listener_info info;
        jz_listener_get_info(source->handle, &info, NULL);
        /* A separate close reserve drains already queued listen requests while
         * the reaper removes the kernel listen ID. Peers use another channel. */
        return live < (info.state == JZ_LISTENER_LISTENING ? source->reject_limit
                                                           : source->reject_capacity);
    }
    return true;
}
void jz_control_handle_cm(struct jz_rdma_control *control, struct rdma_event_channel *channel)
{
    for (unsigned turn = 0; turn < 32; ++turn) {
        if (!rejection_capacity(control, channel))
            return;
        struct rdma_cm_event *event = NULL;
        if (rdma_get_cm_event(channel, &event))
            return;
        enum rdma_cm_event_type type = event->event;
        int status = event->status;
        struct rdma_cm_id *id = event->id;
        struct jz_cm_owner *owner =
            type == RDMA_CM_EVENT_CONNECT_REQUEST ? event->listen_id->context : id->context;
        uint8_t data[256] = {0};
        size_t length = 0;
        if (type == RDMA_CM_EVENT_CONNECT_REQUEST || type == RDMA_CM_EVENT_ESTABLISHED) {
            length = event->param.conn.private_data_len;
            if (length && event->param.conn.private_data)
                memcpy(data, event->param.conn.private_data, length);
            else
                length = 0;
        }
        /* Copy first. Ack can unblock destroy_id and invalidate event and id. */
        rdma_ack_cm_event(event);
        if (!owner)
            continue;
        if (type == RDMA_CM_EVENT_CONNECT_REQUEST) {
            passive_request(control, (struct jz_listen_source *)owner, id, data, length);
            continue;
        }
        if (owner->kind == JZ_OWNER_REJECT)
            continue;
        if (owner->kind == JZ_OWNER_SOURCE) {
            if (type == RDMA_CM_EVENT_DEVICE_REMOVAL)
                jz_listener_request_close(((struct jz_listen_source *)owner)->handle, NULL);
            continue;
        }
        struct jz_rdma_endpoint *ep = (struct jz_rdma_endpoint *)owner;
        if (!ep->peer || ep->phase == JZ_EP_CLOSING || atomic_load(&ep->job.state) != JZ_CM_IDLE)
            continue;
        struct jz_error error = {0};
        int rc = 0;
        if (type == RDMA_CM_EVENT_ADDR_RESOLVED && ep->phase == JZ_EP_CM_ADDR) {
            ep->phase = JZ_EP_CM_ROUTE;
            if (rdma_resolve_route(id, (int)ep->config.timeout_ms))
                rc = jz_fail(&error, errno, "cm-route");
        } else if (type == RDMA_CM_EVENT_ROUTE_RESOLVED && ep->phase == JZ_EP_CM_ROUTE) {
            rc = jz_control_prepare_qp(control, ep, true, &error);
            if (!rc) {
                ep->local.type = JZ_WIRE_HELLO;
                jz_connection_encode(ep->tx, &ep->local, true);
                struct rdma_conn_param param = {.private_data = ep->tx,
                                                .private_data_len = JZ_CM_HELLO_BYTES,
                                                .responder_resources = 1,
                                                .initiator_depth = 1,
                                                .retry_count = 7,
                                                .rnr_retry_count = 7};
                ep->phase = JZ_EP_CM_CONNECT;
                if (rdma_connect(id, &param))
                    rc = jz_fail(&error, errno, "cm-connect");
            }
        } else if (type == RDMA_CM_EVENT_ESTABLISHED && ep->phase == JZ_EP_CM_CONNECT) {
            ep->cm_established = true;
            if (!ep->passive) {
                struct jz_connection_message offer;
                rc = jz_connection_decode(&offer, data, length, true);
                if (rc || offer.type != JZ_WIRE_OFFER || offer.attempt != ep->local.attempt)
                    rc = jz_fail(&error, EPROTO, "cm-offer");
                else {
                    ep->remote_msg = offer;
                    ep->local.session = offer.session;
                }
            }
            if (!rc)
                rc = jz_control_cm_check(ep, &error);
            if (!rc)
                jz_control_ready(ep);
        } else if (type != RDMA_CM_EVENT_TIMEWAIT_EXIT) {
            int cause = status < 0 ? -status : status;
            if (type == RDMA_CM_EVENT_REJECTED)
                cause = ECONNREFUSED;
            else if (type == RDMA_CM_EVENT_DISCONNECTED)
                cause = ECONNRESET;
            if (!cause || cause > 4095)
                cause = EIO;
            rc = jz_fail(&error, cause, "cm-event");
        }
        if (rc)
            jz_control_fail(ep, error.code, error.stage);
    }
}
