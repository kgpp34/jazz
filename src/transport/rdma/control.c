#include "transport/rdma/control.h"
#include "common/wait.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <unistd.h>
#include <fcntl.h>

void jz_control_wake(struct jz_rdma_control *control)
{
    uint64_t one = 1;
    ssize_t ignored = write(control->wake_fd, &one, sizeof(one));
    (void)ignored; /* EAGAIN means a wake is already pending. */
}
int jz_control_sequence(struct jz_rdma_control *control, uint64_t *out, struct jz_error *error)
{
    if (control->next_identity == UINT64_MAX)
        return jz_fail(error, EOVERFLOW, "session-sequence");
    *out = ++control->next_identity;
    return 0;
}
struct jz_rdma_endpoint *jz_control_allocate_endpoint(struct jz_rdma_control *control)
{
    for (size_t i = 0; i < control->backend->max_connections; ++i) {
        struct jz_rdma_endpoint *ep = &control->endpoints[i];
        if (ep->peer)
            continue;
        *ep = (struct jz_rdma_endpoint){.owner = {.kind = JZ_OWNER_ENDPOINT}, .socket = -1};
        atomic_init(&ep->job.state, JZ_CM_IDLE);
        return ep;
    }
    return NULL;
}
void jz_control_fail(struct jz_rdma_endpoint *ep, int cause, const char *stage)
{
    struct jz_error error = {.code = cause > 0 ? -cause : cause,
                             .system_errno = cause > 0 ? cause : -cause,
                             .stage = stage};
    jz_peer_fail(ep->peer, &error);
    ep->phase = JZ_EP_CLOSING;
}
int jz_control_ready(struct jz_rdma_endpoint *ep)
{
    int rc;
    if (ep->passive) {
        rc = jz_listener_publish(ep->source->handle, ep->setup_ticket, NULL);
        ep->setup_released = true;
        ep->source = NULL;
    } else
        rc = jz_peer_set_active(ep->peer, NULL);
    ep->phase = rc ? JZ_EP_CLOSING : JZ_EP_ACTIVE;
    return rc;
}

/* ID destruction may wait for events reported by the kernel but not yet acked.
 * Keep the event pump running while a separate, bounded reaper executes it. */
static void *reaper_thread(void *arg)
{
    struct jz_rdma_control *control = arg;
    pthread_mutex_lock(&control->reap_lock);
    for (;;) {
        while (!control->reap_head && !control->reap_exit)
            pthread_cond_wait(&control->reap_changed, &control->reap_lock);
        if (!control->reap_head && control->reap_exit)
            break;
        struct jz_cm_job *job = control->reap_head;
        control->reap_head = job->next;
        if (!control->reap_head)
            control->reap_tail = NULL;
        atomic_store_explicit(&job->state, JZ_CM_RUNNING, memory_order_release);
        pthread_mutex_unlock(&control->reap_lock);
        int rc = rdma_destroy_id(job->id);
        job->result = rc ? -(errno ? errno : EIO) : 0;
        atomic_store_explicit(&job->state, JZ_CM_DONE, memory_order_release);
        /* No access to job after DONE: the control owner may recycle its slot. */
        jz_control_wake(control);
        pthread_mutex_lock(&control->reap_lock);
    }
    pthread_mutex_unlock(&control->reap_lock);
    return NULL;
}
void jz_control_schedule_id(struct jz_rdma_control *control, struct jz_cm_job *job)
{
    if (!job->id || atomic_load_explicit(&job->state, memory_order_acquire) != JZ_CM_IDLE)
        return;
    pthread_mutex_lock(&control->reap_lock);
    job->next = NULL;
    atomic_store_explicit(&job->state, JZ_CM_QUEUED, memory_order_release);
    if (control->reap_tail)
        control->reap_tail->next = job;
    else
        control->reap_head = job;
    control->reap_tail = job;
    pthread_cond_signal(&control->reap_changed);
    pthread_mutex_unlock(&control->reap_lock);
}
int jz_control_id_result(struct jz_cm_job *job)
{
    if (atomic_load_explicit(&job->state, memory_order_acquire) == JZ_CM_DONE) {
        int rc = job->result;
        if (!rc)
            job->id = NULL;
        atomic_store_explicit(&job->state, JZ_CM_IDLE, memory_order_release);
        return rc;
    }
    return job->id ? -EINPROGRESS : 0;
}

static struct jz_rdma_endpoint *find_peer(struct jz_rdma_control *control, jz_peer *peer)
{
    for (size_t i = 0; i < control->backend->max_connections; ++i)
        if (control->endpoints[i].peer == peer)
            return &control->endpoints[i];
    return NULL;
}
static void assign_retirements(struct jz_rdma_control *control, struct jz_listen_source *source)
{
    for (;;) {
        jz_peer *peer = NULL;
        struct jz_listener_ticket ticket;
        if (jz_listener_take_retiring(source->handle, &ticket, &peer, NULL))
            break;
        struct jz_rdma_endpoint *ep = find_peer(control, peer);
        if (ep) {
            ep->retire_listener = source->handle;
            ep->retire_ticket = ticket;
        } else {
            /* Its endpoint already retired; only the unclaimed software handle remains. */
            jz_listener_retire_done(source->handle, ticket, NULL);
        }
    }
}
void jz_control_source_progress(struct jz_rdma_control *control, struct jz_listen_source *source)
{
    if (!source->handle)
        return;
    struct jz_listener_info info;
    jz_listener_get_info(source->handle, &info, NULL);
    for (size_t i = 0; i < source->reject_capacity; ++i) {
        struct jz_cm_job *job = &source->rejects[i].job;
        if (job->id) {
            jz_control_id_result(job);
            jz_control_schedule_id(control, job);
        }
    }
    if (source->overflow_reject.job.id) {
        jz_control_id_result(&source->overflow_reject.job);
        jz_control_schedule_id(control, &source->overflow_reject.job);
    }
    assign_retirements(control, source);
    if (info.state == JZ_LISTENER_LISTENING)
        return;
    if (source->socket >= 0) {
        close(source->socket);
        source->socket = -1;
    }
    if (source->job.id) {
        int rc = jz_control_id_result(&source->job);
        if (rc && rc != -EINPROGRESS) {
            struct jz_error error = {.code = rc, .system_errno = -rc, .stage = "cm-listen-destroy"};
            jz_listener_set_cleanup_error(source->handle, &error, NULL);
        }
        jz_control_schedule_id(control, &source->job);
    }
    bool rejected_live = source->overflow_reject.job.id != NULL;
    for (size_t i = 0; i < source->reject_capacity; ++i)
        rejected_live |= source->rejects[i].job.id != NULL;
    if (!source->job.id && !rejected_live)
        jz_listener_source_closed(source->handle, NULL);
    jz_listener_get_info(source->handle, &info, NULL);
    if (info.state == JZ_LISTENER_CLOSED) {
        if (source->channel)
            rdma_destroy_event_channel(source->channel);
        free(source->rejects);
        jz_listener *handle = source->handle;
        source->handle = NULL;
        source->rejects = NULL;
        source->channel = NULL;
        jz_listener_release(handle);
    }
}
static void cleanup_endpoint(struct jz_rdma_control *control, struct jz_rdma_endpoint *ep)
{
    if (ep->passive && !ep->setup_released) {
        struct jz_peer_info info;
        jz_peer_get_info(ep->peer, &info, NULL);
        jz_listener_abort(ep->source->handle, ep->setup_ticket,
                          info.terminal_error.code ? &info.terminal_error : NULL, NULL);
        ep->setup_released = true;
        ep->source = NULL;
    }
    if (ep->socket >= 0) {
        close(ep->socket);
        ep->socket = -1;
    }
    if (ep->cm_established && ep->job.id && atomic_load(&ep->job.state) == JZ_CM_IDLE) {
        rdma_disconnect(ep->job.id);
        ep->cm_established = false;
    }
    if (ep->qp) {
        int rc = ibv_destroy_qp(ep->qp);
        if (rc) {
            struct jz_error error = {
                .code = -rc, .system_errno = rc, .stage = "connection-destroy-qp"};
            jz_peer_set_cleanup_error(ep->peer, &error);
            return;
        }
        ep->qp = NULL;
        if (ep->job.id)
            ep->job.id->qp = NULL; /* Avoid librdmacm's void cleanup hiding failure. */
    }
    if (ep->job.id) {
        int rc = jz_control_id_result(&ep->job);
        if (rc && rc != -EINPROGRESS) {
            struct jz_error error = {
                .code = rc, .system_errno = -rc, .stage = "connection-destroy-id"};
            jz_peer_set_cleanup_error(ep->peer, &error);
        }
        if (ep->job.id) {
            jz_control_schedule_id(control, &ep->job);
            return;
        }
    }
    jz_peer_set_closed(ep->peer, NULL);
    if (ep->retire_listener)
        jz_listener_retire_done(ep->retire_listener, ep->retire_ticket, NULL);
    jz_peer *peer = ep->peer;
    ep->peer = NULL;
    jz_peer_release(peer);
}

static size_t collect_polls(struct jz_rdma_control *control)
{
    size_t n = 0;
    control->polls[n] = (struct pollfd){.fd = control->wake_fd, .events = POLLIN};
    control->poll_owners[n++] = NULL;
    control->polls[n] =
        (struct pollfd){.fd = control->channel ? control->channel->fd : -1, .events = POLLIN};
    control->poll_owners[n++] = NULL;
    for (size_t i = 0; i < control->backend->max_listeners; ++i) {
        struct jz_listen_source *source = &control->sources[i];
        if (!source->handle)
            continue;
        int fd = source->channel ? source->channel->fd : source->socket;
        if (fd < 0)
            continue;
        control->polls[n] = (struct pollfd){.fd = fd, .events = POLLIN};
        control->poll_owners[n++] = source;
    }
    for (size_t i = 0; i < control->backend->max_connections; ++i) {
        struct jz_rdma_endpoint *ep = &control->endpoints[i];
        if (!ep->peer || ep->socket < 0)
            continue;
        short events = POLLIN;
        if (ep->phase == JZ_EP_TCP_CONNECT || ep->phase == JZ_EP_SEND_HELLO ||
            ep->phase == JZ_EP_SEND_OFFER || ep->phase == JZ_EP_SEND_READY ||
            ep->phase == JZ_EP_SEND_ACK)
            events |= POLLOUT;
        control->polls[n] = (struct pollfd){.fd = ep->socket, .events = events};
        control->poll_owners[n++] = ep;
    }
    return n;
}
static bool all_retired(struct jz_rdma_control *control)
{
    for (size_t i = 0; i < control->backend->max_connections; ++i)
        if (control->endpoints[i].peer)
            return false;
    for (size_t i = 0; i < control->backend->max_listeners; ++i)
        if (control->sources[i].handle)
            return false;
    return true;
}
static void *control_thread(void *arg)
{
    struct jz_rdma_control *control = arg;
    pthread_mutex_lock(&control->lock);
    for (;;) {
        size_t count = collect_polls(control);
        pthread_mutex_unlock(&control->lock);
        int rc = poll(control->polls, (nfds_t)count, 10);
        pthread_mutex_lock(&control->lock);
        if (rc > 0) {
            if (control->polls[0].revents) {
                uint64_t value;
                while (read(control->wake_fd, &value, sizeof(value)) > 0) {
                }
            }
            if (control->polls[1].revents & POLLIN)
                jz_control_handle_cm(control, control->channel);
            for (size_t i = 2; i < count; ++i) {
                if (!control->polls[i].revents)
                    continue;
                struct jz_cm_owner *owner = control->poll_owners[i];
                if (owner->kind == JZ_OWNER_SOURCE) {
                    struct jz_listen_source *source = (struct jz_listen_source *)owner;
                    if (source->channel)
                        jz_control_handle_cm(control, source->channel);
                    else
                        jz_control_accept_tcp(control, source);
                } else
                    jz_control_endpoint_progress(control, (struct jz_rdma_endpoint *)owner,
                                                 control->polls[i].revents);
            }
        }
        for (size_t i = 0; i < control->backend->max_listeners; ++i)
            if (control->sources[i].handle)
                jz_control_source_progress(control, &control->sources[i]);
        for (size_t i = 0; i < control->backend->max_connections; ++i) {
            struct jz_rdma_endpoint *ep = &control->endpoints[i];
            if (!ep->peer)
                continue;
            jz_control_endpoint_progress(control, ep, 0);
            if (ep->phase == JZ_EP_CLOSING)
                cleanup_endpoint(control, ep);
        }
        if (control->stopping && all_retired(control)) {
            control->stopped = true;
            pthread_cond_broadcast(&control->changed);
            break;
        }
    }
    pthread_mutex_unlock(&control->lock);
    return NULL;
}

int jz_control_ensure(struct jz_rdma_backend *backend, struct jz_error *error)
{
    if (backend->control)
        return 0;
    struct jz_rdma_control *control = calloc(1, sizeof(*control));
    if (!control)
        return jz_fail(error, ENOMEM, "control-allocate");
    control->backend = backend;
    control->wake_fd = -1;
    control->poll_capacity = backend->max_connections + backend->max_listeners + 2;
    control->endpoints = calloc(backend->max_connections, sizeof(*control->endpoints));
    control->sources = calloc(backend->max_listeners, sizeof(*control->sources));
    control->cqs = calloc(backend->device_count, sizeof(*control->cqs));
    control->polls = calloc(control->poll_capacity, sizeof(*control->polls));
    control->poll_owners = calloc(control->poll_capacity, sizeof(*control->poll_owners));
    int rc = ENOMEM;
    if (!control->endpoints || !control->sources || !control->cqs || !control->polls ||
        !control->poll_owners)
        goto failed_storage;
    rc = pthread_mutex_init(&control->lock, NULL);
    if (rc)
        goto failed_storage;
    pthread_condattr_t attr;
    rc = pthread_condattr_init(&attr);
    if (rc)
        goto failed_lock;
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (!rc)
        rc = pthread_cond_init(&control->changed, &attr);
    pthread_condattr_destroy(&attr);
    if (rc)
        goto failed_lock;
    rc = pthread_mutex_init(&control->reap_lock, NULL);
    if (rc)
        goto failed_condition;
    rc = pthread_cond_init(&control->reap_changed, NULL);
    if (rc)
        goto failed_reap_lock;
    if (getrandom(control->instance, sizeof(control->instance), GRND_NONBLOCK) !=
        (ssize_t)sizeof(control->instance)) {
        rc = errno ? errno : EIO;
        goto failed_reap_condition;
    }
    control->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (control->wake_fd < 0) {
        rc = errno;
        goto failed_reap_condition;
    }
    if (backend->setup_modes & JZ_RDMA_SETUP_CM) {
        control->channel = rdma_create_event_channel();
        if (!control->channel) {
            rc = errno;
            goto failed_fds;
        }
        if (fcntl(control->channel->fd, F_SETFL, O_NONBLOCK)) {
            rc = errno;
            goto failed_fds;
        }
    }
    rc = pthread_create(&control->reaper, NULL, reaper_thread, control);
    if (rc)
        goto failed_fds;
    rc = pthread_create(&control->thread, NULL, control_thread, control);
    if (rc) {
        pthread_mutex_lock(&control->reap_lock);
        control->reap_exit = true;
        pthread_cond_signal(&control->reap_changed);
        pthread_mutex_unlock(&control->reap_lock);
        pthread_join(control->reaper, NULL);
        goto failed_fds;
    }
    backend->control = control;
    return 0;
failed_fds:
    if (control->channel)
        rdma_destroy_event_channel(control->channel);
    if (control->wake_fd >= 0)
        close(control->wake_fd);
failed_reap_condition:
    pthread_cond_destroy(&control->reap_changed);
failed_reap_lock:
    pthread_mutex_destroy(&control->reap_lock);
failed_condition:
    pthread_cond_destroy(&control->changed);
failed_lock:
    pthread_mutex_destroy(&control->lock);
failed_storage:
    free(control->poll_owners);
    free(control->polls);
    free(control->cqs);
    free(control->sources);
    free(control->endpoints);
    free(control);
    return jz_fail(error, rc, "control-initialize");
}

int jz_rdma_control_shutdown(struct jz_rdma_backend *backend, struct jz_error *error)
{
    if (!backend->control_lock_live)
        return 0;
    pthread_mutex_lock(&backend->control_lock);
    struct jz_rdma_control *control = backend->control;
    if (!control) {
        pthread_mutex_unlock(&backend->control_lock);
        return 0;
    }
    if (!control->joined) {
        struct timespec deadline;
        int rc = jz_wait_deadline(5000, &deadline, error);
        if (rc) {
            pthread_mutex_unlock(&backend->control_lock);
            return rc;
        }
        pthread_mutex_lock(&control->lock);
        control->stopping = true;
        for (size_t i = 0; i < backend->max_listeners; ++i)
            if (control->sources[i].handle)
                jz_listener_request_close(control->sources[i].handle, NULL);
        for (size_t i = 0; i < backend->max_connections; ++i)
            if (control->endpoints[i].peer)
                jz_peer_request_close(control->endpoints[i].peer, NULL);
        jz_control_wake(control);
        while (!control->stopped) {
            rc = pthread_cond_timedwait(&control->changed, &control->lock, &deadline);
            if (rc && !control->stopped) {
                pthread_mutex_unlock(&control->lock);
                pthread_mutex_unlock(&backend->control_lock);
                return jz_fail(error, rc, "control-quiesce");
            }
        }
        pthread_mutex_unlock(&control->lock);
        pthread_join(control->thread, NULL);
        pthread_mutex_lock(&control->reap_lock);
        control->reap_exit = true;
        pthread_cond_signal(&control->reap_changed);
        pthread_mutex_unlock(&control->reap_lock);
        pthread_join(control->reaper, NULL);
        control->joined = true;
    }
    for (size_t i = 0; i < backend->device_count; ++i) {
        if (!control->cqs[i])
            continue;
        int rc = ibv_destroy_cq(control->cqs[i]);
        if (rc) {
            pthread_mutex_unlock(&backend->control_lock);
            return jz_fail(error, rc, "destroy-cq");
        }
        control->cqs[i] = NULL;
    }
    if (control->channel)
        rdma_destroy_event_channel(control->channel);
    close(control->wake_fd);
    pthread_cond_destroy(&control->reap_changed);
    pthread_mutex_destroy(&control->reap_lock);
    pthread_cond_destroy(&control->changed);
    pthread_mutex_destroy(&control->lock);
    free(control->poll_owners);
    free(control->polls);
    free(control->cqs);
    free(control->sources);
    free(control->endpoints);
    free(control);
    backend->control = NULL;
    pthread_mutex_unlock(&backend->control_lock);
    return 0;
}
