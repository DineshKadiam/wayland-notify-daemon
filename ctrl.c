#include "ctrl.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>

#define LOG_MODULE "ctrl"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "ctrl-protocol.h"
#include "tllist.h"

extern volatile sig_atomic_t aborted;

struct ctrl {
    struct fdm *fdm;
    struct notif_mgr *notif_mgr;
    int server_fd;
    char *socket_path;
    tll(int) clients;
};

static bool
fdm_client(struct fdm *fdm, int fd, int events, void *data)
{
    struct ctrl *ctrl = data;

    bool ret = false;
    struct ctrl_request cmd;

    ssize_t count = recv(fd, &cmd, sizeof(cmd), 0);
    if (count == -1 || count != sizeof(cmd)) {
        if (events & EPOLLHUP)
            ret = true;
        else
            LOG_ERRNO("client: FD=%d: failed to receive command", fd);
        goto err;
    }

    /* TODO: endianness */

    struct ctrl_reply reply;

    switch (cmd.cmd) {
    case CTRL_QUIT:
        LOG_DBG("client: FD=%d, quit", fd);
        aborted = 1;
        reply.result = CTRL_OK;
        break;

    case CTRL_DISMISS_CURRENT:
        LOG_DBG("client: FD=%d, dismiss current", fd);
        reply.result = notif_mgr_dismiss_current(ctrl->notif_mgr)
            ? CTRL_OK : CTRL_FAIL;
        break;

    case CTRL_DISMISS_BY_ID:
        LOG_DBG("client: FD=%d, dismiss by-id: %u", fd, cmd.id);
        reply.result = notif_mgr_dismiss_id(ctrl->notif_mgr, cmd.id)
            ? CTRL_OK : CTRL_FAIL;
        break;

    case CTRL_DISMISS_ALL:
        LOG_DBG("client: FD=%d, dismiss all", fd);
        reply.result = notif_mgr_dismiss_all(ctrl->notif_mgr)
            ? CTRL_OK : CTRL_FAIL;
        break;
    }

    LOG_DBG("client: FD=%d, reply: result=%s",
            fd, reply.result == CTRL_OK ? "ok" : "fail");

    count = write(fd, &reply, sizeof(reply));
    if (count != sizeof(reply)) {
        LOG_ERRNO("client: FD=%d: failed to send reply", fd);
        goto err;
    }

    ret = true;

err:
    if (events & EPOLLHUP) {
        /* Client disconnected */
        LOG_DBG("client: FD=%d disconnected", fd);
        fdm_del(fdm, fd);
        close(fd);

        tll_foreach(ctrl->clients, it) {
            if (it->item == fd) {
                tll_remove(ctrl->clients, it);
            }
        }
    }

    return ret;
}

static bool
fdm_server(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLHUP) {
        LOG_ERR("disconnected from controller UNIX socket");
        return false;
    }

    struct ctrl *ctrl = data;

    struct sockaddr_un addr;
    socklen_t addr_size = sizeof(addr);
    int client_fd = accept(ctrl->server_fd, (struct sockaddr *)&addr, &addr_size);

    if (client_fd == -1) {
        LOG_ERRNO("failed to accept client connection");
        return false;
    }

    LOG_DBG("client FD=%d connected", client_fd);

    tll_push_back(ctrl->clients, client_fd);
    if (!fdm_add(ctrl->fdm, client_fd, &fdm_client, ctrl)) {
        LOG_ERR("failed to register client FD with FDM");
        tll_pop_back(ctrl->clients);
        return false;
    }

    return true;
}

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
ctrl_init(struct fdm *fdm, struct notif_mgr *notif_mgr)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
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

    if (listen(fd, 5) < 0) {
        LOG_ERRNO("%s: failed to listen", addr.sun_path);
        goto err;
    }

    struct ctrl *ctrl = malloc(sizeof(*ctrl));
    *ctrl = (struct ctrl){
        .fdm = fdm,
        .notif_mgr = notif_mgr,
        .server_fd = fd,
        .socket_path = sock_path,
        .clients = tll_init(),
    };

    if (!fdm_add(fdm, fd, &fdm_server, ctrl)) {
        LOG_ERR("failed to register with FDM");
        goto err;
    }

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

    fdm_del(ctrl->fdm, ctrl->server_fd);
    close(ctrl->server_fd);

    if (ctrl->socket_path != NULL)
        unlink(ctrl->socket_path);
    free(ctrl->socket_path);

    free(ctrl);
}

int
ctrl_poll_fd(const struct ctrl *ctrl)
{
    return ctrl->server_fd;
}
