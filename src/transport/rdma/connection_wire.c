#include "transport/rdma/connection_wire.h"
#include <errno.h>
#include <string.h>

/* Explicit byte encoding avoids struct padding, unaligned loads and host order. */
static void put(uint8_t *out, uint64_t value, size_t bytes)
{
    for (size_t i = bytes; i > 0; --i) {
        out[i - 1] = (uint8_t)value;
        value >>= 8;
    }
}
static uint64_t get(const uint8_t *in, size_t bytes)
{
    uint64_t result = 0;
    for (size_t i = 0; i < bytes; ++i)
        result = (result << 8) | in[i];
    return result;
}
void jz_connection_encode(uint8_t *out, const struct jz_connection_message *msg, bool cm)
{
    memset(out, 0, cm ? JZ_CM_HELLO_BYTES : JZ_OOB_FRAME_BYTES);
    memcpy(out, "JZTE", 4);
    out[4] = 1;
    out[5] = (uint8_t)msg->type;
    out[6] = (uint8_t)msg->setup;
    memcpy(out + 8, msg->instance, 16);
    put(out + 24, msg->attempt, 8);
    put(out + 32, msg->session, 8);
    if (cm)
        return;
    put(out + 40, msg->qpn, 4);
    put(out + 44, msg->psn, 4);
    out[48] = (uint8_t)msg->mtu;
    out[49] = msg->reads;
    memcpy(out + 52, msg->gid.raw, 16);
}
int jz_connection_decode(struct jz_connection_message *out, const uint8_t *in, size_t size, bool cm)
{
    size_t expected = cm ? JZ_CM_HELLO_BYTES : JZ_OOB_FRAME_BYTES;
    if (!out || !in || size < expected || (!cm && size != expected))
        return -EPROTO;
    if (memcmp(in, "JZTE", 4) || in[4] != 1 || in[7])
        return -EPROTO;
    if (in[5] < JZ_WIRE_HELLO || in[5] > JZ_WIRE_ACK ||
        in[6] != (cm ? JZ_RDMA_SETUP_CM : JZ_RDMA_SETUP_OOB))
        return -EPROTO;
    uint8_t any = 0;
    for (size_t i = 8; i < 24; ++i)
        any |= in[i];
    if (!any || !get(in + 24, 8))
        return -EPROTO;
    struct jz_connection_message msg = {.type = (enum jz_wire_type)in[5],
                                        .setup = (enum jz_rdma_setup)in[6],
                                        .attempt = get(in + 24, 8),
                                        .session = get(in + 32, 8)};
    memcpy(msg.instance, in + 8, 16);
    if (msg.type == JZ_WIRE_HELLO ? msg.session != 0 : msg.session == 0)
        return -EPROTO;
    if (!cm) {
        msg.qpn = (uint32_t)get(in + 40, 4);
        msg.psn = (uint32_t)get(in + 44, 4);
        msg.mtu = (enum ibv_mtu)in[48];
        msg.reads = in[49];
        memcpy(msg.gid.raw, in + 52, 16);
        if (!msg.qpn || msg.qpn > 0xffffff || msg.psn > 0xffffff || !msg.reads ||
            msg.mtu < IBV_MTU_256 || msg.mtu > IBV_MTU_4096 || in[50] || in[51])
            return -EPROTO;
        for (size_t i = 68; i < expected; ++i)
            if (in[i])
                return -EPROTO;
        any = 0;
        for (size_t i = 0; i < 16; ++i)
            any |= msg.gid.raw[i];
        if (!any)
            return -EPROTO;
    }
    *out = msg;
    return 0;
}
