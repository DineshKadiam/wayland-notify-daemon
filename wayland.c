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

#define LOG_MODULE "wayland"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "notification.h"
#include "tllist.h"

struct wayland {
    const struct config *conf;
    struct fdm *fdm;
    struct notif_mgr *notif_mgr;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_seat *seat;
    struct zxdg_output_manager_v1 *xdg_output_manager;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;

    struct {
        struct wl_pointer *pointer;
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
        char *theme_name;
    } pointer;

    bool have_argb8888;

    tll(struct monitor) monitors;
    const struct monitor *monitor;
};

static void
update_cursor_surface(struct wayland *wayl)
{
    if (wayl->pointer.cursor == NULL)
        return;

    const int scale = wayl->monitor->scale;
    wl_surface_set_buffer_scale(wayl->pointer.surface, scale);

    struct wl_cursor_image *image = wayl->pointer.cursor->images[0];

    wl_surface_attach(
        wayl->pointer.surface, wl_cursor_image_get_buffer(image), 0, 0);

    wl_pointer_set_cursor(
        wayl->pointer.pointer, wayl->pointer.serial,
        wayl->pointer.surface,
        image->hotspot_x / scale, image->hotspot_y / scale);

    wl_surface_damage_buffer(
        wayl->pointer.surface, 0, 0, INT32_MAX, INT32_MAX);

    wl_surface_commit(wayl->pointer.surface);
}

static bool
reload_cursor_theme(struct wayland *wayl)
{
    if (wayl->pointer.size == 0)
        return true;

    if (wayl->pointer.theme != NULL) {
        wl_cursor_theme_destroy(wayl->pointer.theme);
        wayl->pointer.theme = NULL;
        wayl->pointer.cursor = NULL;
    }

    LOG_DBG("reloading cursor theme: %s@%d",
            wayl->pointer.theme_name, wayl->pointer.size);

    wayl->pointer.theme = wl_cursor_theme_load(
        wayl->pointer.theme_name, wayl->pointer.size * wayl->monitor->scale, wayl->shm);
    if (wayl->pointer.theme == NULL) {
        LOG_ERR("failed to load cursor theme");
        return false;
    }

    wayl->pointer.cursor = wl_cursor_theme_get_cursor(
        wayl->pointer.theme, "left_ptr");
    assert(wayl->pointer.cursor != NULL);
    update_cursor_surface(wayl);

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
}

static void
output_mode(void *data, struct wl_output *wl_output, uint32_t flags,
            int32_t width, int32_t height, int32_t refresh)
{
}

static void
output_done(void *data, struct wl_output *wl_output)
{
}

static void
output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
    struct monitor *mon = data;
    mon->scale = factor;
    reload_cursor_theme(mon->wayl);
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
    mon->name = strdup(name);
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
    struct wayland *wayl = data;
    wayl->pointer.serial = serial;
    wayl->pointer.x = wl_fixed_to_int(surface_x) * wayl->monitor->scale;
    wayl->pointer.y = wl_fixed_to_int(surface_y) * wayl->monitor->scale;
    wayl->pointer.on_surface = surface;
    update_cursor_surface(wayl);
}

static void
wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
                 uint32_t serial, struct wl_surface *surface)
{
    struct wayland *wayl = data;
    wayl->pointer.serial = 0;
    wayl->pointer.x = wayl->pointer.y = 0;
    wayl->pointer.on_surface = NULL;
}

static void
wl_pointer_motion(void *data, struct wl_pointer *wl_pointer,
                  uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    struct wayland *wayl = data;
    wayl->pointer.x = wl_fixed_to_int(surface_x) * wayl->monitor->scale;
    wayl->pointer.y = wl_fixed_to_int(surface_y) * wayl->monitor->scale;
}

static void
wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
                  uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    LOG_DBG("BUTTON: button=%x, state=%u", button, state);

    struct wayland *wayl = data;

    switch (state) {
    case WL_POINTER_BUTTON_STATE_PRESSED: {
        if (button == BTN_LEFT) {
            assert(wayl->pointer.on_surface != NULL);
            notif_mgr_dismiss_surface(wayl->notif_mgr, wayl->pointer.on_surface);
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
    struct wayland *wayl = data;

    if (wayl->pointer.pointer != NULL) {
        wl_pointer_release(wayl->pointer.pointer);
        wayl->pointer.pointer = NULL;
    }

    if (caps & WL_SEAT_CAPABILITY_POINTER) {
        wayl->pointer.pointer = wl_seat_get_pointer(wl_seat);
        wl_pointer_add_listener(wayl->pointer.pointer, &pointer_listener, wayl);
    }
}

static void
seat_name(void *data, struct wl_seat *wl_seat, const char *name)
{
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

    else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct wl_output *output = wl_registry_bind(
            wayl->registry, name, &wl_output_interface, 3);

        tll_push_back(wayl->monitors, ((struct monitor){
            .wayl = wayl, .output = output}
        ));

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

    else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        wayl->xdg_output_manager = wl_registry_bind(
            wayl->registry, name, &zxdg_output_manager_v1_interface, 2);
    }

    else if (strcmp(interface, wl_seat_interface.name) == 0) {
        wayl->seat = wl_registry_bind(wayl->registry, name, &wl_seat_interface, 4);
        wl_seat_add_listener(wayl->seat, &seat_listener, wayl);
        wl_display_roundtrip(wayl->display);
    }
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

static bool
fdm_handler(struct fdm *fdm, int fd, int events, void *data)
{
    struct wayland *wayl = data;
    int event_count =  wl_display_dispatch(wayl->display);

    if (events & EPOLLHUP) {
        LOG_INFO("disconnected from Wayland");
        return false;
    }

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

    if (!wayl->have_argb8888) {
        LOG_ERR("compositor does not support ARGB surfaces");
        goto err;
    }

    /* TODO: let user choose output */
    tll_foreach(wayl->monitors, it) {
        LOG_DBG("%s: %dx%d+%dx%d (scale=%d)", it->item.name, it->item.width_px,
                it->item.height_px, it->item.x, it->item.y, it->item.scale);

        if (wayl->conf->output == NULL)
            wayl->monitor = &it->item;
        else if (strcmp(it->item.name, wayl->conf->output) == 0)
            wayl->monitor = &it->item;
    }

    if (wayl->monitor == NULL) {
        LOG_ERR("no outputs found");
        goto err;
    }

    if (!fdm_add(fdm, wl_display_get_fd(wayl->display), EPOLLIN, &fdm_handler, wayl)) {
        LOG_ERR("failed to register with FDM");
        goto err;
    }
    wayl->fdm = fdm;

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
    wayl->pointer.size = cursor_size;
    wayl->pointer.theme_name = cursor_theme != NULL ? strdup(cursor_theme) : NULL;

    wayl->pointer.surface = wl_compositor_create_surface(wayl->compositor);
    if (wayl->pointer.surface == NULL) {
        LOG_ERR("failed to create cursor surface");
        goto err;
    }

    reload_cursor_theme(wayl);

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
        fdm_del(wayl->fdm, wl_display_get_fd(wayl->display));

    tll_foreach(wayl->monitors, it) {
        free(it->item.name);
        if (it->item.xdg)
            zxdg_output_v1_destroy(it->item.xdg);
        if (it->item.output != NULL)
            wl_output_destroy(it->item.output);
    }
    tll_free(wayl->monitors);

    free(wayl->pointer.theme_name);

    if (wayl->pointer.theme != NULL)
        wl_cursor_theme_destroy(wayl->pointer.theme);
    if (wayl->pointer.surface != NULL)
        wl_surface_destroy(wayl->pointer.surface);
    if (wayl->pointer.pointer != NULL)
        wl_pointer_destroy(wayl->pointer.pointer);
    if (wayl->layer_shell != NULL)
        zwlr_layer_shell_v1_destroy(wayl->layer_shell);
    if (wayl->xdg_output_manager != NULL)
        zxdg_output_manager_v1_destroy(wayl->xdg_output_manager);
    if (wayl->seat != NULL)
        wl_seat_destroy(wayl->seat);
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
wayl_monitor(const struct wayland *wayl)
{
    return wayl->monitor;
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
    wl_display_roundtrip(wayl->display);
}
