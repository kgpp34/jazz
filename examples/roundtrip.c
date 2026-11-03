#include "jazz/jazz.h"

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t pattern(size_t i) { return (uint8_t)((i * 131U + i / 251U + 17U) % 251U + 1U); }

static int number(const char *text, uint64_t max, uint64_t *out)
{
    if (!text || !*text || text[0] < '0' || text[0] > '9') return -1;
    char *end;
    errno = 0;
    unsigned long long n = strtoull(text, &end, 10);
    if (errno || *end || n > max) return -1;
    *out = (uint64_t)n;
    return 0;
}

static void report(int rc, const struct jazz_error *e)
{
    fprintf(stderr, "error=%d stage=%s system_errno=%d wc_status=%" PRIu32 "\n",
            rc, e->stage ? e->stage : "example-validation", e->system_errno, e->wc_status);
}

int main(int argc, char **argv)
{
    struct jazz_config cfg;
    jazz_config_init(&cfg);
    const char *role = NULL;
    size_t bytes = 4096;
    unsigned pause_ms = 0;
    static const struct option opts[] = {
        {"mode", required_argument, NULL, 'm'}, {"role", required_argument, NULL, 'r'},
        {"local", required_argument, NULL, 'l'}, {"peer", required_argument, NULL, 'p'},
        {"port", required_argument, NULL, 'P'}, {"device", required_argument, NULL, 'd'},
        {"bytes", required_argument, NULL, 'b'}, {"timeout-ms", required_argument, NULL, 't'},
        {"ib-port", required_argument, NULL, 'i'}, {"gid-index", required_argument, NULL, 'g'},
        {"pause-ms", required_argument, NULL, 's'}, {NULL, 0, NULL, 0}
    };
    int arg;
    while ((arg = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        uint64_t n = 0;
        switch (arg) {
        case 'm':
            if (!strcmp(optarg, "cm")) cfg.mode = JAZZ_CM;
            else if (!strcmp(optarg, "oob")) cfg.mode = JAZZ_OOB;
            else goto usage;
            break;
        case 'r': role = optarg; break;
        case 'l': cfg.local_address = optarg; break;
        case 'p': cfg.peer_address = optarg; break;
        case 'd': cfg.device = optarg; break;
        case 'P': if (number(optarg, UINT16_MAX, &n)) goto usage; cfg.port = (uint16_t)n; break;
        case 'b': if (number(optarg, 16U * 1024U * 1024U, &n) || !n) goto usage; bytes = (size_t)n; break;
        case 't': if (number(optarg, 60000, &n) || !n) goto usage; cfg.timeout_ms = (uint32_t)n; break;
        case 'i': if (number(optarg, 255, &n) || !n) goto usage; cfg.ib_port = (uint8_t)n; break;
        case 'g': if (number(optarg, 255, &n)) goto usage; cfg.gid_index = (int)n; break;
        case 's': if (number(optarg, 60000, &n)) goto usage; pause_ms = (unsigned)n; break;
        default: goto usage;
        }
    }
    if (optind != argc || !role || (strcmp(role, "server") && strcmp(role, "client"))) goto usage;
    int server = !strcmp(role, "server");
    jazz_listener *listener = NULL;
    jazz_connection *c = NULL;
    jazz_region *region = NULL;
    struct jazz_error error = {0};
    uint8_t *buffer = NULL;
    int rc;
    if (server) {
        rc = jazz_listen(&cfg, &listener, &error);
        if (rc) goto cleanup;
        printf("LISTENING %u\n", (unsigned)jazz_listener_port(listener));
        fflush(stdout);
        rc = jazz_accept(listener, &c, &error);
        if (!rc) rc = jazz_listener_close(&listener, &error);
    } else rc = jazz_connect(&cfg, &c, &error);
    if (rc) goto cleanup;
    buffer = malloc(bytes + 32);
    if (!buffer) { rc = -ENOMEM; goto cleanup; }
    memset(buffer, 0xa5, bytes + 32);
    if (!server) for (size_t i = 0; i < bytes; ++i) buffer[i + 16] = pattern(i);
    rc = jazz_region_register(c, buffer, bytes + 32,
        server ? JAZZ_REMOTE_READ | JAZZ_REMOTE_WRITE : 0, &region, &error);
    if (rc) goto cleanup;
    uint8_t message[JAZZ_MESSAGE_MAX];
    size_t size = 0;
    struct jazz_remote_region remote;
    if (server) {
        if (jazz_region_unregister(&region, NULL) != -EBUSY) { rc = -EPROTO; goto cleanup; }
        rc = jazz_region_export(region, &remote, &error);
        if (!rc) rc = jazz_descriptor_encode(&remote, message);
        if (!rc) rc = jazz_send_message(c, message, JAZZ_DESCRIPTOR_SIZE, &error);
        if (!rc && pause_ms) {
            struct timespec pause = { .tv_sec = pause_ms / 1000U,
                                      .tv_nsec = (long)(pause_ms % 1000U) * 1000000L };
            while (nanosleep(&pause, &pause) && errno == EINTR) {}
        }
        if (!rc) rc = jazz_receive_message(c, message, sizeof(message), &size, &error);
        if (!rc && (size != 1 || message[0] != 0x52)) rc = -EPROTO;
        for (size_t i = 0; !rc && i < bytes; ++i)
            if (buffer[i + 16] != pattern(i)) rc = -EBADMSG;
        for (size_t i = 0; !rc && i < 16; ++i)
            if (buffer[i] != 0xa5 || buffer[bytes + 16 + i] != 0xa5) rc = -EBADMSG;
        if (!rc) { message[0] = 0x41; rc = jazz_send_message(c, message, 1, &error); }
    } else {
        rc = jazz_receive_message(c, message, sizeof(message), &size, &error);
        if (!rc) rc = jazz_descriptor_decode(message, size, &remote);
        /* These failures must be rejected locally without poisoning the QP. */
        if (!rc) {
            struct jazz_remote_region stale = remote;
            stale.session ^= UINT64_C(0x100);
            if (jazz_write(c, region, 16, &stale, 16, bytes, NULL) != -ESTALE ||
                jazz_write(c, region, 16, &remote, UINT64_MAX, bytes, NULL) != -ERANGE)
                rc = -EPROTO;
        }
        if (!rc) rc = jazz_write(c, region, 16, &remote, 16, bytes, &error);
        if (!rc) {
            memset(buffer + 16, 0, bytes);
            rc = jazz_read(c, region, 16, &remote, 16, bytes, &error);
        }
        for (size_t i = 0; !rc && i < bytes; ++i)
            if (buffer[i + 16] != pattern(i)) rc = -EBADMSG;
        for (size_t i = 0; !rc && i < 16; ++i)
            if (buffer[i] != 0xa5 || buffer[bytes + 16 + i] != 0xa5) rc = -EBADMSG;
        if (!rc) { message[0] = 0x52; rc = jazz_send_message(c, message, 1, &error); }
        if (!rc) rc = jazz_receive_message(c, message, sizeof(message), &size, &error);
        if (!rc && (size != 1 || message[0] != 0x41)) rc = -EPROTO;
    }
cleanup:
    if (rc) report(rc, &error);
    /* Even a timeout must stop DMA before caller-owned storage is freed. */
    if (c && jazz_connection_quiesce(c, &error)) { report(-EIO, &error); return 1; }
    if (jazz_region_unregister(&region, &error)) { report(-EIO, &error); return 1; }
    free(buffer);
    if (jazz_connection_close(&c, &error)) { report(-EIO, &error); return 1; }
    if (jazz_listener_close(&listener, &error)) { report(-EIO, &error); return 1; }
    if (!rc) printf("PASS mode=%s role=%s bytes=%zu\n", cfg.mode == JAZZ_CM ? "cm" : "oob", role, bytes);
    return rc ? 1 : 0;
usage:
    fprintf(stderr, "Usage: %s --mode cm|oob --role server|client --local IPv4 "
            "[--peer IPv4] --port PORT [--device RNIC] [--bytes N] [--timeout-ms N]\n", argv[0]);
    return 2;
}
