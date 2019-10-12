#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#define LOG_MODULE "main"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "ctrl-protocol.h"

static void
print_usage(const char *prog)
{
    printf("Usage: %s dismiss [all | <id>]\n"
           "       %s quit\n",
           prog, prog);
}

int
main(int argc, const char *const *argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *cmd_word = argv[1];

    enum command_type cmd_type;
    uint32_t id;

    if (strcmp(cmd_word, "quit") == 0) {
        cmd_type = CMD_QUIT;
        id = 0;
    }

    else if (strcmp(cmd_word, "dismiss") == 0) {
        if (argc >= 3) {
            const char *id_or_all = argv[2];
            if (strcmp(id_or_all, "all") == 0) {
                cmd_type = CMD_DISMISS_ALL;
                id = 0;
            } else {
                char *end = NULL;
                errno = 0;
                id = strtoul(id_or_all, &end, 0);

                if (errno != 0 || *end != '\0') {
                    LOG_ERR("%s: invalid notification ID (expected an integer)", id_or_all);
                    return EXIT_FAILURE;
                }

                cmd_type = CMD_DISMISS_BY_ID;
            }
        } else {
            cmd_type = CMD_DISMISS_CURRENT;
            id = 0;
        }
    }

    else {
        LOG_ERR("%s: invalid command", cmd_word);
        return EXIT_FAILURE;
    }

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
        .cmd = cmd_type,
        .id = id,
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
