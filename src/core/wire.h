#ifndef JZ_WIRE_H
#define JZ_WIRE_H

#include <stddef.h>
#include <stdint.h>
#include "jz/prototype.h"

enum { JZ_FRAME_SIZE = 96, JZ_HELLO_SIZE = 40 };
enum jz_kind { JZ_HELLO = 1, JZ_READY = 2, JZ_MESSAGE = 3 };
struct jz_hello {
    uint8_t gid[16];
    uint32_t qpn;
    uint32_t psn;
    uint32_t mtu;
    uint32_t read_depth;
    uint64_t session;
};

void jz_put32(uint8_t *out, uint32_t value);
void jz_put64(uint8_t *out, uint64_t value);
uint32_t jz_get32(const uint8_t *in);
uint64_t jz_get64(const uint8_t *in);
int jz_frame_encode(uint8_t out[JZ_FRAME_SIZE], enum jz_kind kind,
                    const void *payload, size_t length);
int jz_frame_decode(const uint8_t *frame, size_t size, enum jz_kind kind,
                    const uint8_t **payload, size_t *length);
int jz_hello_encode(const struct jz_hello *hello, uint8_t out[JZ_HELLO_SIZE]);
int jz_hello_decode(const uint8_t *payload, size_t length, struct jz_hello *out);
int jz_range_valid(uint64_t size, uint64_t offset, uint64_t length);

#endif
