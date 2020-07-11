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
    uint32_t wl_name;

    int x;
    int y;

    struct {
        /* Physical size, in mm */
        struct {
            int width;
            int height;
        } mm;

        /* Physical size, in pixels */
        struct {
            int width;
            int height;
        } px_real;

        /* Scaled size, in pixels */
        struct {
            int width;
            int height;
        } px_scaled;
    } dim;

    struct {
        /* PPI, based on physical size */
        struct {
            int x;
            int y;
        } real;

        /* PPI, logical, based on scaled size */
        struct {
            int x;
            int y;
        } scaled;
    } ppi;

    int scale;
    float refresh;
    enum wl_output_subpixel subpixel;

    /* From wl_output */
    char *make;
    char *model;

    /* From xdg_output */
    char *name;
    char *description;

    float inch;  /* e.g. 24" */
};

struct wayland *wayl_init(const struct config *conf, struct fdm *fdm, struct notif_mgr *notif_mgr);
void wayl_destroy(struct wayland *wayl);

struct wl_compositor *wayl_compositor(const struct wayland *wayl);
struct zwlr_layer_shell_v1 *wayl_layer_shell(const struct wayland *wayl);

struct buffer *wayl_get_buffer(const struct wayland *wayl, int width, int height);
const struct monitor *wayl_preferred_monitor(const struct wayland *wayl);
const struct monitor *wayl_monitor_get(
    const struct wayland *wayl, struct wl_output *output);
int wayl_guess_scale(const struct wayland *wayl);
int wayl_dpi_guess(const struct wayland *wayl);

int wayl_poll_fd(const struct wayland *wayl);
void wayl_flush(struct wayland *wayl);
void wayl_roundtrip(struct wayland *wayl);
