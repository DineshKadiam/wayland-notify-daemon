#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <poll.h>
#include <assert.h>
#include <errno.h>
#include <locale.h>
#include <getopt.h>

#define LOG_MODULE "main"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "ctrl.h"
#include "dbus.h"
#include "fdm.h"
#include "icon.h"
#include "wayland.h"


volatile sig_atomic_t aborted = 0;

static void
sig_handler(int signo)
{
    aborted = 1;
}

static void
print_usage(const char *prog)
{
    printf("Usage: %s\n", prog);
}

int
main(int argc, char *const *argv)
{
    static const struct option longopts[] = {
        {"help", no_argument, 0, 'h'},
        {NULL,   no_argument, 0, 0},
    };

    while (true) {
        int c = getopt_long(argc, argv, ":h", longopts, NULL);
        if (c == -1)
            break;

        switch (c) {
        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;

        case ':':
            fprintf(stderr, "error: -%c: missing required argument\n", optopt);
            return EXIT_FAILURE;

        case '?':
            fprintf(stderr, "error: -%c: invalid option\n", optopt);
            return EXIT_FAILURE;
        }
    }

    int ret = EXIT_FAILURE;
    setlocale(LC_ALL, "");

    struct config conf = {};
    struct fdm *fdm = NULL;
    struct ctrl *ctrl = NULL;
    struct dbus *bus = NULL;
    struct wayland *wayl = NULL;
    struct notif_mgr *mgr = NULL;
    icon_theme_list_t icon_theme = tll_init();

    if (!config_load(&conf))
        goto err;

    icon_theme = icon_load_theme(conf.icon_theme_name);

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
    config_destroy(conf);
    return ret;
}
