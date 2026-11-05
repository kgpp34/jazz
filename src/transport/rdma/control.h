#ifndef JZ_RDMA_CONTROL_H
#define JZ_RDMA_CONTROL_H
#include "transport/rdma/internal.h"
#include "transport/rdma/connection_wire.h"
#include "control/listener_internal.h"
#include "control/peer_internal.h"
#include <rdma/rdma_cma.h>
#include <poll.h>
#include <sys/socket.h>

enum jz_cm_job_state { JZ_CM_IDLE, JZ_CM_QUEUED, JZ_CM_RUNNING, JZ_CM_DONE };
struct jz_cm_job {
    struct rdma_cm_id *id; /* Never accessed by control while the destroy job is running. */
    struct jz_cm_job *next;
    atomic_int state;
    int result; /* Published before DONE, negative errno on failure. */
};
enum jz_owner_kind { JZ_OWNER_SOURCE = 1, JZ_OWNER_ENDPOINT, JZ_OWNER_REJECT };
struct jz_cm_owner {
    enum jz_owner_kind kind;
};
struct jz_rejected_id {
    struct jz_cm_owner owner;
    struct jz_cm_job job;
};
struct jz_bind_config {
    enum jz_rdma_setup setup;
    size_t device;
    uint8_t port;
    uint32_t gid_index;
    bool gid_explicit; /* Enforce an explicitly requested CM source GID index. */
    union ibv_gid gid;
    enum ibv_mtu mtu;
    uint32_t timeout_ms;
    struct sockaddr_storage local;
};
struct jz_listen_source {
    struct jz_cm_owner owner;
    jz_listener *handle; /* Control pin survives public destroy. */
    struct jz_bind_config config;
    int socket;
    struct rdma_event_channel *channel; /* Listener-only channel; peers migrate away. */
    struct jz_cm_job job;
    struct jz_rejected_id *rejects; /* Bounded rejection retirement, including close reserve. */
    struct jz_rejected_id overflow_reject; /* One reserved event-reader retirement slot. */
    size_t reject_capacity;
    size_t reject_limit;
    struct jz_rdma_bound_address bound;
};
enum jz_setup_phase {
    JZ_EP_NEW,
    JZ_EP_TCP_CONNECT,
    JZ_EP_RECV_HELLO,
    JZ_EP_SEND_HELLO,
    JZ_EP_RECV_OFFER,
    JZ_EP_SEND_OFFER,
    JZ_EP_RECV_READY,
    JZ_EP_SEND_READY,
    JZ_EP_RECV_ACK,
    JZ_EP_SEND_ACK,
    JZ_EP_CM_ADDR,
    JZ_EP_CM_ROUTE,
    JZ_EP_CM_CONNECT,
    JZ_EP_ACTIVE,
    JZ_EP_CLOSING
};
struct jz_rdma_endpoint {
    struct jz_cm_owner owner;
    jz_peer *peer; /* Independent control ownership pin. */
    struct jz_bind_config config;
    struct sockaddr_storage remote;
    struct jz_listen_source *source; /* Setup lease only; cleared at publication/abort. */
    struct jz_listener_ticket setup_ticket;
    jz_listener *retire_listener; /* Only while owning an unclaimed retirement lease. */
    struct jz_listener_ticket retire_ticket;
    bool passive;
    bool setup_released;
    bool cm_established;
    enum jz_setup_phase phase;
    struct timespec deadline;
    int socket;
    struct jz_cm_job job;
    struct ibv_qp *qp;
    struct jz_connection_message local;
    struct jz_connection_message remote_msg;
    uint8_t tx[JZ_OOB_FRAME_BYTES], rx[JZ_OOB_FRAME_BYTES];
    size_t sent, received; /* Partial TCP I/O progress within one fixed frame. */
};
struct jz_rdma_control {
    struct jz_rdma_backend *backend;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t thread, reaper;
    bool stopping, stopped, joined;
    int wake_fd;
    struct rdma_event_channel *channel; /* Accepted/active CM IDs, independent of listeners. */
    struct ibv_cq **cqs;                /* Per-device execution-domain CQ ownership. */
    struct jz_rdma_endpoint *endpoints;
    struct jz_listen_source *sources;
    struct pollfd *polls;
    void **poll_owners;
    size_t poll_capacity;
    uint8_t instance[16];
    uint64_t next_identity;
    pthread_mutex_t reap_lock;
    pthread_cond_t reap_changed;
    struct jz_cm_job *reap_head, *reap_tail;
    bool reap_exit;
};

int jz_rdma_control_shutdown(struct jz_rdma_backend *, struct jz_error *);
int jz_control_ensure(struct jz_rdma_backend *, struct jz_error *);
void jz_control_wake(struct jz_rdma_control *);
void jz_control_schedule_id(struct jz_rdma_control *, struct jz_cm_job *);
int jz_control_id_result(struct jz_cm_job *);
int jz_control_prepare_qp(struct jz_rdma_control *, struct jz_rdma_endpoint *, bool,
                          struct jz_error *);
int jz_control_oob_connect_qp(struct jz_rdma_endpoint *, struct jz_error *);
int jz_control_cm_check(struct jz_rdma_endpoint *, struct jz_error *);
void jz_control_fail(struct jz_rdma_endpoint *, int, const char *);
int jz_control_ready(struct jz_rdma_endpoint *);
struct jz_rdma_endpoint *jz_control_allocate_endpoint(struct jz_rdma_control *);
void jz_control_handle_cm(struct jz_rdma_control *, struct rdma_event_channel *);
void jz_control_accept_tcp(struct jz_rdma_control *, struct jz_listen_source *);
void jz_control_endpoint_progress(struct jz_rdma_control *, struct jz_rdma_endpoint *, short);
void jz_control_source_progress(struct jz_rdma_control *, struct jz_listen_source *);
int jz_control_validate_bind(struct jz_rdma_backend *, const char *, enum jz_rdma_setup, size_t,
                             uint8_t, int, uint32_t, struct jz_bind_config *, struct jz_error *);
int jz_control_parse_address(const char *, uint16_t, struct sockaddr_storage *, struct jz_error *);
void jz_control_set_port(struct sockaddr_storage *, uint16_t);
uint16_t jz_control_get_port(const struct sockaddr_storage *);
socklen_t jz_control_address_size(const struct sockaddr_storage *);
int jz_control_sequence(struct jz_rdma_control *, uint64_t *, struct jz_error *);
int jz_control_begin_endpoint(struct jz_rdma_control *, struct jz_rdma_endpoint *,
                              struct jz_error *);
int jz_control_start_cm(struct jz_rdma_control *, struct jz_rdma_endpoint *, struct jz_error *);
#endif
