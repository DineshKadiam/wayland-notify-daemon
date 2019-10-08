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

    struct dbus *bus = dbus_init();
    if (bus == NULL)
        goto err;

    const struct sigaction sigact = {
        .sa_handler = &sig_handler,
    };
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);

    while (!aborted) {
        struct pollfd fds[] = {
            {.fd = dbus_poll_fd(bus), .events = POLLIN},
        };

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

        assert(fds[0].revents & POLLIN);
        if (!dbus_dispatch(bus))
            break;
    }

err:
    dbus_destroy(bus);
    return ret;
}
