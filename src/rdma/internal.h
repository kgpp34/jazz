#ifndef JZ_INTERNAL_H
#define JZ_INTERNAL_H

#include "jz/prototype.h"
#include "core/socket.h"
#include "core/wire.h"
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <stdbool.h>

/* Internal constraints, not public ABI. Fixed wr_id values work only because a
 * connection executes one send/data operation at a time. Concurrent batches will
 * require tokens identifying individual requests. */
enum { JZ_MAX_REGIONS = 64, JZ_WR_SEND = 1, JZ_WR_RECV = 2, JZ_WR_DATA = 3 };
/* Validated value copy: addresses are converted and the device name is copied. */
struct jz_options {
    enum jz_mode mode;
    struct sockaddr_in local;
    struct sockaddr_in peer;
    char device[64];
    uint8_t ib_port;
    int gid_index;
    uint32_t timeout_ms;
};

/* Owns registration metadata and the MR, not the caller payload.
 * The connection must outlive its regions, so close returns BUSY while they remain. */
struct jz_region {
    struct jz_connection *connection;
    struct ibv_mr *mr;
    size_t length;
    unsigned access;
    bool busy; /* A posted local data WR may still access the buffer; this is not a lock. */
    struct jz_region *next; /* Entry in the connection-owned bounded registration list. */
};

/* CM/OOB produce the same connection type, sharing transfer.c data operations.
 * A single caller thread owns this object; these flags are not atomic.
 * NULL fields indicate resources not yet created or already released. */
struct jz_connection {
    struct jz_options options;
    struct ibv_context *context;
    bool own_context; /* OOB opens/closes its context; CM owns the context lifetime. */
    struct rdma_event_channel *channel;
    struct rdma_cm_id *id;
    bool established; /* Whether CM disconnect is needed; not a general connection state machine. */
    /* The prototype has one PD and two CQs per connection, preventing MR reuse
 * across peers. send_cq receives SEND/READ/WRITE completions; recv_cq receives
 * control RECV completions. Shared resources require new lifetime and CQ accounting. */
    struct ibv_pd *pd;
    struct ibv_cq *send_cq;
    struct ibv_cq *recv_cq;
    struct ibv_qp *qp;
    /* These MRs cover embedded control buffers. Destroy the QP before deregistering
 * MRs, and deregister before freeing the connection, to preserve DMA targets. */
    struct ibv_mr *send_mr;
    struct ibv_mr *recv_mr;
    uint8_t send_buffer[JZ_FRAME_SIZE];
    uint8_t recv_buffer[JZ_FRAME_SIZE];
    bool ready; /* Set after handshake and actual QP/GID validation; cleared by quiesce. */
    struct jz_region *regions;
    unsigned region_count;
    uint64_t session;      /* Random connection identifier included in exported descriptors. */
    uint64_t peer_session; /* Peer identifier from handshake, checked against remote descriptors before post. */
    uint32_t max_message;  /* Device limit for one message; the prototype does not slice requests. */
    uint8_t ib_port;
    int gid_index;
    struct ibv_port_attr port_attr;
    struct ibv_gid_entry gid_entry;
};

/* The listener does not own accepted connections. OOB uses a listening TCP fd;
 * CM uses channel/id. Accepted CM IDs migrate to their own event channel so
 * listener lifetime does not constrain accepted data connections. */
struct jz_listener {
    struct jz_options options;
    int fd;
    struct rdma_event_channel *channel;
    struct rdma_cm_id *id;
    uint16_t port;
};

int jz_fail(struct jz_error *error, int code, const char *stage, int sys);
void jz_clear_error(struct jz_error *error);
int jz_options(const struct jz_config *config, bool client,
               struct jz_options *out, struct jz_error *error);
int jz_new_connection(const struct jz_options *options, jz_connection **out,
                      struct jz_error *error);
int jz_resources(jz_connection *c, bool cm, struct jz_error *error);
int jz_validate_port(jz_connection *c, struct jz_error *error);
int jz_post_receive(jz_connection *c, struct jz_error *error);
int jz_wait_send(jz_connection *c, uint64_t id, enum ibv_wc_opcode opcode,
                 int64_t deadline, struct jz_error *error);
int jz_abort(jz_connection *c, int result);
int jz_cm_listen(jz_listener *l, struct jz_error *error);
int jz_cm_connect(jz_connection *c, int64_t deadline, struct jz_error *error);
int jz_cm_accept(jz_listener *l, jz_connection *c, int64_t deadline,
                 struct jz_error *error);
int jz_oob_connect(jz_connection *c, int64_t deadline, struct jz_error *error);
int jz_oob_accept(jz_listener *l, jz_connection *c, int64_t deadline,
                  struct jz_error *error);

#endif
