#ifndef JZ_CONNECTION_WIRE_H
#define JZ_CONNECTION_WIRE_H
#include "jz/rdma.h"
#include <infiniband/verbs.h>

enum { JZ_CM_HELLO_BYTES = 40, JZ_OOB_FRAME_BYTES = 80 };
enum jz_wire_type { JZ_WIRE_HELLO = 1, JZ_WIRE_OFFER, JZ_WIRE_READY, JZ_WIRE_ACK };
struct jz_connection_message {
    enum jz_wire_type type;
    enum jz_rdma_setup setup;
    uint8_t instance[16];
    uint64_t attempt;
    uint64_t session;
    uint32_t qpn;
    uint32_t psn;
    enum ibv_mtu mtu;
    uint8_t reads;
    union ibv_gid gid;
};
void jz_connection_encode(uint8_t *, const struct jz_connection_message *, bool cm);
int jz_connection_decode(struct jz_connection_message *, const uint8_t *, size_t, bool cm);
#endif
