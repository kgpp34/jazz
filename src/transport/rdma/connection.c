#include "transport/rdma/control.h"
#include "runtime/engine_internal.h"
#include "common/wait.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void jz_rdma_listen_options_init(struct jz_rdma_listen_options *options)
{
    if (options)
        *options = (struct jz_rdma_listen_options){.setup = JZ_RDMA_SETUP_CM,
                                                   .port = 1,
                                                   .gid_index = -1,
                                                   .timeout_ms = 5000,
                                                   .max_pending = 16,
                                                   .capacity = 32};
}
void jz_rdma_connect_options_init(struct jz_rdma_connect_options *options)
{
    if (options)
        *options = (struct jz_rdma_connect_options){
            .setup = JZ_RDMA_SETUP_CM, .port = 1, .gid_index = -1, .timeout_ms = 5000};
}
static int connection_backend(jz_engine *engine, bool ready, struct jz_rdma_backend **out,
                              struct jz_error *error)
{
    if (!engine)
        return jz_fail(error, EINVAL, "engine-null");
    if (engine->state != JZ_ENGINE_READY && (ready || engine->state != JZ_ENGINE_CLEANUP_REQUIRED))
        return jz_fail(error, EBUSY, "connection-engine-state");
    struct jz_rdma_backend *backend = jz_rdma_backend_find(engine);
    if (!backend)
        return jz_fail(error, ENODEV, "rdma-not-configured");
    /* Fake resource fixtures are not native verbs objects. Never bypass their
     * test boundary by sending them to the real kernel connection API. */
    if (backend->driver.calls != &jz_rdma_system_calls)
        return jz_fail(error, EOPNOTSUPP, "connection-native-driver");
    *out = backend;
    return 0;
}
static int bind_source(struct jz_rdma_control *control, struct jz_listen_source *source,
                       size_t backlog, struct jz_error *error)
{
    if (source->config.setup == JZ_RDMA_SETUP_OOB) {
        source->socket =
            socket(source->config.local.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (source->socket < 0)
            return jz_fail(error, errno, "listen-socket");
        int one = 1;
        if (setsockopt(source->socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)))
            return jz_fail(error, errno, "listen-reuseaddr");
        if (bind(source->socket, (struct sockaddr *)&source->config.local,
                 jz_control_address_size(&source->config.local)))
            return jz_fail(error, errno, "listen-bind");
        if (listen(source->socket, (int)backlog))
            return jz_fail(error, errno, "listen-tcp");
        socklen_t length = sizeof(source->config.local);
        if (getsockname(source->socket, (struct sockaddr *)&source->config.local, &length))
            return jz_fail(error, errno, "listen-address");
    } else {
        source->channel = rdma_create_event_channel();
        if (!source->channel)
            return jz_fail(error, errno, "cm-listen-channel");
        if (fcntl(source->channel->fd, F_SETFL, O_NONBLOCK) ||
            fcntl(source->channel->fd, F_SETFD, FD_CLOEXEC))
            return jz_fail(error, errno, "cm-listen-nonblocking");
        if (rdma_create_id(source->channel, &source->job.id, &source->owner, RDMA_PS_TCP))
            return jz_fail(error, errno, "cm-listen-id");
        if (rdma_bind_addr(source->job.id, (struct sockaddr *)&source->config.local))
            return jz_fail(error, errno, "cm-listen-bind");
        struct jz_rdma_device *device = &control->backend->devices[source->config.device];
        if (source->job.id->verbs != device->lease.verbs ||
            source->job.id->port_num != source->config.port)
            return jz_fail(error, EXDEV, "cm-listen-device");
        if (rdma_listen(source->job.id, (int)backlog))
            return jz_fail(error, errno, "cm-listen");
        source->bound.service_port = ntohs(rdma_get_src_port(source->job.id));
        /* The helper returns the network-order port in rdma-core. */
    }
    if (source->config.setup == JZ_RDMA_SETUP_OOB)
        source->bound.service_port = jz_control_get_port(&source->config.local);
    if (!source->bound.service_port)
        return jz_fail(error, EIO, "listen-zero-port");
    return 0;
}
int jz_rdma_listen(jz_engine *engine, const struct jz_rdma_listen_options *options,
                   jz_listener **out, struct jz_error *error)
{
    jz_error_clear(error);
    if (!options || !out || *out)
        return jz_fail(error, EINVAL, "listen-output");
    struct jz_rdma_backend *backend = NULL;
    int rc = connection_backend(engine, true, &backend, error);
    if (rc)
        return rc;
    if (!options->max_pending || !options->capacity ||
        options->max_pending > backend->max_connections ||
        options->capacity > backend->max_connections)
        return jz_fail(error, EINVAL, "listen-capacity");
    struct jz_bind_config config;
    rc = jz_control_validate_bind(backend, options->local_ip, options->setup, options->device_index,
                                  options->port, options->gid_index, options->timeout_ms, &config,
                                  error);
    if (rc)
        return rc;
    jz_control_set_port(&config.local, options->service_port);
    pthread_mutex_lock(&backend->control_lock);
    rc = jz_control_ensure(backend, error);
    if (rc) {
        pthread_mutex_unlock(&backend->control_lock);
        return rc;
    }
    struct jz_rdma_control *control = backend->control;
    pthread_mutex_lock(&control->lock);
    struct jz_listen_source *source = NULL;
    for (size_t i = 0; i < backend->max_listeners; ++i)
        if (!control->sources[i].handle) {
            source = &control->sources[i];
            break;
        }
    if (!source || control->stopping) {
        rc = jz_fail(error, source ? ESHUTDOWN : ENOSPC, "listen-source-limit");
        goto done;
    }
    jz_listener *handle = NULL;
    rc = jz_listener_create(&handle, options->max_pending, options->capacity, error);
    if (rc)
        goto done;
    *source = (struct jz_listen_source){
        .owner = {.kind = JZ_OWNER_SOURCE},
        .handle = handle,
        .config = config,
        .socket = -1,
        .reject_limit = options->max_pending,
        .reject_capacity = options->setup == JZ_RDMA_SETUP_CM ? options->max_pending * 2 : 0};
    atomic_init(&source->job.state, JZ_CM_IDLE);
    atomic_init(&source->overflow_reject.job.state, JZ_CM_IDLE);
    jz_listener_retain(handle);
    if (source->reject_capacity) {
        source->rejects = calloc(source->reject_capacity, sizeof(*source->rejects));
        if (!source->rejects) {
            source->reject_capacity = 0;
            rc = jz_fail(error, ENOMEM, "cm-reject-reserve");
        } else
            for (size_t i = 0; i < source->reject_capacity; ++i)
                atomic_init(&source->rejects[i].job.state, JZ_CM_IDLE);
    }
    if (!rc)
        rc = bind_source(control, source, options->max_pending, error);
    if (rc) {
        /* The service retains partial sources for retryable cleanup; the caller
         * never receives a misleading bound listener on failure. */
        jz_listener_request_close(handle, NULL);
        jz_listener_release(handle);
    } else {
        source->bound.setup = config.setup;
        size_t length = strlen(options->local_ip);
        memcpy(source->bound.ip, options->local_ip, length + 1);
        *out = handle;
    }
    jz_control_wake(control);
done:
    pthread_mutex_unlock(&control->lock);
    pthread_mutex_unlock(&backend->control_lock);
    return rc;
}
int jz_rdma_connect(jz_engine *engine, const struct jz_rdma_connect_options *options, jz_peer **out,
                    struct jz_error *error)
{
    jz_error_clear(error);
    if (!options || !out || *out || !options->service_port)
        return jz_fail(error, EINVAL, "connect-output");
    struct jz_rdma_backend *backend = NULL;
    int rc = connection_backend(engine, true, &backend, error);
    if (rc)
        return rc;
    struct jz_bind_config config;
    rc = jz_control_validate_bind(backend, options->local_ip, options->setup, options->device_index,
                                  options->port, options->gid_index, options->timeout_ms, &config,
                                  error);
    if (rc)
        return rc;
    struct sockaddr_storage remote;
    rc = jz_control_parse_address(options->remote_ip, options->service_port, &remote, error);
    if (rc)
        return rc;
    if (remote.ss_family != config.local.ss_family)
        return jz_fail(error, EAFNOSUPPORT, "connect-family");
    struct timespec deadline;
    rc = jz_wait_deadline(options->timeout_ms, &deadline, error);
    if (rc)
        return rc;
    pthread_mutex_lock(&backend->control_lock);
    rc = jz_control_ensure(backend, error);
    if (rc) {
        pthread_mutex_unlock(&backend->control_lock);
        return rc;
    }
    struct jz_rdma_control *control = backend->control;
    pthread_mutex_lock(&control->lock);
    struct jz_rdma_endpoint *ep = jz_control_allocate_endpoint(control);
    if (!ep || control->stopping) {
        rc = jz_fail(error, ep ? ESHUTDOWN : ENOSPC, "connection-limit");
        goto done;
    }
    uint64_t attempt;
    rc = jz_control_sequence(control, &attempt, error);
    if (rc)
        goto done;
    jz_peer *peer = NULL;
    rc = jz_peer_create(&peer, error);
    if (rc)
        goto done;
    jz_peer_retain(peer);
    ep->peer = peer;
    ep->config = config;
    ep->remote = remote;
    ep->deadline = deadline;
    ep->local = (struct jz_connection_message){.setup = config.setup, .attempt = attempt};
    memcpy(ep->local.instance, control->instance, 16);
    *out = peer;
    jz_control_wake(control);
done:
    pthread_mutex_unlock(&control->lock);
    pthread_mutex_unlock(&backend->control_lock);
    return rc;
}
int jz_rdma_listener_address(jz_engine *engine, const jz_listener *listener,
                             struct jz_rdma_bound_address *out, struct jz_error *error)
{
    jz_error_clear(error);
    if (!listener || !out)
        return jz_fail(error, EINVAL, "listen-address-output");
    struct jz_rdma_backend *backend = NULL;
    int rc = connection_backend(engine, false, &backend, error);
    if (rc)
        return rc;
    pthread_mutex_lock(&backend->control_lock);
    struct jz_rdma_control *control = backend->control;
    rc = jz_fail(error, ENOENT, "listen-address-missing");
    if (control) {
        pthread_mutex_lock(&control->lock);
        for (size_t i = 0; i < backend->max_listeners; ++i)
            if (control->sources[i].handle == listener) {
                *out = control->sources[i].bound;
                jz_error_clear(error);
                rc = 0;
                break;
            }
        pthread_mutex_unlock(&control->lock);
    }
    pthread_mutex_unlock(&backend->control_lock);
    return rc;
}
int jz_rdma_peer_query(jz_engine *engine, const jz_peer *peer, struct jz_rdma_connection_info *out,
                       struct jz_error *error)
{
    jz_error_clear(error);
    if (!peer || !out)
        return jz_fail(error, EINVAL, "connection-query-output");
    struct jz_rdma_backend *backend = NULL;
    int rc = connection_backend(engine, false, &backend, error);
    if (rc)
        return rc;
    pthread_mutex_lock(&backend->control_lock);
    struct jz_rdma_control *control = backend->control;
    rc = jz_fail(error, ENOENT, "connection-missing");
    if (control) {
        pthread_mutex_lock(&control->lock);
        for (size_t i = 0; i < backend->max_connections; ++i) {
            struct jz_rdma_endpoint *ep = &control->endpoints[i];
            if (ep->peer != peer)
                continue;
            struct jz_peer_info info;
            jz_peer_get_info(peer, &info, NULL);
            if (ep->phase != JZ_EP_ACTIVE || info.state != JZ_PEER_ACTIVE) {
                rc = jz_fail(error, EAGAIN, "connection-not-ready");
                break;
            }
            struct ibv_qp_attr attr;
            struct ibv_qp_init_attr init;
            int cause = ibv_query_qp(ep->qp, &attr, IBV_QP_STATE, &init);
            if (cause || attr.qp_state != IBV_QPS_RTS) {
                rc = jz_fail(error, cause ? cause : EIO, "connection-qp-state");
                break;
            }
            struct jz_rdma_connection_info result = {.setup = ep->config.setup,
                                                     .device_index = ep->config.device,
                                                     .local_qpn = ep->qp->qp_num,
                                                     .remote_qpn = ep->remote_msg.qpn,
                                                     .mtu = 128u << (unsigned)ep->local.mtu,
                                                     .attempt = ep->local.attempt,
                                                     .session = ep->local.session};
            memcpy(result.local_instance, ep->local.instance, 16);
            memcpy(result.remote_instance, ep->remote_msg.instance, 16);
            *out = result;
            jz_error_clear(error);
            rc = 0;
            break;
        }
        pthread_mutex_unlock(&control->lock);
    }
    pthread_mutex_unlock(&backend->control_lock);
    return rc;
}
