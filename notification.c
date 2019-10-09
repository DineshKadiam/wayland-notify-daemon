#include "notification.h"

#include <stdlib.h>
#include <stdint.h>
#include <wchar.h>

#include <pixman.h>
#include <wayland-client.h>
#include <wlr-layer-shell-unstable-v1.h>

#define LOG_MODULE "notification"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "dbus.h"
#include "wayland.h"

struct notif_mgr;

struct notif {
    const struct notif_mgr *mgr;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;

    uint32_t id;
    enum urgency urgency;
    wchar_t *app;
    wchar_t *summary;
    wchar_t *body;
    pixman_image_t *pix;

    struct buffer *pending;
    bool frame_is_scheduled;
};

struct notif_mgr {
    const struct config *conf;
    struct wayland *wayl;
    struct dbus *bus;

    tll(struct notif *) notifs;
};

static size_t next_id = 1;

struct notif_mgr *
notif_mgr_new(const struct config *conf)
{
    struct notif_mgr *mgr = malloc(sizeof(*mgr));
    *mgr = (struct notif_mgr) {
        .conf = conf,
        .wayl = NULL,   /* notif_mgr_configure() */
        .bus = NULL,    /* notif_mgr_configure() */
        .notifs = tll_init(),
    };

    return mgr;
}

void
notif_mgr_destroy(struct notif_mgr *mgr)
{
    if (mgr == NULL)
        return;

    tll_foreach(mgr->notifs, it)
        notif_destroy(it->item);
    tll_free(mgr->notifs);
    free(mgr);
}

void
notif_mgr_configure(struct notif_mgr *mgr, struct wayland *wayl, struct dbus *bus)
{
    assert(mgr->wayl == NULL);
    assert(mgr->bus == NULL);

    mgr->wayl = wayl;
    mgr->bus = bus;
}

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
notif_mgr_get_notif(struct notif_mgr *mgr, uint32_t id)
{
    if (id != 0) {
        tll_foreach(mgr->notifs, it) {
            if (it->item->id == id)
                return it->item;
        }
    }

    struct wayland *wayl = mgr->wayl;
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

    zwlr_layer_surface_v1_set_anchor(
        layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);

    struct notif *notif = malloc(sizeof(*notif));
    *notif = (struct notif) {
        .mgr = mgr,
        .surface = surface,
        .layer_surface = layer_surface,
        .id = next_id++,
        .urgency = URGENCY_NORMAL,
    };
    tll_push_back(mgr->notifs, notif);

    zwlr_layer_surface_v1_add_listener(
        layer_surface, &layer_surface_listener, notif);
    wl_surface_commit(notif->surface);
    return notif;
}

bool
notif_mgr_del_notif(struct notif_mgr *mgr, uint32_t id)
{
    if (id == 0)
        return false;

    tll_foreach(mgr->notifs, it) {
        if (it->item->id != id)
            continue;

        notif_destroy(it->item);
        tll_remove(mgr->notifs, it);
        return true;
    }

    return false;
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

    if (notif->pix != NULL) {
        free(pixman_image_get_data(notif->pix));
        pixman_image_unref(notif->pix);
    }

    free(notif->app);
    free(notif->summary);
    free(notif->body);
    free(notif);
}

uint32_t
notif_id(const struct notif *notif)
{
    return notif->id;
}

void
notif_set_application(struct notif *notif, const char *text)
{
    free(notif->app);

    size_t chars = mbstowcs(NULL, text, 0);
    notif->app = malloc((chars + 1) * sizeof(wchar_t));
    mbstowcs(notif->app, text, chars + 1);
}

void
notif_set_summary(struct notif *notif, const char *text)
{
    free(notif->summary);

    size_t chars = mbstowcs(NULL, text, 0);
    notif->summary = malloc((chars + 1) * sizeof(wchar_t));
    mbstowcs(notif->summary, text, chars + 1);
}

void
notif_set_body(struct notif *notif, const char *text)
{
    free(notif->body);

    size_t chars = mbstowcs(NULL, text, 0);
    notif->body = malloc((chars + 1) * sizeof(wchar_t));
    mbstowcs(notif->body, text, chars + 1);
}

void
notif_set_urgency(struct notif *notif, enum urgency urgency)
{
    /* TODO: re-sort and refresh */
    notif->urgency = urgency;
}

void
notif_set_image(struct notif *notif, pixman_image_t *pix)
{
    if (notif->pix != NULL)
        pixman_image_unref(notif->pix);
    notif->pix = pix;
}

static void frame_callback(
    void *data, struct wl_callback *wl_callback, uint32_t callback_data);

static const struct wl_callback_listener frame_listener = {
    .done = &frame_callback,
};

static void
commit_buffer(struct notif *notif, struct buffer *buf)
{
    struct wayland *wayl = notif->mgr->wayl;
    assert(buf->busy);

    wl_surface_set_buffer_scale(notif->surface, wayl_monitor(wayl)->scale);
    wl_surface_attach(notif->surface, buf->wl_buf, 0, 0);
    wl_surface_damage_buffer(notif->surface, 0, 0, buf->width, buf->height);

    struct wl_callback *cb = wl_surface_frame(notif->surface);
    wl_callback_add_listener(cb, &frame_listener, notif);

    notif->frame_is_scheduled = true;
    wl_surface_commit(notif->surface);
    wayl_flush(wayl);
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

struct glyph_layout {
    const struct glyph *glyph;
    const pixman_color_t *color;
    int x, y;
};

typedef tll(struct glyph_layout) glyph_list_t;

static void
notif_layout(struct notif *notif, struct font *font, const wchar_t *text,
             int indent, int y, int *width, int *height, glyph_list_t *glyphs)
{
#define max(x, y) ((x) > (y) ? (x) : (y))

    *width = 0;
    *height = 0;

    const struct config *conf = notif->mgr->conf;
    int x = indent;

    bool bold __attribute__((unused)) = false;
    bool italic __attribute__((unused)) = false;
    bool underline __attribute__((unused)) = false;

    for (const wchar_t *wc = text; *wc != L'\0'; wc++) {

        if (wcsncasecmp(wc, L"</b>", 4) == 0 ||
            wcsncasecmp(wc, L"</i>", 4) == 0 ||
            wcsncasecmp(wc, L"</u>", 4) == 0)
        {
            if (wc[2] == L'b' || wc[2] == L'B')
                bold = false;
            else if (wc[2] == L'i' || wc[2] == L'I')
                italic = false;
            else
                underline = false;

            wc += 3;
            continue;
        }

        if (wcsncasecmp(wc, L"<b>", 3) == 0 ||
            wcsncasecmp(wc, L"<i>", 3) == 0 ||
            wcsncasecmp(wc, L"<u>", 3) == 0)
        {
            if (wc[1] == L'b' || wc[1] == L'B')
                bold = true;
            else if (wc[1] == L'i' || wc[1] == L'I')
                italic = true;
            else
                underline = true;

            wc += 2;
            continue;
        }

        /* TODO: filter out href and img tags */

#if 0
        char attrs[1024] = {0};
        if (bold)
            strcat(attrs, "weight=bold:");
        if (italic)
            strcat(attrs, "slant=italic:");
        if (strlen(attrs) > 0)
            attrs[strlen(attrs) - 1] = '\0';
#endif

        const struct glyph *glyph = font_glyph_for_wc(font, *wc);
        if (glyph == NULL || !glyph->valid)
            continue;

        if (x + glyph->x_advance + conf->padding.right > conf->max_width) {
            *width = max(*width, x + conf->padding.right);
            *height += font->fextents.height;

            x = indent;
            y += font->fextents.height;
        }

        struct glyph_layout layout = {
            .glyph = glyph,
            .color = &conf->app.color,
            .x = x,
            .y = y + font->fextents.ascent,
        };

        tll_push_back(*glyphs, layout);
        x += glyph->x_advance;
    }

    *width = max(*width, x + conf->padding.right);
    *height += font->fextents.height;

#undef max
}

static int
notif_show(struct notif *notif, int y)
{
#define max(x, y) ((x) > (y) ? (x) : (y))

    const struct notif_mgr *mgr = notif->mgr;
    const struct config *conf = mgr->conf;

    int width = 0;
    int height = conf->padding.top;
    glyph_list_t glyphs = tll_init();

    int indent = conf->padding.left;
    int _w, _h;

    if (notif->pix != NULL)
        indent += pixman_image_get_width(notif->pix) + conf->padding.left;

    /* App */
    notif_layout(notif, conf->app.font, notif->app, indent, height, &_w, &_h, &glyphs);
    width = max(width, _w);
    height += _h;

    /* Summary */
    notif_layout(notif, conf->summary.font, notif->summary, indent, height, &_w, &_h, &glyphs);
    width = max(width, _w);
    height += _h;

    /* Body */
    notif_layout(notif, conf->body.font, notif->body, indent, height, &_w, &_h, &glyphs);
    width = max(width, _w);
    height += _h;

    if (notif->pix != NULL)
        height = max(height, conf->padding.top + pixman_image_get_height(notif->pix));
    height += conf->padding.bottom;

#undef max

    struct wayland *wayl = notif->mgr->wayl;
    const int scale = wayl_monitor(wayl)->scale;

    LOG_DBG("show: y = %d, width = %d, height = %d (scale = %d)",
            y, width, height, scale);

    /* Resize and position */
    zwlr_layer_surface_v1_set_size(notif->layer_surface, width, height);
    zwlr_layer_surface_v1_set_margin(
        notif->layer_surface,
        y,                   /* top */
        conf->margin.right,  /* right */
        conf->margin.bottom, /* bottom */
        conf->margin.left);  /* left */
    wayl_roundtrip(wayl);

    struct buffer *buf = wayl_get_buffer(wayl, width * scale, height * scale);
    const int brd_sz = 1;

    /* Border */
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix, &conf->border.color,
        4, (pixman_rectangle16_t []){
            {0, 0, buf->width, brd_sz * scale},                             /* top */
            {buf->width - brd_sz * scale, 0, brd_sz * scale, buf->height},  /* right */
            {0, buf->height - brd_sz * scale, buf->width, brd_sz * scale},  /* bottom */
            {0, 0, brd_sz * scale, buf->height},                            /* left */
    });

    /* Background */
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix, &conf->bg,
        1, &(pixman_rectangle16_t){
            brd_sz, brd_sz,
            (buf->width - 2 * brd_sz) * scale,
            (buf->height - 2 * brd_sz) * scale}
    );

    /* Image */
    if (notif->pix != NULL) {
        pixman_image_composite32(
            PIXMAN_OP_OVER, notif->pix, NULL, buf->pix, 0, 0, 0, 0,
            conf->padding.left, conf->padding.top,
            pixman_image_get_width(notif->pix),
            pixman_image_get_height(notif->pix));
    }

    /* Text */
    tll_foreach(glyphs, it) {
        const struct glyph *glyph = it->item.glyph;
        if (pixman_image_get_format(glyph->pix) == PIXMAN_a8r8g8b8) {
            /* Glyph surface is a fully rendered bitmap */
            pixman_image_composite32(
                PIXMAN_OP_OVER, glyph->pix, NULL, buf->pix, 0, 0, 0, 0,
                it->item.x + glyph->x, it->item.y - glyph->y,
                glyph->width, glyph->height);
        } else {
            /* Glyph surface is an alpha mask */
            pixman_image_t *src = pixman_image_create_solid_fill(it->item.color);
            pixman_image_composite32(
                PIXMAN_OP_OVER, src, glyph->pix, buf->pix, 0, 0, 0, 0,
                it->item.x + glyph->x, it->item.y - glyph->y,
                glyph->width, glyph->height);
            pixman_image_unref(src);
        }
    }

    tll_free(glyphs);

    if (notif->frame_is_scheduled) {
        if (notif->pending != NULL)
            notif->pending->busy = false;
        notif->pending = buf;
    } else {

        assert(notif->pending == NULL);
        commit_buffer(notif, buf);
    }

    return height;
}

void
notif_mgr_refresh(struct notif_mgr *mgr)
{
    int y = mgr->conf->margin.top;
    tll_foreach(mgr->notifs, it)
        y += notif_show(it->item, y) + mgr->conf->margin.top;
}

static bool
notif_dismiss(struct notif *notif)
{
    dbus_signal_dismissed(notif->mgr->bus, notif->id);
    notif_destroy(notif);
    return true;
}

bool
notif_mgr_dismiss_current(struct notif_mgr *mgr)
{
    if (tll_length(mgr->notifs) == 0)
        return false;

    bool ret = notif_dismiss(tll_pop_front(mgr->notifs));
    notif_mgr_refresh(mgr);
    return ret;
}

bool
notif_mgr_dismiss_id(struct notif_mgr *mgr, uint32_t id)
{
    tll_foreach(mgr->notifs, it) {
        if (it->item->id != id)
            continue;

        struct notif *notif = it->item;
        tll_remove(mgr->notifs, it);

        bool ret = notif_dismiss(notif);
        notif_mgr_refresh(mgr);
        return ret;
    }

    return false;
}

bool
notif_mgr_dismiss_surface(struct notif_mgr *mgr, struct wl_surface *surface)
{
    tll_foreach(mgr->notifs, it) {
        if (it->item->surface != surface)
            continue;

        struct notif *notif = it->item;
        tll_remove(mgr->notifs, it);

        bool ret = notif_dismiss(notif);
        notif_mgr_refresh(mgr);
        return ret;
    }

    return false;
}

bool
notif_mgr_dismiss_all(struct notif_mgr *mgr)
{
    bool ret = true;
    tll_foreach(mgr->notifs, it) {
        struct notif *notif = it->item;
        if (!notif_dismiss(notif))
            ret = false;
        tll_remove(mgr->notifs, it);
    }

    notif_mgr_refresh(mgr);
    return ret;
}
