#include "core/socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

int64_t jz_now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return -1;
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int jz_remaining(int64_t deadline)
{
    int64_t now = jz_now_ms();
    if (now < 0) return -EIO;
    if (now >= deadline) return 0;
    int64_t remaining = deadline - now;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

int jz_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -errno;
    return 0;
}

int jz_wait_fd(int fd, short events, int64_t deadline)
{
    for (;;) {
        int remaining = jz_remaining(deadline);
        if (remaining < 0) return remaining;
        if (!remaining) return -ETIMEDOUT;
        struct pollfd p = { .fd = fd, .events = events };
        int n = poll(&p, 1, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (!n) return -ETIMEDOUT;
        if (p.revents & POLLNVAL) return -EBADF;
        if (p.revents & (events | POLLERR | POLLHUP)) return 0;
    }
}

int jz_address(const char *text, uint16_t port, struct sockaddr_in *out)
{
    if (!text || !out) return -EINVAL;
    *out = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_pton(AF_INET, text, &out->sin_addr) != 1 ||
        out->sin_addr.s_addr == htonl(INADDR_ANY) ||
        IN_MULTICAST(ntohl(out->sin_addr.s_addr)) ||
        out->sin_addr.s_addr == htonl(INADDR_BROADCAST))
        return -EINVAL;
    return 0;
}

int jz_socket_listen(const struct sockaddr_in *local, uint16_t *port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
        bind(fd, (const struct sockaddr *)local, sizeof(*local)) || listen(fd, 16)) {
        int rc = -errno;
        close(fd);
        return rc;
    }
    struct sockaddr_in bound;
    socklen_t size = sizeof(bound);
    if (getsockname(fd, (struct sockaddr *)&bound, &size)) {
        int rc = -errno;
        close(fd);
        return rc;
    }
    *port = ntohs(bound.sin_port);
    return fd;
}

int jz_socket_accept(int listener, int64_t deadline)
{
    for (;;) {
        int rc = jz_wait_fd(listener, POLLIN, deadline);
        if (rc) return rc;
        int fd = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) return fd;
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
            return -errno;
    }
}

int jz_socket_connect(const struct sockaddr_in *local,
                      const struct sockaddr_in *peer, int64_t deadline)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
    struct sockaddr_in source = *local;
    source.sin_port = 0;
    int rc = 0;
    if (bind(fd, (const struct sockaddr *)&source, sizeof(source))) rc = -errno;
    if (!rc && connect(fd, (const struct sockaddr *)peer, sizeof(*peer))) {
        if (errno != EINPROGRESS) rc = -errno;
        else {
            rc = jz_wait_fd(fd, POLLOUT, deadline);
            if (!rc) {
                int error = 0;
                socklen_t size = sizeof(error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size)) rc = -errno;
                else if (error) rc = -error;
            }
        }
    }
    if (rc) { close(fd); return rc; }
    return fd;
}

int jz_socket_io(int fd, void *data, size_t length, int sending, int64_t deadline)
{
    size_t done = 0;
    while (done < length) {
        int remaining = jz_remaining(deadline);
        if (remaining < 0) return remaining;
        if (!remaining) return -ETIMEDOUT;
        ssize_t n = sending ? send(fd, (char *)data + done, length - done, MSG_NOSIGNAL)
                            : recv(fd, (char *)data + done, length - done, 0);
        if (n > 0) { done += (size_t)n; continue; }
        if (!n) return -ECONNRESET;
        if (errno == EINTR) continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) return -errno;
        int rc = jz_wait_fd(fd, sending ? POLLOUT : POLLIN, deadline);
        if (rc) return rc;
    }
    return 0;
}
