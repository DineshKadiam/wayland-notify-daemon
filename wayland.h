#pragma once

#include <stdbool.h>
#include <wayland-client.h>
#include <fcft/fcft.h>

#include "config.h"
#include "fdm.h"
#include "notification.h"
#include "shm.h"

struct wayland;

struct monitor {
    struct wayland *wayl;
    struct wl_output *output;
    struct zxdg_output_v1 *xdg;
    char *name;
    uint32_t wl_name;

    int x;
    int y;

    int width_mm;
    int height_mm;

    int width_px;
    int height_px;

    int x_ppi;
    int y_ppi;

    int scale;
    float refresh;
    enum fcft_subpixel subpixel;
};

struct wayland *wayl_init(const struct config *conf, struct fdm *fdm, struct notif_mgr *notif_mgr);
void wayl_destroy(struct wayland *wayl);

struct wl_compositor *wayl_compositor(const struct wayland *wayl);
struct zwlr_layer_shell_v1 *wayl_layer_shell(const struct wayland *wayl);

struct buffer *wayl_get_buffer(const struct wayland *wayl, int width, int height);
const struct monitor *wayl_monitor(const struct wayland *wayl);

int wayl_poll_fd(const struct wayland *wayl);
void wayl_flush(struct wayland *wayl);
void wayl_roundtrip(struct wayland *wayl);
