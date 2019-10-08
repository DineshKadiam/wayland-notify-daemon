#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <poll.h>
#include <assert.h>
#include <errno.h>

#define LOG_MODULE "main"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "dbus.h"
#include "wayland.h"


static volatile sig_atomic_t aborted = 0;

static void
sig_handler(int signo)
{
    aborted = 1;
}

int
main(int argc, const char *const *argv)
{
    int ret = EXIT_FAILURE;

    struct dbus *bus = NULL;
    struct wayland *wayl = NULL;

    if ((bus = dbus_init()) == NULL)
        goto err;

    if ((wayl = wayl_init()) == NULL)
        goto err;

    const struct sigaction sigact = {
        .sa_handler = &sig_handler,
    };
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);

    while (!aborted) {
        struct pollfd fds[] = {
            {.fd = wayl_poll_fd(wayl), .events = POLLIN},
            {.fd = dbus_poll_fd(bus), .events = POLLIN},
        };

        const size_t WAYL_FD = 0;
        const size_t DBUS_FD = 1;

        int r = poll(fds, sizeof(fds) / sizeof(fds[0]), -1);
        if (r == -1) {
            if (aborted) {
                ret = EXIT_SUCCESS;
                break;
            } else if (errno == EINTR)
                continue;
            else {
                LOG_ERRNO("failed to poll");
                goto err;
            }
        }

        if (fds[WAYL_FD].revents & POLLHUP) {
            LOG_WARN("disconnected from wayland");
            ret = EXIT_SUCCESS;
            break;
        }

        if (fds[DBUS_FD].revents & POLLHUP) {
            LOG_ERR("disconnected from DBus");
            break;
        }

        if (fds[WAYL_FD].revents & POLLIN) {
            if (!wayl_dispatch(wayl))
                break;
        }

        if (fds[DBUS_FD].revents & POLLIN) {
            if (!dbus_dispatch(bus))
                break;
        }
    }

err:
    wayl_destroy(wayl);
    dbus_destroy(bus);
    return ret;
}
