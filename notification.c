#include "notification.h"

#include <stdlib.h>
#include <stdint.h>

#include <pixman.h>
#include <wayland-client.h>
#include <wlr-layer-shell-unstable-v1.h>

#define LOG_MODULE "notification"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "wayland.h"

struct notif {
    struct wayland *wayl;
    struct dbus *bus;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;

    uint32_t id;

    struct buffer *pending;
    bool frame_is_scheduled;
};

static size_t next_id = 0;

static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                        uint32_t serial, uint32_t w, uint32_t h)
{
    LOG_DBG("configure: width=%u, height=%u", w, h);
    zwlr_layer_surface_v1_ack_configure(surface, serial);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_configure,
};

struct notif *
notif_new(struct wayland *wayl, struct dbus *bus)
{
    const struct monitor *mon = wayl_monitor(wayl);
    if (mon == NULL)
        return NULL;

    struct wl_surface *surface = wl_compositor_create_surface(wayl_compositor(wayl));
    if (surface == NULL) {
        LOG_ERR("failed to create wayland surface");
        return NULL;
    }

    struct zwlr_layer_surface_v1 *layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        wayl_layer_shell(wayl), surface, mon->output,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP, "fnott");

    if (layer_surface == NULL) {
        LOG_ERR("failed to create layer shell surface");
        wl_surface_destroy(surface);
        return NULL;
    }

    zwlr_layer_surface_v1_set_size(layer_surface, 100, 100);
    zwlr_layer_surface_v1_set_anchor(
        layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_margin(layer_surface, 50, 50, 50, 50);

    struct notif *notif = malloc(sizeof(*notif));
    *notif = (struct notif) {
        .wayl = wayl,
        .bus = bus,
        .surface = surface,
        .layer_surface = layer_surface,
        .id = next_id++,
    };

    zwlr_layer_surface_v1_add_listener(
        layer_surface, &layer_surface_listener, notif);
    wl_surface_commit(notif->surface);
    return notif;
}

void
notif_destroy(struct notif *notif)
{
    if (notif == NULL)
        return;

    if (notif->layer_surface)
        zwlr_layer_surface_v1_destroy(notif->layer_surface);
    if (notif->surface)
        wl_surface_destroy(notif->surface);

    free(notif);
}

uint32_t
notif_id(const struct notif *notif)
{
    return notif->id;
}

static void frame_callback(
    void *data, struct wl_callback *wl_callback, uint32_t callback_data);

static const struct wl_callback_listener frame_listener = {
    .done = &frame_callback,
};

static void
commit_buffer(struct notif *notif, struct buffer *buf)
{
    assert(buf->busy);

    wl_surface_set_buffer_scale(notif->surface, wayl_monitor(notif->wayl)->scale);
    wl_surface_attach(notif->surface, buf->wl_buf, 0, 0);
    wl_surface_damage_buffer(notif->surface, 0, 0, buf->width, buf->height);

    struct wl_callback *cb = wl_surface_frame(notif->surface);
    wl_callback_add_listener(cb, &frame_listener, notif);

    notif->frame_is_scheduled = true;
    wl_surface_commit(notif->surface);
    wayl_flush(notif->wayl);
}

static void
frame_callback(void *data, struct wl_callback *wl_callback, uint32_t callback_data)
{
    struct notif *notif = data;

    LOG_DBG("frame callback");
    notif->frame_is_scheduled = false;
    wl_callback_destroy(wl_callback);

    if (notif->pending != NULL) {
        commit_buffer(notif, notif->pending);
        notif->pending = NULL;
    }
}

void
notif_show(struct notif *notif)
{
    const int width = 400;
    const int height = 200;
    const int scale = wayl_monitor(notif->wayl)->scale;

    struct buffer *buf = wayl_get_buffer(notif->wayl, width * scale, height * scale);
    zwlr_layer_surface_v1_set_size(notif->layer_surface, buf->width, buf->height);

    wayl_roundtrip(notif->wayl);

    pixman_color_t bg = {0x0000, 0xffff, 0x0000, 0xffff};
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix, &bg,
        1, &(pixman_rectangle16_t) {0, 0, buf->width * scale, buf->height * scale});

    if (notif->frame_is_scheduled) {
        if (notif->pending != NULL)
            notif->pending->busy = false;
        notif->pending = buf;
    } else {
        assert(notif->pending == NULL);
        commit_buffer(notif, buf);
    }
}
