#include "transport/rdma/control.h"
#include "common/wait.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>

static int begin_hello(struct jz_rdma_control *control, struct jz_rdma_endpoint *ep,
                       struct jz_error *error)
{
    int rc = jz_control_prepare_qp(control, ep, false, error);
    if (rc)
        return rc;
    ep->local.type = JZ_WIRE_HELLO;
    jz_connection_encode(ep->tx, &ep->local, false);
    ep->phase = JZ_EP_SEND_HELLO;
    return 0;
}
int jz_control_begin_endpoint(struct jz_rdma_control *control, struct jz_rdma_endpoint *ep,
                              struct jz_error *error)
{
    if (ep->config.setup == JZ_RDMA_SETUP_CM)
        return jz_control_start_cm(control, ep, error);
    ep->socket = socket(ep->config.local.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (ep->socket < 0)
        return jz_fail(error, errno, "oob-socket");
    if (bind(ep->socket, (struct sockaddr *)&ep->config.local,
             jz_control_address_size(&ep->config.local)))
        return jz_fail(error, errno, "oob-bind");
    int rc =
        connect(ep->socket, (struct sockaddr *)&ep->remote, jz_control_address_size(&ep->remote));
    if (!rc)
        return begin_hello(control, ep, error);
    if (errno != EINPROGRESS)
        return jz_fail(error, errno, "oob-connect");
    ep->phase = JZ_EP_TCP_CONNECT;
    return 0;
}
void jz_control_accept_tcp(struct jz_rdma_control *control, struct jz_listen_source *source)
{
    struct jz_listener_info info;
    jz_listener_get_info(source->handle, &info, NULL);
    if (info.state != JZ_LISTENER_LISTENING)
        return;
    /* Bound work per turn so slow/flooded listeners cannot monopolize control. */
    for (unsigned turn = 0; turn < 16; ++turn) {
        int fd = accept4(source->socket, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        struct jz_rdma_endpoint *ep = jz_control_allocate_endpoint(control);
        jz_peer *peer = NULL;
        struct jz_listener_ticket ticket;
        if (!ep || jz_listener_admit(source->handle, &ticket, &peer, NULL)) {
            close(fd);
            continue;
        }
        ep->peer = peer;
        jz_peer_retain(peer);
        ep->socket = fd;
        ep->config = source->config;
        ep->source = source;
        ep->setup_ticket = ticket;
        ep->passive = true;
        ep->phase = JZ_EP_RECV_HELLO;
        ep->local.setup = JZ_RDMA_SETUP_OOB;
        memcpy(ep->local.instance, control->instance, 16);
        struct jz_error error;
        if (jz_wait_deadline(ep->config.timeout_ms, &ep->deadline, &error))
            jz_control_fail(ep, error.code, error.stage);
    }
}
static bool same_identity(const struct jz_rdma_endpoint *ep,
                          const struct jz_connection_message *msg)
{
    return msg->attempt == ep->local.attempt && msg->session == ep->local.session &&
           !memcmp(msg->instance, ep->remote_msg.instance, 16) && msg->qpn == ep->remote_msg.qpn &&
           msg->psn == ep->remote_msg.psn && !memcmp(msg->gid.raw, ep->remote_msg.gid.raw, 16) &&
           msg->mtu == ep->local.mtu && msg->reads == ep->remote_msg.reads;
}
static int receive_frame(struct jz_rdma_control *control, struct jz_rdma_endpoint *ep,
                         struct jz_error *error)
{
    struct jz_connection_message msg;
    int rc = jz_connection_decode(&msg, ep->rx, sizeof(ep->rx), false);
    if (rc)
        return jz_fail(error, EPROTO, "oob-frame");
    if (ep->phase == JZ_EP_RECV_HELLO) {
        if (msg.type != JZ_WIRE_HELLO)
            return jz_fail(error, EPROTO, "oob-hello");
        ep->remote_msg = msg;
        ep->local.attempt = msg.attempt;
        rc = jz_control_sequence(control, &ep->local.session, error);
        if (!rc)
            rc = jz_control_prepare_qp(control, ep, false, error);
        if (!rc)
            rc = jz_control_oob_connect_qp(ep, error);
        if (rc)
            return rc;
        ep->local.type = JZ_WIRE_OFFER;
        ep->phase = JZ_EP_SEND_OFFER;
    } else if (ep->phase == JZ_EP_RECV_OFFER) {
        if (msg.type != JZ_WIRE_OFFER || msg.attempt != ep->local.attempt ||
            msg.mtu > ep->local.mtu)
            return jz_fail(error, EPROTO, "oob-offer");
        ep->remote_msg = msg;
        ep->local.session = msg.session;
        rc = jz_control_oob_connect_qp(ep, error);
        if (rc)
            return rc;
        ep->local.type = JZ_WIRE_READY;
        ep->phase = JZ_EP_SEND_READY;
    } else if (ep->phase == JZ_EP_RECV_READY) {
        if (msg.type != JZ_WIRE_READY || !same_identity(ep, &msg))
            return jz_fail(error, EPROTO, "oob-ready");
        ep->local.type = JZ_WIRE_ACK;
        ep->phase = JZ_EP_SEND_ACK;
    } else {
        if (msg.type != JZ_WIRE_ACK || !same_identity(ep, &msg))
            return jz_fail(error, EPROTO, "oob-ack");
        jz_control_ready(ep);
        return 0;
    }
    ep->sent = 0;
    ep->received = 0;
    jz_connection_encode(ep->tx, &ep->local, false);
    return 0;
}
static void tcp_progress(struct jz_rdma_control *control, struct jz_rdma_endpoint *ep, short events)
{
    struct jz_error error = {0};
    if (ep->phase == JZ_EP_TCP_CONNECT) {
        if (!(events & (POLLOUT | POLLERR | POLLHUP | POLLNVAL)))
            return;
        int cause = 0;
        socklen_t length = sizeof(cause);
        if (getsockopt(ep->socket, SOL_SOCKET, SO_ERROR, &cause, &length))
            cause = errno;
        if (cause) {
            jz_control_fail(ep, cause, "oob-connect");
            return;
        }
        if (begin_hello(control, ep, &error))
            jz_control_fail(ep, error.code, error.stage);
        return;
    }
    if (events & (POLLERR | POLLNVAL)) {
        jz_control_fail(ep, ECONNRESET, "oob-socket-event");
        return;
    }
    if (ep->phase == JZ_EP_ACTIVE) {
        if (events & (POLLIN | POLLHUP)) {
            uint8_t byte;
            ssize_t n = recv(ep->socket, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
            if (!n)
                jz_control_fail(ep, ECONNRESET, "oob-peer-closed");
            else if (n > 0)
                jz_control_fail(ep, EPROTO, "oob-unexpected-data");
            else if (errno != EAGAIN && errno != EINTR)
                jz_control_fail(ep, errno, "oob-recv");
        }
        return;
    }
    bool sending = ep->phase == JZ_EP_SEND_HELLO || ep->phase == JZ_EP_SEND_OFFER ||
                   ep->phase == JZ_EP_SEND_READY || ep->phase == JZ_EP_SEND_ACK;
    if (sending) {
        if (!(events & (POLLOUT | POLLHUP)))
            return;
        ssize_t n = send(ep->socket, ep->tx + ep->sent, sizeof(ep->tx) - ep->sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno != EAGAIN && errno != EINTR)
                jz_control_fail(ep, errno, "oob-send");
            return;
        }
        ep->sent += (size_t)n;
        if (ep->sent != sizeof(ep->tx))
            return;
        ep->received = 0;
        if (ep->phase == JZ_EP_SEND_ACK) {
            jz_control_ready(ep);
            return;
        }
        ep->phase = ep->phase == JZ_EP_SEND_HELLO   ? JZ_EP_RECV_OFFER
                    : ep->phase == JZ_EP_SEND_OFFER ? JZ_EP_RECV_READY
                                                    : JZ_EP_RECV_ACK;
    } else if (events & (POLLIN | POLLHUP)) {
        ssize_t n = recv(ep->socket, ep->rx + ep->received, sizeof(ep->rx) - ep->received, 0);
        if (n < 0) {
            if (errno != EAGAIN && errno != EINTR)
                jz_control_fail(ep, errno, "oob-recv");
            return;
        }
        if (!n) {
            jz_control_fail(ep, ECONNRESET, "oob-peer-closed");
            return;
        }
        ep->received += (size_t)n;
        if (ep->received == sizeof(ep->rx) && receive_frame(control, ep, &error))
            jz_control_fail(ep, error.code ? error.code : ECANCELED,
                            error.stage ? error.stage : "oob-publication");
    }
}
void jz_control_endpoint_progress(struct jz_rdma_control *control, struct jz_rdma_endpoint *ep,
                                  short events)
{
    struct jz_peer_info info;
    jz_peer_get_info(ep->peer, &info, NULL);
    if (info.state == JZ_PEER_STOPPING || info.state == JZ_PEER_CLOSED)
        ep->phase = JZ_EP_CLOSING;
    if (ep->phase == JZ_EP_CLOSING)
        return;
    if (ep->phase != JZ_EP_ACTIVE) {
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) {
            jz_control_fail(ep, errno, "setup-clock");
            return;
        }
        if (now.tv_sec > ep->deadline.tv_sec ||
            (now.tv_sec == ep->deadline.tv_sec && now.tv_nsec >= ep->deadline.tv_nsec)) {
            jz_control_fail(ep, ETIMEDOUT, "setup-deadline");
            return;
        }
    }
    if (ep->phase == JZ_EP_NEW) {
        struct jz_error error;
        if (jz_control_begin_endpoint(control, ep, &error))
            jz_control_fail(ep, error.code, error.stage);
    } else if (ep->config.setup == JZ_RDMA_SETUP_OOB)
        tcp_progress(control, ep, events);
}
