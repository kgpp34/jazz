#include "core/socket.h"
#include "check.h"
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    struct sockaddr_in local;
    CHECK(jz_address("localhost", 0, &local) == -EINVAL);
    CHECK(jz_address("0.0.0.0", 0, &local) == -EINVAL);
    CHECK(jz_address("224.0.0.1", 0, &local) == -EINVAL);
    CHECK(jz_address("127.0.0.1", 0, &local) == 0);
    uint16_t port = 0;
    int listener = jz_socket_listen(&local, &port);
    CHECK(listener >= 0 && port);
    CHECK(jz_socket_accept(listener, jz_now_ms() + 30) == -ETIMEDOUT);
    struct sockaddr_in peer = local;
    peer.sin_port = htons(port);
    int client = jz_socket_connect(&local, &peer, jz_now_ms() + 500);
    CHECK(client >= 0);
    int server = jz_socket_accept(listener, jz_now_ms() + 500);
    CHECK(server >= 0);
    close(listener);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        close(client);
        for (uint8_t i = 0; i < 96; ++i) {
            if (jz_socket_io(server, &i, 1, 1, jz_now_ms() + 1000)) _exit(1);
        }
        close(server);
        _exit(0);
    }
    close(server);
    uint8_t frame[96];
    CHECK(jz_socket_io(client, frame, sizeof(frame), 0, jz_now_ms() + 1000) == 0);
    for (size_t i = 0; i < sizeof(frame); ++i) CHECK(frame[i] == i);
    CHECK(jz_socket_io(client, frame, 1, 0, jz_now_ms() + 1000) == -ECONNRESET);
    close(client);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    int pair[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
    CHECK(send(pair[0], "abc", 3, MSG_NOSIGNAL) == 3);
    close(pair[0]);
    CHECK(jz_socket_io(pair[1], frame, 4, 0, jz_now_ms() + 500) == -ECONNRESET);
    close(pair[1]);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
    int64_t start = jz_now_ms();
    CHECK(jz_socket_io(pair[0], frame, 1, 0, start + 40) == -ETIMEDOUT);
    CHECK(jz_now_ms() - start < 500);
    close(pair[1]);
    CHECK(signal(SIGPIPE, SIG_DFL) != SIG_ERR);
    CHECK(jz_socket_io(pair[0], frame, 1, 1, jz_now_ms() + 500) == -EPIPE);
    close(pair[0]);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair) == 0);
    uint8_t *large = calloc(1, 4U * 1024U * 1024U);
    CHECK(large);
    CHECK(jz_socket_io(pair[0], large, 4U * 1024U * 1024U, 1, jz_now_ms() + 40) == -ETIMEDOUT);
    free(large); close(pair[0]); close(pair[1]);
    return 0;
}
