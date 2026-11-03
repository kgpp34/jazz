#include "jz/prototype.h"
#include "check.h"
#include <errno.h>

int main(void)
{
    struct jz_config cfg;
    jz_config_init(&cfg);
    jz_listener *listener = NULL;
    jz_connection *connection = NULL;
    struct jz_error error;
    CHECK(jz_connect(&cfg, &connection, &error) == -EINVAL && connection == NULL);
    CHECK(error.code == -EINVAL && error.stage);
    cfg.mode = JZ_OOB;
    cfg.local_address = "127.0.0.1";
    cfg.timeout_ms = 40;
    CHECK(jz_listen(&cfg, &listener, &error) == 0);
    CHECK(jz_listener_port(listener));
    CHECK(jz_accept(listener, &connection, &error) == -ETIMEDOUT);
    CHECK(connection == NULL);
    CHECK(jz_listener_close(&listener, &error) == 0 && listener == NULL);
    CHECK(jz_listener_close(&listener, &error) == 0);
    cfg.peer_address = "127.0.0.1";
    cfg.port = 1;
    cfg.device = "__jz_nonexistent_device__";
    int missing = jz_connect(&cfg, &connection, &error);
    CHECK((missing == -ENODEV || missing == -ENOSYS) && connection == NULL);
    cfg.mode = (enum jz_mode)99;
    CHECK(jz_listen(&cfg, &listener, &error) == -EINVAL);
    cfg.mode = JZ_CM; cfg.gid_index = 0;
    CHECK(jz_listen(&cfg, &listener, &error) == -EINVAL);
    cfg.mode = JZ_OOB; cfg.gid_index = -1; cfg.timeout_ms = 0;
    CHECK(jz_listen(&cfg, &listener, &error) == -EINVAL);
    CHECK(jz_connection_close(&connection, &error) == 0);
    CHECK(jz_connection_quiesce(NULL, &error) == -EINVAL);
    CHECK(jz_write(NULL, NULL, 0, NULL, 0, 1, &error) == -EINVAL);
    CHECK(jz_region_unregister(NULL, &error) == -EINVAL);
    return 0;
}
