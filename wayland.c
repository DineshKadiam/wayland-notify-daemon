#include "wayland.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <sys/epoll.h>

#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xdg-output-unstable-v1.h>
#include <wlr-layer-shell-unstable-v1.h>

#include <tllist.h>

#define LOG_MODULE "wayland"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "notification.h"

struct seat {
    struct wayland *wayl;
    struct wl_seat *wl_seat;
    uint32_t wl_name;
    char *name;

    struct wl_pointer *wl_pointer;
    struct {
        uint32_t serial;

        /* Current location */
        int x;
        int y;
        struct wl_surface *on_surface;

        /* Cursor image */
        struct wl_surface *surface;
        struct wl_cursor_theme *theme;
        struct wl_cursor *cursor;

        /* Cursor theme info */
        int size;
        int scale;
        char *theme_name;
    } pointer;
};

struct wayland {
    const struct config *conf;
    struct fdm *fdm;
    struct notif_mgr *notif_mgr;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct zxdg_output_manager_v1 *xdg_output_manager;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;


    bool have_argb8888;

    tll(struct seat) seats;
    tll(struct monitor) monitors;
    const struct monitor *monitor;
};

static void
seat_destroy(struct seat *seat)
{
    if (seat == NULL)
        return;

    free(seat->pointer.theme_name);

    if (seat->pointer.theme != NULL)
        wl_cursor_theme_destroy(seat->pointer.theme);
    if (seat->pointer.surface != NULL)
        wl_surface_destroy(seat->pointer.surface);
    if (seat->wl_pointer != NULL)
        wl_pointer_destroy(seat->wl_pointer);
    if (seat->wl_seat != NULL)
        wl_seat_destroy(seat->wl_seat);

    free(seat->name);
}

static void
update_cursor_surface(struct seat *seat)
{
    if (seat->pointer.serial == 0)
        return;

    if (seat->pointer.cursor == NULL)
        return;

    if (seat->wl_pointer == NULL)
        return;

    int scale = seat->pointer.scale;
    wl_surface_set_buffer_scale(seat->pointer.surface, scale);

    struct wl_cursor_image *image = seat->pointer.cursor->images[0];

    wl_surface_attach(
        seat->pointer.surface, wl_cursor_image_get_buffer(image), 0, 0);

    wl_pointer_set_cursor(
        seat->wl_pointer, seat->pointer.serial,
        seat->pointer.surface,
        image->hotspot_x / scale, image->hotspot_y / scale);

    wl_surface_damage_buffer(
        seat->pointer.surface, 0, 0, INT32_MAX, INT32_MAX);

    wl_surface_commit(seat->pointer.surface);
}

static bool
reload_cursor_theme(struct seat *seat, int new_scale)
{
    if (seat->pointer.size == 0)
        return true;

    if (seat->pointer.theme != NULL && seat->pointer.scale == new_scale)
        return true;

    if (seat->pointer.theme != NULL) {
        wl_cursor_theme_destroy(seat->pointer.theme);
        seat->pointer.theme = NULL;
        seat->pointer.cursor = NULL;
    }

    LOG_DBG("reloading cursor theme: %s@%d * %d",
            seat->pointer.theme_name, seat->pointer.size, new_scale);

    struct wayland *wayl = seat->wayl;

    seat->pointer.theme = wl_cursor_theme_load(
        seat->pointer.theme_name, seat->pointer.size * new_scale, wayl->shm);
    if (seat->pointer.theme == NULL) {
        LOG_ERR("failed to load cursor theme");
        return false;
    }

    seat->pointer.cursor = wl_cursor_theme_get_cursor(
        seat->pointer.theme, "left_ptr");
    assert(seat->pointer.cursor != NULL);
    seat->pointer.scale = new_scale;

    return true;
}

static void
shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
    struct wayland *wayl = data;
    if (format == WL_SHM_FORMAT_ARGB8888)
        wayl->have_argb8888 = true;

}

static void
output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y,
                int32_t physical_width, int32_t physical_height,
                int32_t subpixel, const char *make, const char *model,
                int32_t transform)
{
    struct monitor *mon = data;
    mon->width_mm = physical_width;
    mon->height_mm = physical_height;
    mon->subpixel = subpixel;
}

static void
output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
            int32_t width, int32_t height, int32_t refresh)
{
    if ((flags & WL_OUTPUT_MODE_CURRENT) == 0)
        return;

    struct monitor *mon = data;
    mon->refresh = (float)refresh / 1000;
}

static void
output_done(void *data, struct wl_output *wl_output)
{
    struct monitor *mon = data;

    int x_inches = mon->width_mm * 0.03937008;
    int y_inches = mon->height_mm * 0.03937008;
    mon->x_ppi = mon->width_px / x_inches;
    mon->y_ppi = mon->height_px / y_inches;
}

static void
output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
    struct monitor *mon = data;
    mon->scale = factor;

    notif_mgr_refresh(mon->wayl->notif_mgr);
}

static void
xdg_output_handle_logical_position(
    void *data, struct zxdg_output_v1 *xdg_output, int32_t x, int32_t y)
{
    struct monitor *mon = data;
    mon->x = x;
    mon->y = y;
}

static void
xdg_output_handle_logical_size(void *data, struct zxdg_output_v1 *xdg_output,
                               int32_t width, int32_t height)
{
    struct monitor *mon = data;
    mon->width_px = width;
    mon->height_px = height;
}

static void
xdg_output_handle_done(void *data, struct zxdg_output_v1 *xdg_output)
{
}

static void
xdg_output_handle_name(void *data, struct zxdg_output_v1 *xdg_output,
                       const char *name)
{
    struct monitor *mon = data;
    struct wayland *wayl = mon->wayl;
    mon->name = strdup(name);

    if (wayl->conf->output != NULL && strcmp(mon->name, wayl->conf->output) == 0)
        wayl->monitor = mon;
}

static void
xdg_output_handle_description(void *data, struct zxdg_output_v1 *xdg_output,
                              const char *description)
{
}

static void
wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
                 uint32_t serial, struct wl_surface *surface,
                 wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    struct seat *seat = data;
    const struct notif *notif = notif_mgr_get_notif_for_surface(seat->wayl->notif_mgr, surface);
    const struct monitor *mon = notif != NULL ? notif_monitor(notif) : NULL;
    const int scale = mon != NULL ? mon->scale : 1;

    seat->pointer.serial = serial;
    seat->pointer.x = wl_fixed_to_int(surface_x) * scale;
    seat->pointer.y = wl_fixed_to_int(surface_y) * scale;
    seat->pointer.on_surface = surface;
    reload_cursor_theme(seat, scale);
    update_cursor_surface(seat);
}

static void
wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
                 uint32_t serial, struct wl_surface *surface)
{
    struct seat *seat = data;
    seat->pointer.serial = 0;
    seat->pointer.x = seat->pointer.y = 0;
    seat->pointer.on_surface = NULL;
}

static void
wl_pointer_motion(void *data, struct wl_pointer *wl_pointer,
                  uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    struct seat *seat = data;
    struct wayland *wayl = seat->wayl;
    const int scale = wayl->monitor != NULL ? wayl->monitor->scale : 1;

    seat->pointer.x = wl_fixed_to_int(surface_x) * scale;
    seat->pointer.y = wl_fixed_to_int(surface_y) * scale;
}

static void
wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
                  uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    LOG_DBG("BUTTON: button=%x, state=%u", button, state);

    struct seat *seat = data;
    struct wayland *wayl = seat->wayl;

    switch (state) {
    case WL_POINTER_BUTTON_STATE_PRESSED: {
        if (button == BTN_LEFT) {
            assert(seat->pointer.on_surface != NULL);
            notif_mgr_dismiss_surface(wayl->notif_mgr, seat->pointer.on_surface);
        }
        break;
    }

    case WL_POINTER_BUTTON_STATE_RELEASED:
        break;
    }
}

static void
wl_pointer_axis(void *data, struct wl_pointer *wl_pointer,
                uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static void
wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
                         uint32_t axis, int32_t discrete)
{
}

static void
wl_pointer_frame(void *data, struct wl_pointer *wl_pointer)
{
}

static void
wl_pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
                       uint32_t axis_source)
{
}

static void
wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
                     uint32_t time, uint32_t axis)
{
}

const struct wl_pointer_listener pointer_listener = {
    .enter = wl_pointer_enter,
    .leave = wl_pointer_leave,
    .motion = wl_pointer_motion,
    .button = wl_pointer_button,
    .axis = wl_pointer_axis,
    .frame = wl_pointer_frame,
    .axis_source = wl_pointer_axis_source,
    .axis_stop = wl_pointer_axis_stop,
    .axis_discrete = wl_pointer_axis_discrete,
};

static void
seat_capabilities(void *data, struct wl_seat *wl_seat,
                  enum wl_seat_capability caps)
{
    struct seat *seat = data;

    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        if (seat->wl_pointer == NULL) {
            seat->wl_pointer = wl_seat_get_pointer(wl_seat);
            wl_pointer_add_listener(seat->wl_pointer, &pointer_listener, seat);
        }
    } else {
        if (seat->wl_pointer != NULL) {
            wl_pointer_release(seat->wl_pointer);
            seat->wl_pointer = NULL;
        }
    }
}

static void
seat_name(void *data, struct wl_seat *wl_seat, const char *name)
{
    struct seat *seat = data;
    free(seat->name);
    seat->name = strdup(name);
}

static const struct wl_shm_listener shm_listener = {
    .format = &shm_format,
};

static const struct wl_output_listener output_listener = {
    .geometry = &output_geometry,
    .mode = &output_mode,
    .done = &output_done,
    .scale = &output_scale,
};

static struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xdg_output_handle_logical_position,
    .logical_size = xdg_output_handle_logical_size,
    .done = xdg_output_handle_done,
    .name = xdg_output_handle_name,
    .description = xdg_output_handle_description,
};

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};


static bool
verify_iface_version(const char *iface, uint32_t version, uint32_t wanted)
{
    if (version >= wanted)
        return true;

    LOG_ERR("%s: need interface version %u, but compositor only implements %u",
            iface, wanted, version);
    return false;
}

static void
handle_global(void *data, struct wl_registry *registry,
              uint32_t name, const char *interface, uint32_t version)
{
    LOG_DBG("global: 0x%08x, interface=%s, version=%u", name, interface, version);
    struct wayland *wayl = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        const uint32_t required = 4;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->compositor = wl_registry_bind(
            wayl->registry, name, &wl_compositor_interface, required);
    }

    else if (strcmp(interface, wl_shm_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->shm = wl_registry_bind(
            wayl->registry, name, &wl_shm_interface, required);
        wl_shm_add_listener(wayl->shm, &shm_listener, wayl);
    }

    else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        const uint32_t required = 1;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->layer_shell = wl_registry_bind(
            wayl->registry, name, &zwlr_layer_shell_v1_interface, required);
    }

    else if (strcmp(interface, wl_output_interface.name) == 0) {
        const uint32_t required = 3;
        if (!verify_iface_version(interface, version, required))
            return;

        struct wl_output *output = wl_registry_bind(
            wayl->registry, name, &wl_output_interface, required);

        tll_push_back(wayl->monitors, ((struct monitor){
            .wayl = wayl, .output = output,
            .wl_name = name,}
        ));

        struct monitor *mon = &tll_back(wayl->monitors);
        wl_output_add_listener(output, &output_listener, mon);

        /*
         * The "output" interface doesn't give us the monitors'
         * identifiers (e.g. "LVDS-1"). Use the XDG output interface
         * for that.
         */

        assert(wayl->xdg_output_manager != NULL);
        if (wayl->xdg_output_manager != NULL) {
            mon->xdg = zxdg_output_manager_v1_get_xdg_output(
                wayl->xdg_output_manager, mon->output);

            zxdg_output_v1_add_listener(mon->xdg, &xdg_output_listener, mon);
        }
    }

    else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        const uint32_t required = 2;
        if (!verify_iface_version(interface, version, required))
            return;

        wayl->xdg_output_manager = wl_registry_bind(
            wayl->registry, name, &zxdg_output_manager_v1_interface, required);
    }

    else if (strcmp(interface, wl_seat_interface.name) == 0) {
        const uint32_t required = 4;
        if (!verify_iface_version(interface, version, required))
            return;

        struct wl_seat *wl_seat = wl_registry_bind(
            wayl->registry, name, &wl_seat_interface, required);

        /* Cursor */
        unsigned cursor_size = 24;
        const char *cursor_theme = getenv("XCURSOR_THEME");

        {
            const char *env_cursor_size = getenv("XCURSOR_SIZE");
            if (env_cursor_size != NULL) {
                unsigned size;
                if (sscanf(env_cursor_size, "%u", &size) == 1)
                    cursor_size = size;
            }
        }

        /* Note: theme is (re)loaded on scale and output changes */
        LOG_INFO("cursor theme: %s, size: %u", cursor_theme, cursor_size);

        struct wl_surface *surf = wl_compositor_create_surface(wayl->compositor);
        if (surf == NULL) {
            LOG_ERR("failed to create cursor surface");
            wl_seat_destroy(wl_seat);
            return;
        }

        tll_push_back(wayl->seats, ((struct seat){
                    .wayl = wayl,
                    .wl_seat = wl_seat,
                    .wl_name = name,
                    .pointer = {
                        .surface = surf,
                        .size = cursor_size,
                        .theme_name = cursor_theme != NULL ? strdup(cursor_theme) : NULL,
                    }}));

        struct seat *seat = &tll_back(wayl->seats);
        wl_seat_add_listener(wl_seat, &seat_listener, seat);
    }
}

static void
monitor_destroy(struct monitor *mon)
{
    free(mon->name);
    if (mon->xdg != NULL)
        zxdg_output_v1_destroy(mon->xdg);
    if (mon->output != NULL)
        wl_output_destroy(mon->output);
    // free(mon->make);
    // free(mon->model);
}

static void
handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    LOG_DBG("global removed: 0x%08x", name);

    struct wayland *wayl = data;

    tll_foreach(wayl->monitors, it) {
        struct monitor *mon = &it->item;

        if (mon->wl_name != name)
            continue;

        LOG_INFO("monitor disabled: %s", mon->name);

        if (wayl->monitor == mon)
            wayl->monitor = NULL;

        notif_mgr_monitor_removed(wayl->notif_mgr, mon);

        monitor_destroy(mon);
        tll_remove(wayl->monitors, it);
        return;
    }

    tll_foreach(wayl->seats, it) {
        struct seat *seat = &it->item;

        if (seat->wl_name != name)
            continue;

        LOG_INFO("seat removed: %s", seat->name);
        seat_destroy(seat);
        tll_remove(wayl->seats, it);
    }

    LOG_WARN("unknown global removed: 0x%08x", name);
}

static const struct wl_registry_listener registry_listener = {
    .global = &handle_global,
    .global_remove = &handle_global_remove,
};

static bool
fdm_handler(struct fdm *fdm, int fd, int events, void *data)
{
    struct wayland *wayl = data;
    int event_count = 0;

    if (events & EPOLLIN)
        wl_display_read_events(wayl->display);

    while (wl_display_prepare_read(wayl->display) != 0)
        wl_display_dispatch_pending(wayl->display);

    if (events & EPOLLHUP) {
        LOG_WARN("disconnected from Wayland");
        wl_display_cancel_read(wayl->display);
        return false;
    }

    wl_display_flush(wayl->display);
    return event_count != -1;
}

struct wayland *
wayl_init(const struct config *conf, struct fdm *fdm, struct notif_mgr *notif_mgr)
{
    struct wayland *wayl = calloc(1, sizeof(*wayl));
    wayl->conf = conf;
    wayl->notif_mgr = notif_mgr;

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

    wl_display_roundtrip(wayl->display);

    if (!wayl->have_argb8888) {
        LOG_ERR("compositor does not support ARGB surfaces");
        goto err;
    }

    if (tll_length(wayl->monitors) == 0) {
        LOG_ERR("no outputs found");
        goto err;
    }

    tll_foreach(wayl->monitors, it) {
        LOG_INFO("%s: %dx%d+%dx%d (PPI=%dx%d, refresh=%.2fHz, scale=%d)",
                 it->item.name, it->item.width_px, it->item.height_px,
                 it->item.x, it->item.y, it->item.x_ppi, it->item.y_ppi,
                 it->item.refresh, it->item.scale);
    }

    if (wl_display_prepare_read(wayl->display) != 0) {
        LOG_ERRNO("failed to prepare for reading wayland events");
        goto err;
    }

    if (!fdm_add(fdm, wl_display_get_fd(wayl->display), EPOLLIN, &fdm_handler, wayl)) {
        LOG_ERR("failed to register with FDM");
        goto err;
    }
    wayl->fdm = fdm;
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

    if (wayl->fdm != NULL)
        fdm_del_no_close(wayl->fdm, wl_display_get_fd(wayl->display));

    tll_foreach(wayl->monitors, it)
        monitor_destroy(&it->item);
    tll_free(wayl->monitors);

    tll_foreach(wayl->seats, it)
        seat_destroy(&it->item);
    tll_free(wayl->seats);

    if (wayl->layer_shell != NULL)
        zwlr_layer_shell_v1_destroy(wayl->layer_shell);
    if (wayl->xdg_output_manager != NULL)
        zxdg_output_manager_v1_destroy(wayl->xdg_output_manager);
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

struct wl_compositor *
wayl_compositor(const struct wayland *wayl)
{
    return wayl->compositor;
}

struct zwlr_layer_shell_v1 *
wayl_layer_shell(const struct wayland *wayl)
{
    return wayl->layer_shell;
}

struct buffer *
wayl_get_buffer(const struct wayland *wayl, int width, int height)
{
    return shm_get_buffer(wayl->shm, width, height);
}

const struct monitor *
wayl_preferred_monitor(const struct wayland *wayl)
{
    return wayl->monitor;
}

const struct monitor *
wayl_monitor_get(const struct wayland *wayl, struct wl_output *output)
{
    tll_foreach(wayl->monitors, it) {
        if (it->item.output == output)
            return &it->item;
    }

    return NULL;
}

int
wayl_poll_fd(const struct wayland *wayl)
{
    return wl_display_get_fd(wayl->display);
}

void
wayl_flush(struct wayland *wayl)
{
    wl_display_flush(wayl->display);
}

void
wayl_roundtrip(struct wayland *wayl)
{
    wl_display_cancel_read(wayl->display);
    wl_display_roundtrip(wayl->display);

    while (wl_display_prepare_read(wayl->display) != 0)
        wl_display_dispatch_pending(wayl->display);
    wl_display_flush(wayl->display);
}
