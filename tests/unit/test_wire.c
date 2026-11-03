#include "core/wire.h"
#include "check.h"
#include <errno.h>
#include <string.h>

int main(void)
{
    uint8_t frame[JZ_FRAME_SIZE], copy[JZ_FRAME_SIZE];
    uint8_t data[JZ_MESSAGE_MAX];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)i;
    for (size_t n = 0; n <= sizeof(data); ++n) {
        CHECK(jz_frame_encode(frame, JZ_MESSAGE, data, n) == 0);
        const uint8_t *payload;
        size_t length;
        CHECK(jz_frame_decode(frame, sizeof(frame), JZ_MESSAGE, &payload, &length) == 0);
        CHECK(length == n && memcmp(payload, data, n) == 0);
        for (size_t cut = 0; cut < sizeof(frame); ++cut)
            CHECK(jz_frame_decode(frame, cut, JZ_MESSAGE, &payload, &length) == -EPROTO);
        CHECK(jz_frame_decode(frame, sizeof(frame), JZ_READY, &payload, &length) == -EPROTO);
        for (size_t i = 0; i < 16; ++i) {
            memcpy(copy, frame, sizeof(copy));
            copy[i] ^= 0x80;
            CHECK(jz_frame_decode(copy, sizeof(copy), JZ_MESSAGE, &payload, &length) == -EPROTO);
        }
        if (n < JZ_MESSAGE_MAX) {
            memcpy(copy, frame, sizeof(copy)); copy[16 + n] = 1;
            CHECK(jz_frame_decode(copy, sizeof(copy), JZ_MESSAGE, &payload, &length) == -EPROTO);
        }
    }
    CHECK(jz_frame_encode(frame, JZ_MESSAGE, data, sizeof(data) + 1) == -EINVAL);
    struct jz_hello h = { .gid = {1}, .qpn = 0x123456, .psn = 0xfedcba,
                          .mtu = 3, .read_depth = 1, .session = UINT64_C(0x0102030405060708) };
    struct jz_hello decoded;
    uint8_t hello[JZ_HELLO_SIZE];
    CHECK(jz_hello_encode(&h, hello) == 0);
    CHECK(hello[16] == 0 && hello[17] == 0x12 && hello[32] == 1 && hello[39] == 8);
    CHECK(jz_hello_decode(hello, sizeof(hello), &decoded) == 0);
    CHECK(decoded.qpn == h.qpn && decoded.psn == h.psn && decoded.session == h.session);
    jz_put32(hello + 16, 0x1000000);
    CHECK(jz_hello_decode(hello, sizeof(hello), &decoded) == -EPROTO);
    CHECK(jz_range_valid(UINT64_MAX, UINT64_MAX - 1, 1));
    CHECK(!jz_range_valid(16, UINT64_MAX, 2));
    CHECK(!jz_range_valid(16, 1, UINT64_MAX));
    struct jz_remote_region region = { .address = 0x10203040, .length = 4096,
        .rkey = 0xdeadbeef, .access = JZ_REMOTE_READ | JZ_REMOTE_WRITE, .session = 42 };
    struct jz_remote_region region2;
    uint8_t descriptor[JZ_DESCRIPTOR_SIZE];
    CHECK(jz_descriptor_encode(&region, descriptor) == 0);
    CHECK(jz_descriptor_decode(descriptor, sizeof(descriptor), &region2) == 0);
    CHECK(region2.address == region.address && region2.rkey == region.rkey && region2.session == 42);
    jz_put64(descriptor, UINT64_MAX);
    CHECK(jz_descriptor_decode(descriptor, sizeof(descriptor), &region2) == -EPROTO);
    region.address = 1; region.access = 128;
    CHECK(jz_descriptor_encode(&region, descriptor) == -EINVAL);
    return 0;
}
