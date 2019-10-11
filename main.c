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
#include "icon.h"
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
    tll_push_back(font_list, "Fantasque Sans Mono");

    struct font font_summary = {0};
    struct font font_app = {0};
    struct font font_body = {0};

    if (!font_from_name(font_list, "pixelsize=32:weight=bold", &font_summary) ||
        !font_from_name(font_list, "pixelsize=20:slant=italic", &font_app) ||
        !font_from_name(font_list, "pixelsize=24", &font_body))
    {
        LOG_ERR("%s: failed to instantiate font", tll_front(font_list));
        font_destroy(&font_summary);
        font_destroy(&font_app);
        font_destroy(&font_body);
        tll_free(font_list);
        return EXIT_FAILURE;
    }

    tll_free(font_list);

    struct config conf = {
        .max_width = 1400,
        .max_height = 1000,
        .max_icon_size = 64,
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
        .by_urgency = {
            {
                .bg = {0x2b2b, 0x2b2b, 0x2b2b, 0xffff},
                .border = {
                    .color = {0x9090, 0x9090, 0x9090, 0xffff},
                    .size = 1,
                },
                .app = {
                    .font = &font_app,
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
                .summary = {
                    .font = &font_summary,
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
                .body = {
                    .font = &font_body,
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
                .action = {
                    .font = &font_body,
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
            },
            {
                .bg = {0x3f3f, 0x5f5f, 0x3f3f, 0xffff},
                .border = {
                    .color = {0x9090, 0x9090, 0x9090, 0xffff},
                    .size = 1,
                },
                .app = {
                    .font = &font_app,
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .summary = {
                    .font = &font_summary,
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .body = {
                    .font = &font_body,
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .action = {
                    .font = &font_body,
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
            },
            {
                .bg = {0x6c6c, 0x3333, 0x3333, 0xffff},
                .border = {
                    .color = {0x9090, 0x9090, 0x9090, 0xffff},
                    .size = 1,
                },
                .app = {
                    .font = &font_app,
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .summary = {
                    .font = &font_summary,
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .body = {
                    .font = &font_body,
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .action = {
                    .font = &font_body,
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
            },
        },
    };

    struct fdm *fdm = NULL;
    struct ctrl *ctrl = NULL;
    struct dbus *bus = NULL;
    struct wayland *wayl = NULL;
    struct notif_mgr *mgr = NULL;
    icon_theme_list_t icon_theme = tll_init();

    icon_theme = icon_load_theme("hicolor");

    if ((fdm = fdm_init()) == NULL)
        goto err;

    if ((mgr = notif_mgr_new(&conf)) == NULL)
        goto err;

    if ((wayl = wayl_init(fdm, mgr)) == NULL)
        goto err;

    if ((bus = dbus_init(&conf, fdm, wayl, mgr, &icon_theme)) == NULL)
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
    icon_themes_destroy(icon_theme);
    ctrl_destroy(ctrl);
    notif_mgr_destroy(mgr);
    dbus_destroy(bus);
    wayl_destroy(wayl);
    fdm_destroy(fdm);
    font_destroy(&font_summary);
    font_destroy(&font_app);
    font_destroy(&font_body);
    return ret;
}
