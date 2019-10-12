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

struct action {
    char *id;
    char *label;
    wchar_t *wid;
    wchar_t *wlabel;
};

struct notif {
    struct notif_mgr *mgr;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;

    uint32_t id;
    enum urgency urgency;
    wchar_t *app;
    wchar_t *summary;
    wchar_t *body;
    tll(struct action) actions;

    pixman_image_t *pix;
    int image_width;
    int image_height;

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
    if (id == 0 && tll_length(mgr->notifs) > 0)
        return tll_front(mgr->notifs);

    tll_foreach(mgr->notifs, it) {
        if (it->item->id == id)
            return it->item;
    }

    return NULL;
}

struct notif *
notif_mgr_create_notif(struct notif_mgr *mgr, uint32_t replaces_id)
{
    if (replaces_id != 0) {
        struct notif *old_notif = notif_mgr_get_notif(mgr, replaces_id);
        if (old_notif != NULL)
            return old_notif;
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
        .actions = tll_init(),
    };

    bool inserted = false;
    tll_rforeach(mgr->notifs, it) {
        if (it->item->urgency >= notif->urgency) {
            tll_insert_after(mgr->notifs, it, notif);
            inserted = true;
            break;
        }
    }

    if (!inserted)
        tll_push_front(mgr->notifs, notif);

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

    tll_foreach(notif->actions, it) {
        free(it->item.id);
        free(it->item.wid);
        free(it->item.label);
        free(it->item.wlabel);
    }

    tll_free(notif->actions);

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
    if (notif->urgency == urgency)
        return;

    notif->urgency = urgency;

    if (tll_length(notif->mgr->notifs) <= 1)
        return;

    tll_foreach(notif->mgr->notifs, it) {
        if (it->item == notif) {
            tll_remove(notif->mgr->notifs, it);
            break;
        }
    }

    tll_rforeach(notif->mgr->notifs, it) {
        if (it->item->urgency >= notif->urgency) {
            tll_insert_after(notif->mgr->notifs, it, notif);
            notif_mgr_refresh(notif->mgr);
            return;
        }
    }

    tll_push_front(notif->mgr->notifs, notif);
    notif_mgr_refresh(notif->mgr);
}

void
notif_set_image(struct notif *notif, pixman_image_t *pix)
{
    const int max_size = notif->mgr->conf->max_icon_size;

    if (notif->pix != NULL)
        pixman_image_unref(notif->pix);

    notif->pix = pix;
    notif->image_width = pixman_image_get_width(pix);
    notif->image_height = pixman_image_get_height(pix);

    if (notif->image_width <= max_size && notif->image_height <= max_size)
        return;

    double scale_w = notif->image_width / max_size;
    double scale_h = notif->image_height / max_size;
    double scale = scale_w > scale_h ? scale_w : scale_h;

    notif->image_width /= scale;
    notif->image_height /= scale;

    LOG_DBG("image re-scaled: %dx%d -> %dx%d",
            pixman_image_get_width(pix), pixman_image_get_height(pix),
            notif->image_width, notif->image_height);

    struct pixman_transform t;
    pixman_transform_init_scale(
        &t, pixman_double_to_fixed(scale), pixman_double_to_fixed(scale));
    pixman_image_set_transform(pix, &t);
}

void
notif_add_action(struct notif *notif, const char *id, const char *label)
{
    wchar_t *wid, *wlabel;

    size_t chars = mbstowcs(NULL, id, 0);
    wid = malloc((chars + 1) * sizeof(wchar_t));
    mbstowcs(wid, id, chars + 1);

    chars = mbstowcs(NULL, label, 0);
    wlabel = malloc((chars + 1) * sizeof(wchar_t));
    mbstowcs(wlabel, label, chars + 1);

    tll_push_back(
        notif->actions,
        ((struct action){
            .id = strdup(id), .wid = wid,
            .label = strdup(label), .wlabel = wlabel}));
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
notif_layout(struct notif *notif, struct font *font, const pixman_color_t *color,
             const wchar_t *text, int left_pad, int right_pad,
             int y, int *width, int *height, glyph_list_t *glyphs)
{
#define max(x, y) ((x) > (y) ? (x) : (y))

    *width = 0;
    *height = 0;

    const struct config *conf = notif->mgr->conf;
    int x = left_pad;

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

        if (x + glyph->x_advance + right_pad > conf->max_width) {
            *width = max(*width, x + right_pad);
            *height += font->fextents.height;

            x = left_pad;
            y += font->fextents.height;
        }

        struct glyph_layout layout = {
            .glyph = glyph,
            .color = color,
            .x = x,
            .y = y + font->fextents.ascent,
        };

        tll_push_back(*glyphs, layout);
        x += glyph->x_advance;
    }

    *width = max(*width, x + right_pad);
    *height += font->fextents.height;

#undef max
}

static int
notif_show(struct notif *notif, int y)
{
#define max(x, y) ((x) > (y) ? (x) : (y))

    const struct notif_mgr *mgr = notif->mgr;
    const struct config *conf = mgr->conf;
    const struct urgency_config *urgency = &conf->by_urgency[notif->urgency];

    const int pad_horizontal = urgency->body.font->fextents.max_x_advance;
    const int pad_vertical = urgency->body.font->fextents.height;

    int width = 0;
    int height = pad_vertical;
    glyph_list_t glyphs = tll_init();

    int indent = pad_horizontal;
    int _w, _h;

    if (notif->pix != NULL)
        indent += notif->image_width + pad_horizontal;

    /* App */
    notif_layout(notif, urgency->app.font, &urgency->app.color, notif->app, indent, pad_horizontal, height, &_w, &_h, &glyphs);
    if (tll_length(notif->actions) > 0) {
        /* TODO: better 'action' indicator */
        int _a, _b;
        notif_layout(notif, urgency->app.font, &urgency->app.color, L"*", _w - pad_horizontal, pad_horizontal, height, &_a, &_b, &glyphs);
    }
    width = max(width, _w);
    height += _h;

    /* Summary */
    notif_layout(notif, urgency->summary.font, &urgency->summary.color, notif->summary, indent, pad_horizontal, height, &_w, &_h, &glyphs);
    width = max(width, _w);
    height += _h;

    /* Body */
    notif_layout(notif, urgency->body.font, &urgency->body.color, notif->body, indent, pad_horizontal, height, &_w, &_h, &glyphs);
    width = max(width, _w);
    height += _h;

    if (notif->pix != NULL)
        height = max(height, pad_vertical + notif->image_height);
    height += pad_vertical;

#undef max

    struct wayland *wayl = notif->mgr->wayl;
    const int scale = wayl_monitor(wayl)->scale;

    LOG_DBG("show: y = %d, width = %d, height = %d (scale = %d)",
            y, width, height, scale);

    /* Resize and position */
    zwlr_layer_surface_v1_set_size(notif->layer_surface, width / scale, height / scale);
    zwlr_layer_surface_v1_set_margin(
        notif->layer_surface,
        y / scale,                   /* top */
        conf->margin.right / scale,  /* right */
        conf->margin.bottom / scale, /* bottom */
        conf->margin.left / scale);  /* left */
    wayl_roundtrip(wayl);

    struct buffer *buf = wayl_get_buffer(wayl, width, height);
    const int brd_sz = urgency->border.size;;

    /* Border */
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix, &urgency->border.color,
        4, (pixman_rectangle16_t []){
            {0, 0, buf->width, brd_sz},                     /* top */
            {buf->width - brd_sz, 0, brd_sz, buf->height},  /* right */
            {0, buf->height - brd_sz, buf->width, brd_sz},  /* bottom */
            {0, 0, brd_sz, buf->height},                    /* left */
    });

    /* Background */
    pixman_image_fill_rectangles(
        PIXMAN_OP_SRC, buf->pix, &urgency->bg,
        1, &(pixman_rectangle16_t){
            brd_sz, brd_sz,
            buf->width - 2 * brd_sz, buf->height - 2 * brd_sz}
    );

    /* Image */
    if (notif->pix != NULL) {
        pixman_image_composite32(
            PIXMAN_OP_OVER, notif->pix, NULL, buf->pix, 0, 0, 0, 0,
            pad_horizontal, (height - notif->image_height) / 2,
            notif->image_width, notif->image_height);
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
    tll_rforeach(mgr->notifs, it)
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
