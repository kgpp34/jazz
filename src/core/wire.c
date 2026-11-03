#include "core/wire.h"

#include <errno.h>
#include <string.h>

void jz_put32(uint8_t *out, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        out[i] = (uint8_t)(value >> (24U - 8U * i));
}

void jz_put64(uint8_t *out, uint64_t value)
{
    jz_put32(out, (uint32_t)(value >> 32));
    jz_put32(out + 4, (uint32_t)value);
}

uint32_t jz_get32(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

uint64_t jz_get64(const uint8_t *in)
{
    return ((uint64_t)jz_get32(in) << 32) | jz_get32(in + 4);
}

int jz_range_valid(uint64_t size, uint64_t offset, uint64_t length)
{
    return offset <= size && length <= size - offset;
}

int jz_frame_encode(uint8_t out[JZ_FRAME_SIZE], enum jz_kind kind,
                    const void *payload, size_t length)
{
    if (!out || length > JAZZ_MESSAGE_MAX || (length && !payload) ||
        kind < JZ_HELLO || kind > JZ_MESSAGE)
        return -EINVAL;
    memset(out, 0, JZ_FRAME_SIZE);
    jz_put32(out, UINT32_C(0x4a415a5a));
    jz_put32(out + 4, 1);
    jz_put32(out + 8, (uint32_t)kind);
    jz_put32(out + 12, (uint32_t)length);
    if (length) memcpy(out + 16, payload, length);
    return 0;
}

int jz_frame_decode(const uint8_t *frame, size_t size, enum jz_kind kind,
                    const uint8_t **payload, size_t *length)
{
    if (!frame || !payload || !length) return -EINVAL;
    if (size != JZ_FRAME_SIZE || jz_get32(frame) != UINT32_C(0x4a415a5a) ||
        jz_get32(frame + 4) != 1 || jz_get32(frame + 8) != (uint32_t)kind)
        return -EPROTO;
    uint32_t n = jz_get32(frame + 12);
    if (n > JAZZ_MESSAGE_MAX) return -EPROTO;
    for (size_t i = 16 + (size_t)n; i < JZ_FRAME_SIZE; ++i)
        if (frame[i] != 0) return -EPROTO;
    *payload = frame + 16;
    *length = n;
    return 0;
}

static int hello_valid(const struct jz_hello *h)
{
    uint8_t any = 0;
    for (size_t i = 0; i < sizeof(h->gid); ++i) any |= h->gid[i];
    return any && h->qpn && h->qpn <= UINT32_C(0xffffff) &&
           h->psn <= UINT32_C(0xffffff) && h->mtu >= 1 && h->mtu <= 5 &&
           h->read_depth >= 1 && h->read_depth <= 255 && h->session;
}

int jz_hello_encode(const struct jz_hello *h, uint8_t out[JZ_HELLO_SIZE])
{
    if (!h || !out || !hello_valid(h)) return -EINVAL;
    memcpy(out, h->gid, 16);
    jz_put32(out + 16, h->qpn);
    jz_put32(out + 20, h->psn);
    jz_put32(out + 24, h->mtu);
    jz_put32(out + 28, h->read_depth);
    jz_put64(out + 32, h->session);
    return 0;
}

int jz_hello_decode(const uint8_t *payload, size_t length, struct jz_hello *out)
{
    if (!payload || !out) return -EINVAL;
    if (length != JZ_HELLO_SIZE) return -EPROTO;
    struct jz_hello h;
    memcpy(h.gid, payload, 16);
    h.qpn = jz_get32(payload + 16);
    h.psn = jz_get32(payload + 20);
    h.mtu = jz_get32(payload + 24);
    h.read_depth = jz_get32(payload + 28);
    h.session = jz_get64(payload + 32);
    if (!hello_valid(&h)) return -EPROTO;
    *out = h;
    return 0;
}

static int descriptor_valid(const struct jazz_remote_region *r)
{
    return r->length && r->address <= UINT64_MAX - r->length && r->session &&
           !(r->access & ~(uint32_t)(JAZZ_REMOTE_READ | JAZZ_REMOTE_WRITE));
}

int jazz_descriptor_encode(const struct jazz_remote_region *r,
                           uint8_t out[JAZZ_DESCRIPTOR_SIZE])
{
    if (!r || !out || !descriptor_valid(r)) return -EINVAL;
    jz_put64(out, r->address);
    jz_put64(out + 8, r->length);
    jz_put32(out + 16, r->rkey);
    jz_put32(out + 20, r->access);
    jz_put64(out + 24, r->session);
    return 0;
}

int jazz_descriptor_decode(const uint8_t *bytes, size_t length,
                           struct jazz_remote_region *out)
{
    if (!bytes || !out) return -EINVAL;
    if (length != JAZZ_DESCRIPTOR_SIZE) return -EPROTO;
    struct jazz_remote_region r = {
        .address = jz_get64(bytes), .length = jz_get64(bytes + 8),
        .rkey = jz_get32(bytes + 16), .access = jz_get32(bytes + 20),
        .session = jz_get64(bytes + 24)
    };
    if (!descriptor_valid(&r)) return -EPROTO;
    *out = r;
    return 0;
}
