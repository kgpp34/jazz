#include "jazz/jazz.h"
#include "check.h"
#include <errno.h>

int main(void)
{
    struct jazz_config cfg;
    jazz_config_init(&cfg);
    jazz_listener *listener = NULL;
    jazz_connection *connection = NULL;
    struct jazz_error error;
    CHECK(jazz_connect(&cfg, &connection, &error) == -EINVAL && connection == NULL);
    CHECK(error.code == -EINVAL && error.stage);
    cfg.mode = JAZZ_OOB;
    cfg.local_address = "127.0.0.1";
    cfg.timeout_ms = 40;
    CHECK(jazz_listen(&cfg, &listener, &error) == 0);
    CHECK(jazz_listener_port(listener));
    CHECK(jazz_accept(listener, &connection, &error) == -ETIMEDOUT);
    CHECK(connection == NULL);
    CHECK(jazz_listener_close(&listener, &error) == 0 && listener == NULL);
    CHECK(jazz_listener_close(&listener, &error) == 0);
    cfg.peer_address = "127.0.0.1";
    cfg.port = 1;
    cfg.device = "__jazz_nonexistent_device__";
    int missing = jazz_connect(&cfg, &connection, &error);
    CHECK((missing == -ENODEV || missing == -ENOSYS) && connection == NULL);
    cfg.mode = (enum jazz_mode)99;
    CHECK(jazz_listen(&cfg, &listener, &error) == -EINVAL);
    cfg.mode = JAZZ_CM; cfg.gid_index = 0;
    CHECK(jazz_listen(&cfg, &listener, &error) == -EINVAL);
    cfg.mode = JAZZ_OOB; cfg.gid_index = -1; cfg.timeout_ms = 0;
    CHECK(jazz_listen(&cfg, &listener, &error) == -EINVAL);
    CHECK(jazz_connection_close(&connection, &error) == 0);
    CHECK(jazz_connection_quiesce(NULL, &error) == -EINVAL);
    CHECK(jazz_write(NULL, NULL, 0, NULL, 0, 1, &error) == -EINVAL);
    CHECK(jazz_region_unregister(NULL, &error) == -EINVAL);
    return 0;
}
