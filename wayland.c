#include "wayland.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wlr-layer-shell-unstable-v1.h>

#define LOG_MODULE "wayland"
#define LOG_ENABLE_DBG 0
#include "log.h"

struct wayland {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwlr_layer_surface_v1 *layer_surface;

    bool have_argb8888;
};

static void
shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
    struct wayland *wayl = data;
    if (format == WL_SHM_FORMAT_ARGB8888)
        wayl->have_argb8888 = true;

}

static const struct wl_shm_listener shm_listener = {
    .format = &shm_format,
};

static void
handle_global(void *data, struct wl_registry *registry,
              uint32_t name, const char *interface, uint32_t version)
{
    struct wayland *wayl = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        wayl->compositor = wl_registry_bind(
            wayl->registry, name, &wl_compositor_interface, 4);
    }

    else if (strcmp(interface, wl_shm_interface.name) == 0) {
        wayl->shm = wl_registry_bind(wayl->registry, name, &wl_shm_interface, 1);
        wl_shm_add_listener(wayl->shm, &shm_listener, wayl);
        wl_display_roundtrip(wayl->display);
    }

    else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        wayl->layer_shell = wl_registry_bind(
            wayl->registry, name, &zwlr_layer_shell_v1_interface, 1);
    }
#if 0
    else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct wl_output *output = wl_registry_bind(
            wayl->registry, name, &wl_output_interface, 3);

        tll_push_back(wayl->monitors, ((struct monitor){.output = output}));

        struct monitor *mon = &tll_back(wayl->monitors);
        wl_output_add_listener(output, &output_listener, mon);

        /*
         * The "output" interface doesn't give us the monitors'
         * identifiers (e.g. "LVDS-1"). Use the XDG output interface
         * for that.
         */

        assert(wayl->xdg_output_manager != NULL);
        mon->xdg = zxdg_output_manager_v1_get_xdg_output(
            wayl->xdg_output_manager, mon->output);

        zxdg_output_v1_add_listener(mon->xdg, &xdg_output_listener, mon);
        wl_display_roundtrip(wayl->display);
    }


    else if (strcmp(interface, wl_seat_interface.name) == 0) {
        wayl->seat = wl_registry_bind(wayl->registry, name, &wl_seat_interface, 4);
        wl_seat_add_listener(wayl->seat, &seat_listener, c);
        wl_display_roundtrip(wayl->display);
    }

    else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        wayl->xdg_output_manager = wl_registry_bind(
            wayl->registry, name, &zxdg_output_manager_v1_interface, 2);
    }
#endif
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    LOG_WARN("global removed: %u", name);
}

static const struct wl_registry_listener registry_listener = {
    .global = &handle_global,
    .global_remove = &handle_global_remove,
};

struct wayland *
wayl_init(void)
{
    struct wayland *wayl = calloc(1, sizeof(*wayl));

    wayl->display = wl_display_connect(NULL);
    if (wayl->display == NULL) {
        LOG_ERR("failed to connect to wayland; no compositor running?");
        goto err;
    }

    wayl->registry = wl_display_get_registry(wayl->display);
    if (wayl->registry == NULL) {
        LOG_ERR("failed to get wayland registry");
        goto err;
    }

    wl_registry_add_listener(wayl->registry, &registry_listener, wayl);
    wl_display_roundtrip(wayl->display);

    if (wayl->compositor == NULL) {
        LOG_ERR("no compositor");
        goto err;
    }
    if (wayl->shm == NULL) {
        LOG_ERR("no shared memory buffers interface");
        goto err;
    }
    if (wayl->layer_shell == NULL) {
        LOG_ERR("compositor does not support layer shells");
        goto err;
    }

    if (!wayl->have_argb8888) {
        LOG_ERR("compositor does not support ARGB surfaces");
        goto err;
    }

    return wayl;

err:
    wayl_destroy(wayl);
    return NULL;
}

void
wayl_destroy(struct wayland *wayl)
{
    if (wayl == NULL)
        return;

    if (wayl->layer_shell != NULL)
        zwlr_layer_shell_v1_destroy(wayl->layer_shell);
    if (wayl->shm != NULL)
        wl_shm_destroy(wayl->shm);
    if (wayl->compositor != NULL)
        wl_compositor_destroy(wayl->compositor);
    if (wayl->registry != NULL)
        wl_registry_destroy(wayl->registry);
    if (wayl->display != NULL)
        wl_display_disconnect(wayl->display);

    free(wayl);
}

int
wayl_poll_fd(const struct wayland *wayl)
{
    return wl_display_get_fd(wayl->display);
}

bool
wayl_dispatch(struct wayland *wayl)
{
    int event_count =  wl_display_dispatch(wayl->display);
    wl_display_flush(wayl->display);
    return event_count != -1;

}
