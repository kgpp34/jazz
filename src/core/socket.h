#ifndef JZ_SOCKET_H
#define JZ_SOCKET_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

int64_t jz_now_ms(void);
int jz_remaining(int64_t deadline);
int jz_wait_fd(int fd, short events, int64_t deadline);
int jz_nonblock(int fd);
int jz_address(const char *text, uint16_t port, struct sockaddr_in *out);
int jz_socket_listen(const struct sockaddr_in *local, uint16_t *port);
int jz_socket_accept(int listener, int64_t deadline);
int jz_socket_connect(const struct sockaddr_in *local,
                      const struct sockaddr_in *peer, int64_t deadline);
int jz_socket_io(int fd, void *data, size_t length, int sending, int64_t deadline);

#endif
