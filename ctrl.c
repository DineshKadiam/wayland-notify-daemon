#include "ctrl.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define LOG_MODULE "ctrl"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "ctrl-protocol.h"

struct ctrl {
    struct notif_mgr *notif_mgr;
    int server_fd;
    char *socket_path;
};

static char *
get_socket_path(void)
{
    const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
    if (xdg_runtime == NULL)
        return strdup("/tmp/fnott.sock");

    char *path = malloc(strlen(xdg_runtime) + 1 + strlen("fnott.sock") + 1);
    sprintf(path, "%s/fnott.sock", xdg_runtime);
    return path;
}

struct ctrl *
ctrl_init(struct notif_mgr *notif_mgr)
{
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd == -1) {
        LOG_ERRNO("failed to create UNIX socket");
        return NULL;
    }

    char *sock_path = get_socket_path();
    if (sock_path == NULL)
        goto err;

    unlink(sock_path);

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERRNO("%s: failed to bind", addr.sun_path);
        goto err;
    }

    struct ctrl *ctrl = malloc(sizeof(*ctrl));
    *ctrl = (struct ctrl){
        .notif_mgr = notif_mgr,
        .server_fd = fd,
        .socket_path = sock_path,
    };

    return ctrl;

err:
    if (sock_path)
        free(sock_path);
    if (fd != -1)
        close(fd);
    return NULL;
}

void
ctrl_destroy(struct ctrl *ctrl)
{
    if (ctrl == NULL)
        return;
    if (ctrl->socket_path != NULL)
        unlink(ctrl->socket_path);
    free(ctrl->socket_path);
    close(ctrl->server_fd);
    free(ctrl);
}

int
ctrl_poll_fd(const struct ctrl *ctrl)
{
    return ctrl->server_fd;
}

bool
ctrl_dispatch(struct ctrl *ctrl)
{
    struct command cmd;

    ssize_t count = recv(ctrl->server_fd, &cmd, sizeof(cmd), 0);
    if (count == -1 || count != sizeof(cmd)) {
        LOG_ERRNO("failed to receive command");
        return false;
    }

    /* TODO: endianness */

    switch (cmd.cmd) {
    case CMD_DISMISS_CURRENT:
        notif_mgr_dismiss_current(ctrl->notif_mgr);
        break;

    case CMD_DISMISS_BY_ID:
        notif_mgr_dismiss_id(ctrl->notif_mgr, cmd.id);
        break;

    case CMD_DISMISS_ALL:
        notif_mgr_dismiss_all(ctrl->notif_mgr);
        break;
    }

    return true;
}
