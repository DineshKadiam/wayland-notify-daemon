#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define LOG_MODULE "main"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "ctrl-protocol.h"

int
main(int argc, const char *const *argv)
{
    int ret = EXIT_FAILURE;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1) {
        LOG_ERRNO("failed to create socket");
        goto err;
    }

    bool connected = false;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};

    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime != NULL) {
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/fnott.sock", xdg_runtime);

        if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0)
            connected = true;
    }

    if (!connected) {
        strncpy(addr.sun_path, "/tmp/fnott.sock", sizeof(addr.sun_path) - 1);
        if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
            LOG_ERRNO("failed to connect; is fnott running?");
            goto err;
        }
    }

    /* TODO: endianness */
    struct command cmd = {
        .cmd = CMD_DISMISS_CURRENT,
        .id = 0,
    };

    ssize_t sent = send(fd, &cmd, sizeof(cmd), 0);
    if (sent == -1 || sent != sizeof(cmd)) {
        LOG_ERRNO("failed to send command");
        goto err;
    }

    ret = EXIT_SUCCESS;

err:
    if (fd != -1)
        close(fd);
    return ret;
}
