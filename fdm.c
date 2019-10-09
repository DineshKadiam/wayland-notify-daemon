#include "fdm.h"

#include <stdlib.h>
#include <poll.h>
#include <errno.h>

#define LOG_MODULE "fdm"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "tllist.h"

struct fd {
    int fd;
    void *data;
    fdm_handler_t handler;
};

struct fdm {
    tll(struct fd) fds;
};

struct fdm *
fdm_init(void)
{
    struct fdm *fdm = malloc(sizeof(*fdm));
    *fdm = (struct fdm){
        .fds = tll_init(),
    };
    return fdm;
}

void
fdm_destroy(struct fdm *fdm)
{
    if (fdm == NULL)
        return;
    tll_free(fdm->fds);
    free(fdm);
}

bool
fdm_add(struct fdm *fdm, int fd, fdm_handler_t handler, void *data)
{
#if defined(_DEBUG)
    tll_foreach(fdm->fds, it) {
        if (it->item.fd == fd)
            return false;
    }
#endif

    tll_push_back(
        fdm->fds, ((struct fd){.fd = fd, .data = data, .handler = handler}));
    return true;
}

bool
fdm_del(struct fdm *fdm, int fd)
{
    tll_foreach(fdm->fds, it) {
        if (it->item.fd == fd) {
            tll_remove(fdm->fds, it);
            return true;
        }
    }

    return false;
}

bool
fdm_poll(struct fdm *fdm)
{
    /* TODO: use epoll */
    struct pollfd fds[tll_length(fdm->fds)];

    size_t i = 0;
    tll_foreach(fdm->fds, it)
        fds[i++] = (struct pollfd){.fd = it->item.fd, .events = POLLIN};

    int ret = poll(fds, tll_length(fdm->fds), -1);
    if (ret == -1) {
        if (errno == EINTR)
            return true;

        LOG_ERRNO("failed to poll");
        return false;
    }

    i = 0;
    tll_foreach(fdm->fds, it) {
        if (fds[i].revents & POLLIN) {
            if (!it->item.handler(fdm, it->item.fd, fds[i].revents, it->item.data))
                return false;
        }

        i++;
    }

    return true;
}
