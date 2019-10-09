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
#include "ctrl.h"
#include "dbus.h"
#include "fdm.h"
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
        .padding = {
            .top = 20,
            .right = 20,
            .bottom = 20,
            .left = 20,
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

    struct fdm *fdm = NULL;
    struct ctrl *ctrl = NULL;
    struct dbus *bus = NULL;
    struct wayland *wayl = NULL;
    struct notif_mgr *mgr = NULL;

    if ((fdm = fdm_init()) == NULL)
        goto err;

    if ((mgr = notif_mgr_new(&conf)) == NULL)
        goto err;

    if ((wayl = wayl_init(fdm)) == NULL)
        goto err;

    if ((bus = dbus_init(fdm, wayl, mgr)) == NULL)
        goto err;

    if ((ctrl = ctrl_init(fdm, mgr)) == NULL)
        goto err;

    notif_mgr_configure(mgr, wayl, bus);

    const struct sigaction sigact = {
        .sa_handler = &sig_handler,
    };
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);

    while (!aborted) {
        wayl_flush(wayl);
        if (!fdm_poll(fdm))
            break;
    }

    if (aborted)
        ret = EXIT_SUCCESS;

err:
    ctrl_destroy(ctrl);
    notif_mgr_destroy(mgr);
    dbus_destroy(bus);
    wayl_destroy(wayl);
    fdm_destroy(fdm);
    font_destroy(&font_regular);
    font_destroy(&font_bold);
    font_destroy(&font_italic);
    font_destroy(&font_bold_italic);
    return ret;
}
