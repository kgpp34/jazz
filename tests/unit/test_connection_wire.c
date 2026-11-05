#include "transport/rdma/connection_wire.h"
#include "check.h"
#include <errno.h>
#include <string.h>

int main(void)
{
    struct jz_connection_message original = {.type = JZ_WIRE_HELLO,
                                             .setup = JZ_RDMA_SETUP_OOB,
                                             .attempt = UINT64_C(0x123456789abcdef0),
                                             .qpn = 0x12345,
                                             .psn = 0xabcde,
                                             .mtu = IBV_MTU_1024,
                                             .reads = 1};
    original.instance[0] = 7;
    original.gid.raw[15] = 1;
    uint8_t bytes[JZ_OOB_FRAME_BYTES];
    jz_connection_encode(bytes, &original, false);
    struct jz_connection_message decoded;
    CHECK(jz_connection_decode(&decoded, bytes, sizeof(bytes), false) == 0);
    CHECK(decoded.attempt == original.attempt && decoded.qpn == original.qpn &&
          decoded.psn == original.psn && decoded.mtu == original.mtu);
    CHECK(bytes[24] == 0x12 && bytes[31] == 0xf0 && bytes[41] == 1);
    uint8_t saved[JZ_OOB_FRAME_BYTES];
    memcpy(saved, bytes, sizeof(saved));
    for (size_t size = 0; size < sizeof(bytes); ++size)
        CHECK(jz_connection_decode(&decoded, bytes, size, false) == -EPROTO);
    CHECK(jz_connection_decode(&decoded, bytes, sizeof(bytes) + 1, false) == -EPROTO);
    const size_t invalid_offsets[] = {0, 4, 5, 6, 7, 40, 44, 48, 50, 51, 68, 79};
    for (size_t n = 0; n < sizeof(invalid_offsets) / sizeof(invalid_offsets[0]); ++n) {
        memcpy(bytes, saved, sizeof(bytes));
        bytes[invalid_offsets[n]] = 0xff;
        decoded.attempt = 99;
        CHECK(jz_connection_decode(&decoded, bytes, sizeof(bytes), false) == -EPROTO);
        CHECK(decoded.attempt == 99);
    }
    memcpy(bytes, saved, sizeof(bytes));
    bytes[49] = 0;
    CHECK(jz_connection_decode(&decoded, bytes, sizeof(bytes), false) == -EPROTO);
    memcpy(bytes, saved, sizeof(bytes));
    memset(bytes + 8, 0, 16);
    CHECK(jz_connection_decode(&decoded, bytes, sizeof(bytes), false) == -EPROTO);
    memcpy(bytes, saved, sizeof(bytes));
    memset(bytes + 24, 0, 8);
    CHECK(jz_connection_decode(&decoded, bytes, sizeof(bytes), false) == -EPROTO);
    memcpy(bytes, saved, sizeof(bytes));
    memset(bytes + 52, 0, 16);
    CHECK(jz_connection_decode(&decoded, bytes, sizeof(bytes), false) == -EPROTO);
    for (unsigned type = JZ_WIRE_OFFER; type <= JZ_WIRE_ACK; ++type) {
        original.type = (enum jz_wire_type)type;
        original.session = 123;
        jz_connection_encode(bytes, &original, false);
        CHECK(jz_connection_decode(&decoded, bytes, sizeof(bytes), false) == 0);
        CHECK(decoded.session == 123 && decoded.type == original.type);
    }
    original.setup = JZ_RDMA_SETUP_CM;
    original.type = JZ_WIRE_HELLO;
    original.session = 0;
    jz_connection_encode(bytes, &original, true);
    CHECK(jz_connection_decode(&decoded, bytes, JZ_CM_HELLO_BYTES, true) == 0);
    CHECK(jz_connection_decode(&decoded, bytes, 56, true) == 0); /* Provider padding is allowed. */
    original.type = JZ_WIRE_OFFER;
    original.session = 321;
    jz_connection_encode(bytes, &original, true);
    CHECK(jz_connection_decode(&decoded, bytes, JZ_CM_HELLO_BYTES, true) == 0);
    CHECK(decoded.session == 321);
    CHECK(jz_connection_decode(NULL, bytes, sizeof(bytes), true) == -EPROTO);
    CHECK(jz_connection_decode(&decoded, NULL, sizeof(bytes), true) == -EPROTO);
    return 0;
}
