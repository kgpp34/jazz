#include "jz/rdma.h"
#include "transport/rdma/control.h"
#include "check.h"
#include "common/wait.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int __real_ibv_destroy_qp(struct ibv_qp *);
int __wrap_ibv_destroy_qp(struct ibv_qp *);
int __real_rdma_destroy_id(struct rdma_cm_id *);
int __wrap_rdma_destroy_id(struct rdma_cm_id *);
int __real_rdma_connect(struct rdma_cm_id *, struct rdma_conn_param *);
int __wrap_rdma_connect(struct rdma_cm_id *, struct rdma_conn_param *);
static atomic_bool fail_qp, fail_id;
static atomic_bool corrupt_hello;
static atomic_uint qp_failures, id_failures;
int __wrap_ibv_destroy_qp(struct ibv_qp *qp)
{
    if (atomic_exchange(&fail_qp, false)) {
        atomic_fetch_add(&qp_failures, 1);
        return EBUSY;
    }
    return __real_ibv_destroy_qp(qp);
}
int __wrap_rdma_destroy_id(struct rdma_cm_id *id)
{
    if (atomic_exchange(&fail_id, false)) {
        atomic_fetch_add(&id_failures, 1);
        errno = EBUSY;
        return -1;
    }
    return __real_rdma_destroy_id(id);
}
int __wrap_rdma_connect(struct rdma_cm_id *id, struct rdma_conn_param *param)
{
    if (!atomic_exchange(&corrupt_hello, false))
        return __real_rdma_connect(id, param);
    uint8_t bytes[JZ_CM_HELLO_BYTES];
    CHECK(param && param->private_data_len == sizeof(bytes));
    memcpy(bytes, param->private_data, sizeof(bytes));
    bytes[4] = 99;
    struct rdma_conn_param invalid = *param;
    invalid.private_data = bytes;
    return __real_rdma_connect(id, &invalid);
}
static const char *ip, *device;
static void require(int rc, const struct jz_error *error)
{
    if (rc)
        fprintf(stderr, "connection error: rc=%d stage=%s cause=%d\n", rc,
                error && error->stage ? error->stage : "unknown", error ? error->system_errno : 0);
    CHECK(rc == 0);
}
static jz_engine *prepare(unsigned modes)
{
    const char *devices[] = {device};
    struct jz_rdma_config config;
    jz_rdma_config_init(&config);
    config.devices = devices;
    config.device_count = 1;
    config.setup_modes = modes;
    config.max_connections = 8;
    config.max_listeners = 4;
    jz_engine *engine = NULL;
    struct jz_error error;
    require(jz_engine_create(&engine, &error), &error);
    require(jz_rdma_configure(engine, &config, &error), &error);
    require(jz_engine_prepare(engine, &error), &error);
    return engine;
}
static jz_listener *listen_on(jz_engine *engine, enum jz_rdma_setup mode, uint32_t timeout,
                              struct jz_rdma_bound_address *address)
{
    struct jz_rdma_listen_options options;
    jz_rdma_listen_options_init(&options);
    options.local_ip = ip;
    options.setup = mode;
    options.timeout_ms = timeout;
    options.max_pending = 2;
    options.capacity = 4;
    jz_listener *listener = NULL;
    struct jz_error error;
    require(jz_rdma_listen(engine, &options, &listener, &error), &error);
    require(jz_rdma_listener_address(engine, listener, address, &error), &error);
    CHECK(address->service_port && !strcmp(address->ip, ip) && address->setup == mode);
    return listener;
}
static jz_peer *connect_to(jz_engine *engine, enum jz_rdma_setup mode, uint16_t port,
                           uint32_t timeout)
{
    struct jz_rdma_connect_options options;
    jz_rdma_connect_options_init(&options);
    options.local_ip = ip;
    options.remote_ip = ip;
    options.setup = mode;
    options.service_port = port;
    options.timeout_ms = timeout;
    jz_peer *peer = NULL;
    struct jz_error error;
    require(jz_rdma_connect(engine, &options, &peer, &error), &error);
    return peer;
}
static void close_peer(jz_peer **peer)
{
    struct jz_error error;
    require(jz_peer_request_close(*peer, &error), &error);
    require(jz_peer_wait_closed(*peer, 5000, &error), &error);
    require(jz_peer_destroy(peer, &error), &error);
}
static void close_listener(jz_listener **listener)
{
    struct jz_error error;
    require(jz_listener_request_close(*listener, &error), &error);
    require(jz_listener_wait_closed(*listener, 5000, &error), &error);
    require(jz_listener_destroy(listener, &error), &error);
}
static void write_all(int fd, const void *buffer, size_t size)
{
    const uint8_t *p = buffer;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
        CHECK(n > 0);
        p += n;
        size -= (size_t)n;
    }
}
static void read_all(int fd, void *buffer, size_t size)
{
    uint8_t *p = buffer;
    while (size) {
        ssize_t n = read(fd, p, size);
        if (n < 0 && errno == EINTR)
            continue;
        CHECK(n > 0);
        p += n;
        size -= (size_t)n;
    }
}
struct native_endpoint {
    struct jz_rdma_control *control;
    struct ibv_qp *qp;
    struct ibv_cq *cq;
    struct ibv_pd *pd;
};
struct probe_ready {
    struct jz_rdma_connection_info connection;
    uint64_t address;
    uint32_t rkey;
};
static struct native_endpoint native_endpoint(jz_engine *engine, jz_peer *peer)
{
    /* Test-only access: no application close occurs until both test WRs have
     * completed. There is no payload worker polling this CQ in this stage. */
    struct jz_rdma_control *control = jz_rdma_backend_find(engine)->control;
    struct native_endpoint result = {.control = control};
    pthread_mutex_lock(&control->lock);
    for (size_t i = 0; i < control->backend->max_connections; ++i) {
        struct jz_rdma_endpoint *ep = &control->endpoints[i];
        if (ep->peer == peer && ep->phase == JZ_EP_ACTIVE) {
            result.qp = ep->qp;
            result.cq = control->cqs[ep->config.device];
            result.pd = control->backend->devices[ep->config.device].pd;
            break;
        }
    }
    pthread_mutex_unlock(&control->lock);
    CHECK(result.qp && result.cq && result.pd);
    return result;
}
static void completion(struct ibv_cq *cq, uint64_t id)
{
    struct timespec deadline;
    CHECK(jz_wait_deadline(5000, &deadline, NULL) == 0);
    for (;;) {
        struct ibv_wc wc;
        int rc = ibv_poll_cq(cq, 1, &wc);
        CHECK(rc >= 0);
        if (rc) {
            CHECK(wc.status == IBV_WC_SUCCESS && wc.wr_id == id);
            return;
        }
        struct timespec now;
        CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        CHECK(now.tv_sec < deadline.tv_sec ||
              (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec));
        const struct timespec pause = {.tv_nsec = 100000};
        nanosleep(&pause, NULL);
    }
}
static void server(enum jz_rdma_setup mode, int send_fd, int receive_fd)
{
    jz_engine *engine = prepare((unsigned)mode);
    struct jz_rdma_bound_address address;
    jz_listener *listener = listen_on(engine, mode, 3000, &address);
    write_all(send_fd, &address.service_port, sizeof(address.service_port));
    jz_peer *peer = NULL;
    struct jz_error error;
    require(jz_listener_next_peer(listener, &peer, 5000, &error), &error);
    close_listener(&listener); /* A delivered CM peer must retain event progress. */
    struct jz_rdma_connection_info info;
    require(jz_rdma_peer_query(engine, peer, &info, &error), &error);
    struct native_endpoint native = native_endpoint(engine, peer);
    uint8_t payload[64] = {0};
    struct ibv_mr *mr =
        (ibv_reg_mr)(native.pd, payload, sizeof(payload),
                     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    CHECK(mr);
    struct ibv_sge sge = {.addr = (uintptr_t)(payload + 32), .length = 32, .lkey = mr->lkey};
    struct ibv_recv_wr wr = {.wr_id = 42, .sg_list = &sge, .num_sge = 1}, *bad = NULL;
    pthread_mutex_lock(&native.control->lock);
    CHECK(ibv_post_recv(native.qp, &wr, &bad) == 0);
    pthread_mutex_unlock(&native.control->lock);
    struct probe_ready ready = {
        .connection = info, .address = (uintptr_t)payload, .rkey = mr->rkey};
    write_all(send_fd, &ready, sizeof(ready)); /* Test IPC only, not a public descriptor API. */
    completion(native.cq, 42);
    for (size_t i = 0; i < sizeof(payload); ++i)
        CHECK(payload[i] == (uint8_t)(i % 32 + 11));
    CHECK(ibv_dereg_mr(mr) == 0);
    uint8_t done = 1;
    write_all(send_fd, &done, 1);
    read_all(receive_fd, &done, 1);
    require(jz_peer_wait_closed(peer, 5000, &error), &error);
    require(jz_engine_destroy(&engine, &error), &error);
    CHECK(jz_peer_wait_closed(peer, 0, NULL) == 0);
    CHECK(jz_peer_destroy(&peer, NULL) == 0);
    close(send_fd);
    close(receive_fd);
}
static void pair(const char *self, enum jz_rdma_setup mode)
{
    int to_parent[2], to_child[2];
    CHECK(pipe(to_parent) == 0 && pipe(to_child) == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        close(to_parent[0]);
        close(to_child[1]);
        char mode_arg[16], send_arg[16], recv_arg[16];
        snprintf(mode_arg, sizeof(mode_arg), "%u", (unsigned)mode);
        snprintf(send_arg, sizeof(send_arg), "%d", to_parent[1]);
        snprintf(recv_arg, sizeof(recv_arg), "%d", to_child[0]);
        execl(self, self, "--server", mode_arg, send_arg, recv_arg, (char *)NULL);
        _exit(127);
    }
    close(to_parent[1]);
    close(to_child[0]);
    jz_engine *engine = prepare((unsigned)mode);
    uint16_t port;
    read_all(to_parent[0], &port, sizeof(port));
    if (mode == JZ_RDMA_SETUP_CM) {
        atomic_store(&corrupt_hello, true);
        jz_peer *invalid = connect_to(engine, mode, port, 3000);
        CHECK(jz_peer_wait_ready(invalid, 5000, NULL) == -ECONNREFUSED);
        close_peer(&invalid); /* Invalid protocol must not poison the listener. */
    }
    jz_peer *peer = connect_to(engine, mode, port, 3000);
    struct jz_error error;
    require(jz_peer_wait_ready(peer, 5000, &error), &error);
    struct jz_rdma_connection_info own;
    struct probe_ready ready;
    require(jz_rdma_peer_query(engine, peer, &own, &error), &error);
    read_all(to_parent[0], &ready, sizeof(ready));
    struct jz_rdma_connection_info remote = ready.connection;
    CHECK(own.local_qpn == remote.remote_qpn && own.remote_qpn == remote.local_qpn);
    CHECK(own.attempt == remote.attempt && own.session == remote.session && own.mtu == remote.mtu);
    CHECK(!memcmp(own.remote_instance, remote.local_instance, 16));
    CHECK(!memcmp(own.local_instance, remote.remote_instance, 16));
    struct native_endpoint native = native_endpoint(engine, peer);
    uint8_t payload[64] = {0};
    for (size_t i = 0; i < 32; ++i)
        payload[i] = (uint8_t)(i + 11);
    struct ibv_mr *mr = (ibv_reg_mr)(native.pd, payload, sizeof(payload), IBV_ACCESS_LOCAL_WRITE);
    CHECK(mr);
    struct ibv_sge sge = {.addr = (uintptr_t)payload, .length = 32, .lkey = mr->lkey};
    struct ibv_send_wr rma = {.wr_id = 18,
                              .sg_list = &sge,
                              .num_sge = 1,
                              .opcode = IBV_WR_RDMA_WRITE,
                              .send_flags = IBV_SEND_SIGNALED,
                              .wr = {.rdma = {.remote_addr = ready.address, .rkey = ready.rkey}}},
                       *rma_bad = NULL;
    pthread_mutex_lock(&native.control->lock);
    CHECK(ibv_post_send(native.qp, &rma, &rma_bad) == 0);
    pthread_mutex_unlock(&native.control->lock);
    completion(native.cq, 18);
    rma.wr_id = 19;
    rma.opcode = IBV_WR_RDMA_READ;
    sge.addr = (uintptr_t)(payload + 32);
    pthread_mutex_lock(&native.control->lock);
    CHECK(ibv_post_send(native.qp, &rma, &rma_bad) == 0);
    pthread_mutex_unlock(&native.control->lock);
    completion(native.cq, 19);
    CHECK(!memcmp(payload, payload + 32, 32));
    sge.addr = (uintptr_t)payload;
    struct ibv_send_wr wr = {.wr_id = 17,
                             .sg_list = &sge,
                             .num_sge = 1,
                             .opcode = IBV_WR_SEND,
                             .send_flags = IBV_SEND_SIGNALED},
                       *bad = NULL;
    pthread_mutex_lock(&native.control->lock);
    CHECK(ibv_post_send(native.qp, &wr, &bad) == 0);
    pthread_mutex_unlock(&native.control->lock);
    completion(native.cq, 17);
    CHECK(ibv_dereg_mr(mr) == 0);
    uint8_t done;
    read_all(to_parent[0], &done, 1);
    atomic_store(&fail_qp, true);
    if (mode == JZ_RDMA_SETUP_CM)
        atomic_store(&fail_id, true);
    close_peer(&peer);
    CHECK(atomic_load(&qp_failures) >= 1);
    if (mode == JZ_RDMA_SETUP_CM)
        CHECK(atomic_load(&id_failures) >= 1);
    write_all(to_child[1], &done, 1);
    require(jz_engine_destroy(&engine, &error), &error);
    close(to_parent[0]);
    close(to_child[1]);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    printf("PASS: real %s two-process handshake, RC payload probe and cleanup retry\n",
           mode == JZ_RDMA_SETUP_CM ? "CM" : "OOB");
}
static void bidirectional(enum jz_rdma_setup mode)
{
    jz_engine *a = prepare(JZ_RDMA_SETUP_CM | JZ_RDMA_SETUP_OOB);
    jz_engine *b = prepare(JZ_RDMA_SETUP_CM | JZ_RDMA_SETUP_OOB);
    struct jz_rdma_bound_address aa, ba;
    jz_listener *al = listen_on(a, mode, 3000, &aa), *bl = listen_on(b, mode, 3000, &ba);
    jz_peer *ab = connect_to(a, mode, ba.service_port, 3000);
    jz_peer *ba_peer = connect_to(b, mode, aa.service_port, 3000);
    jz_peer *ap = NULL, *bp = NULL;
    struct jz_error error;
    require(jz_peer_wait_ready(ab, 5000, &error), &error);
    require(jz_peer_wait_ready(ba_peer, 5000, &error), &error);
    require(jz_listener_next_peer(al, &ap, 5000, &error), &error);
    require(jz_listener_next_peer(bl, &bp, 5000, &error), &error);
    require(jz_engine_destroy(&a, &error), &error);
    require(jz_engine_destroy(&b, &error), &error);
    CHECK(jz_listener_wait_closed(al, 0, NULL) == 0 && jz_listener_wait_closed(bl, 0, NULL) == 0);
    CHECK(jz_listener_destroy(&al, NULL) == 0 && jz_listener_destroy(&bl, NULL) == 0);
    close_peer(&ab);
    close_peer(&ba_peer);
    close_peer(&ap);
    close_peer(&bp);
}
static void errors_and_deadlines(void)
{
    jz_engine *engine = prepare(JZ_RDMA_SETUP_OOB);
    struct jz_rdma_bound_address address;
    jz_listener *listener = listen_on(engine, JZ_RDMA_SETUP_OOB, 100, &address);
    struct jz_rdma_listen_options options;
    jz_rdma_listen_options_init(&options);
    options.local_ip = ip;
    options.setup = JZ_RDMA_SETUP_OOB;
    options.service_port = address.service_port;
    options.max_pending = 2;
    options.capacity = 4;
    jz_listener *duplicate = NULL;
    CHECK(jz_rdma_listen(engine, &options, &duplicate, NULL) == -EADDRINUSE && !duplicate);
    int raw = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(raw >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(address.service_port)};
    CHECK(inet_pton(AF_INET, ip, &addr.sin_addr) == 1);
    CHECK(connect(raw, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    uint8_t partial[9] = {'J', 'Z', 'T', 'E', 1};
    CHECK(send(raw, partial, sizeof(partial), MSG_NOSIGNAL) == (ssize_t)sizeof(partial));
    jz_peer *none = NULL;
    CHECK(jz_listener_next_peer(listener, &none, 200, NULL) == -ETIMEDOUT && !none);
    close(raw);
    raw = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(raw >= 0 && connect(raw, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    uint8_t invalid[JZ_OOB_FRAME_BYTES];
    memset(invalid, 0xff, sizeof(invalid));
    CHECK(send(raw, invalid, sizeof(invalid), MSG_NOSIGNAL) == (ssize_t)sizeof(invalid));
    CHECK(jz_listener_next_peer(listener, &none, 200, NULL) == -ETIMEDOUT && !none);
    close(raw);
    close_listener(&listener);
    jz_peer *refused = connect_to(engine, JZ_RDMA_SETUP_OOB, address.service_port, 1000);
    CHECK(jz_peer_wait_ready(refused, 3000, NULL) == -ECONNREFUSED);
    close_peer(&refused);
    int stalled = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(stalled >= 0);
    addr.sin_port = 0;
    CHECK(bind(stalled, (struct sockaddr *)&addr, sizeof(addr)) == 0 && listen(stalled, 1) == 0);
    socklen_t size = sizeof(addr);
    CHECK(getsockname(stalled, (struct sockaddr *)&addr, &size) == 0);
    jz_peer *timeout = connect_to(engine, JZ_RDMA_SETUP_OOB, ntohs(addr.sin_port), 200);
    CHECK(jz_peer_wait_ready(timeout, 10, NULL) == -ETIMEDOUT);
    struct jz_peer_info info;
    CHECK(jz_peer_get_info(timeout, &info, NULL) == 0 && info.state == JZ_PEER_CONNECTING);
    CHECK(jz_peer_wait_ready(timeout, 3000, NULL) == -ETIMEDOUT);
    CHECK(jz_peer_get_info(timeout, &info, NULL) == 0);
    CHECK(info.terminal_error.stage && !strcmp(info.terminal_error.stage, "setup-deadline"));
    close_peer(&timeout);
    close(stalled);
    struct jz_rdma_connect_options bad;
    jz_rdma_connect_options_init(&bad);
    bad.local_ip = "192.0.2.123";
    bad.remote_ip = ip;
    bad.setup = JZ_RDMA_SETUP_OOB;
    bad.service_port = 1;
    CHECK(jz_rdma_connect(engine, &bad, &none, NULL) == -EADDRNOTAVAIL && !none);
    CHECK(jz_engine_destroy(&engine, NULL) == 0);
}
static void mixed_setup_and_unclaimed(void)
{
    jz_engine *a = prepare(JZ_RDMA_SETUP_CM | JZ_RDMA_SETUP_OOB);
    jz_engine *b = prepare(JZ_RDMA_SETUP_CM | JZ_RDMA_SETUP_OOB);
    enum jz_rdma_setup modes[] = {JZ_RDMA_SETUP_CM, JZ_RDMA_SETUP_OOB};
    jz_listener *listeners[2];
    jz_peer *active[2], *passive[2] = {0};
    struct jz_rdma_bound_address addresses[2];
    struct jz_error error;
    for (size_t i = 0; i < 2; ++i) {
        listeners[i] = listen_on(b, modes[i], 3000, &addresses[i]);
        active[i] = connect_to(a, modes[i], addresses[i].service_port, 3000);
    }
    for (size_t i = 0; i < 2; ++i) {
        require(jz_peer_wait_ready(active[i], 5000, &error), &error);
        require(jz_listener_next_peer(listeners[i], &passive[i], 5000, &error), &error);
    }
    CHECK(native_endpoint(a, active[0]).cq == native_endpoint(a, active[1]).cq);
    CHECK(native_endpoint(b, passive[0]).pd == native_endpoint(b, passive[1]).pd);
    close_listener(&listeners[0]);
    CHECK(jz_peer_wait_ready(passive[0], 0, NULL) == 0);
    /* A completed but unclaimed passive session is owned by its listener. */
    jz_peer *unclaimed = connect_to(a, JZ_RDMA_SETUP_OOB, addresses[1].service_port, 3000);
    require(jz_peer_wait_ready(unclaimed, 5000, &error), &error);
    close_listener(&listeners[1]);
    require(jz_peer_wait_closed(unclaimed, 5000, &error), &error);
    close_peer(&unclaimed);
    require(jz_engine_destroy(&a, &error), &error);
    require(jz_engine_destroy(&b, &error), &error);
    for (size_t i = 0; i < 2; ++i) {
        close_peer(&active[i]);
        close_peer(&passive[i]);
    }
}
int main(int argc, char **argv)
{
    ip = getenv("JZ_TEST_IP");
    device = getenv("JZ_TEST_DEVICE");
    if (!ip || !*ip) {
        puts("SKIP: set JZ_TEST_IP for real CM/OOB tests");
        return 77;
    }
    if (!device || !*device)
        device = "rxe0";
    if (argc == 5 && !strcmp(argv[1], "--server")) {
        server((enum jz_rdma_setup)atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
        return 0;
    }
    pair(argv[0], JZ_RDMA_SETUP_OOB);
    pair(argv[0], JZ_RDMA_SETUP_CM);
    bidirectional(JZ_RDMA_SETUP_OOB);
    bidirectional(JZ_RDMA_SETUP_CM);
    mixed_setup_and_unclaimed();
    errors_and_deadlines();
    puts("PASS: real binding, independent sessions, source shutdown and error/deadline paths");
    return 0;
}
