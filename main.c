#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <poll.h>
#include <assert.h>
#include <errno.h>
#include <locale.h>

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
    setlocale(LC_ALL, "");

    font_list_t font_list = tll_init();
    tll_push_back(font_list, "Fantasque Sans Mono:pixelsize=24");

    struct font font_regular;
    if (!font_from_name(font_list, "", &font_regular)) {
        LOG_ERR("%s: failed to instantiate font", tll_front(font_list));
        tll_free(font_list);
        return EXIT_FAILURE;
    }

    struct font font_bold;
    if (!font_from_name(font_list, "weight=bold", &font_bold)) {
        LOG_ERR("%s:weight=bold: failed to instantiate font", tll_front(font_list));
        tll_free(font_list);
        font_destroy(&font_regular);
        return EXIT_FAILURE;
    }

    struct font font_italic;
    if (!font_from_name(font_list, "slant=italic", &font_italic)) {
        LOG_ERR("%s:slant=italic: failed to instantiate font", tll_front(font_list));
        tll_free(font_list);
        font_destroy(&font_regular);
        font_destroy(&font_bold);
        return EXIT_FAILURE;
    }

    struct font font_bold_italic;
    if (!font_from_name(font_list, "weight=bold:slant=italic", &font_bold_italic)) {
        LOG_ERR("%s:weight=bolds:slant=italic: failed to instantiate font", tll_front(font_list));
        tll_free(font_list);
        font_destroy(&font_regular);
        font_destroy(&font_bold);
        font_destroy(&font_italic);
        return EXIT_FAILURE;
    }

    tll_free(font_list);

    struct config conf = {
        .bg = {0x3f3f, 0x5f5f, 0x3f3f, 0xffff},
        .max_width = 400,
        .max_height = 200,
        .margin = {
            .top = 25,
            .right = 25,
            .bottom = 25,
            .left = 25,
        },
        .border = {
            .color = {0x9090, 0x9090, 0x9090, 0xffff},
            .size = 1,
        },
        .app = {
            .font = &font_regular,
            .color = {0xffff, 0xffff, 0xffff, 0xffff},
        },
        .summary = {
            .font = &font_bold_italic,
            .color = {0xffff, 0xffff, 0xffff, 0xffff},
        },
        .body = {
            .font = &font_regular,
            .color = {0xffff, 0xffff, 0xffff, 0xffff},
        },
        .action = {
            .font = &font_regular,
            .color = {0xffff, 0xffff, 0xffff, 0xffff},
        },
    };

    struct dbus *bus = NULL;
    struct wayland *wayl = NULL;
    struct notif_mgr *mgr = NULL;

    if ((mgr = notif_mgr_new(&conf)) == NULL)
        goto err;

    if ((wayl = wayl_init()) == NULL)
        goto err;

    if ((bus = dbus_init(wayl, mgr)) == NULL)
        goto err;

    notif_mgr_configure(mgr, wayl, bus);

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

        wayl_flush(wayl);
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
    notif_mgr_destroy(mgr);
    dbus_destroy(bus);
    wayl_destroy(wayl);
    font_destroy(&font_regular);
    font_destroy(&font_bold);
    font_destroy(&font_italic);
    font_destroy(&font_bold_italic);
    return ret;
}
