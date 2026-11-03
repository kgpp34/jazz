#ifndef JAZZ_INTERNAL_H
#define JAZZ_INTERNAL_H

#include "jazz/jazz.h"
#include "core/socket.h"
#include "core/wire.h"
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <stdbool.h>

enum { JZ_MAX_REGIONS = 64, JZ_WR_SEND = 1, JZ_WR_RECV = 2, JZ_WR_DATA = 3 };
struct jz_options {
    enum jazz_mode mode;
    struct sockaddr_in local;
    struct sockaddr_in peer;
    char device[64];
    uint8_t ib_port;
    int gid_index;
    uint32_t timeout_ms;
};

struct jazz_region {
    struct jazz_connection *connection;
    struct ibv_mr *mr;
    size_t length;
    unsigned access;
    bool busy;
    struct jazz_region *next;
};

struct jazz_connection {
    struct jz_options options;
    struct ibv_context *context;
    bool own_context;
    struct rdma_event_channel *channel;
    struct rdma_cm_id *id;
    bool established;
    struct ibv_pd *pd;
    struct ibv_cq *send_cq;
    struct ibv_cq *recv_cq;
    struct ibv_qp *qp;
    struct ibv_mr *send_mr;
    struct ibv_mr *recv_mr;
    uint8_t send_buffer[JZ_FRAME_SIZE];
    uint8_t recv_buffer[JZ_FRAME_SIZE];
    bool ready;
    struct jazz_region *regions;
    unsigned region_count;
    uint64_t session;
    uint64_t peer_session;
    uint32_t max_message;
    uint8_t ib_port;
    int gid_index;
    struct ibv_port_attr port_attr;
    struct ibv_gid_entry gid_entry;
};

struct jazz_listener {
    struct jz_options options;
    int fd;
    struct rdma_event_channel *channel;
    struct rdma_cm_id *id;
    uint16_t port;
};

int jz_fail(struct jazz_error *error, int code, const char *stage, int sys);
void jz_clear_error(struct jazz_error *error);
int jz_options(const struct jazz_config *config, bool client,
               struct jz_options *out, struct jazz_error *error);
int jz_new_connection(const struct jz_options *options, jazz_connection **out,
                      struct jazz_error *error);
int jz_resources(jazz_connection *c, bool cm, struct jazz_error *error);
int jz_validate_port(jazz_connection *c, struct jazz_error *error);
int jz_post_receive(jazz_connection *c, struct jazz_error *error);
int jz_wait_send(jazz_connection *c, uint64_t id, enum ibv_wc_opcode opcode,
                 int64_t deadline, struct jazz_error *error);
int jz_abort(jazz_connection *c, int result);
int jz_cm_listen(jazz_listener *l, struct jazz_error *error);
int jz_cm_connect(jazz_connection *c, int64_t deadline, struct jazz_error *error);
int jz_cm_accept(jazz_listener *l, jazz_connection *c, int64_t deadline,
                 struct jazz_error *error);
int jz_oob_connect(jazz_connection *c, int64_t deadline, struct jazz_error *error);
int jz_oob_accept(jazz_listener *l, jazz_connection *c, int64_t deadline,
                  struct jazz_error *error);

#endif
