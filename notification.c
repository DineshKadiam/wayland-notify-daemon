#include "notification.h"

#include <stdlib.h>
#include <stdint.h>
#include <wchar.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <fcntl.h>

#include <pixman.h>
#include <wayland-client.h>
#include <wlr-layer-shell-unstable-v1.h>

#define LOG_MODULE "notification"
#define LOG_ENABLE_DBG 0
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
    struct fdm *fdm;
    struct wayland *wayl;
    struct dbus *bus;

    tll(struct notif *) notifs;
};

static size_t next_id = 1;

struct notif_mgr *
notif_mgr_new(const struct config *conf, struct fdm *fdm)
{
    struct notif_mgr *mgr = malloc(sizeof(*mgr));
    *mgr = (struct notif_mgr) {
        .conf = conf,
        .fdm = fdm,
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

char *
notif_get_summary(const struct notif *notif)
{
    if (notif->summary == NULL)
        return NULL;

    size_t chars = wcstombs(NULL, notif->summary, 0);
    char *ret = malloc(chars + 1);
    wcstombs(ret, notif->summary, chars + 1);
    return ret;
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

    if (notif->pix != NULL) {
        free(pixman_image_get_data(notif->pix));
        pixman_image_unref(notif->pix);
        notif->pix = NULL;
    }

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

        if (x + glyph->x_advance + right_pad > conf->max_width || *wc == L'\n') {
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

    assert(*width <= conf->max_width);

#undef max
}

static int
notif_show(struct notif *notif, int y)
{
#define max(x, y) ((x) > (y) ? (x) : (y))
#define min(x, y) ((x) < (y) ? (x) : (y))

    const struct notif_mgr *mgr = notif->mgr;
    const struct config *conf = mgr->conf;
    const struct urgency_config *urgency = &conf->by_urgency[notif->urgency];

    const int pad_horizontal = urgency->body.font->fextents.height;
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

#if 0
    tll_foreach(notif->actions, it) {
        notif_layout(notif, urgency->action.font, &urgency->action.color, it->item.label, indent, pad_horizontal, height, &_w, &_h, &glyphs);
        width = max(width, _w);
        height += _h;
    }
#endif

    if (notif->pix != NULL)
        height = max(height, pad_vertical + notif->image_height);
    height += pad_vertical;

    height = min(height, conf->max_height);

#undef max
#undef min

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

ssize_t
notif_mgr_get_ids(const struct notif_mgr *mgr, uint32_t *ids, size_t max)
{
    size_t count = 0;
    tll_foreach(mgr->notifs, it) {
        if (++count <= max && ids != NULL)
            ids[count - 1] = it->item->id;
    }

    return count;
}

static bool
notif_dismiss(struct notif *notif)
{
    dbus_signal_dismissed(notif->mgr->bus, notif->id);
    notif_destroy(notif);
    return true;
}

static bool
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
    if (id == 0)
        return notif_mgr_dismiss_current(mgr);

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

struct action_async {
    struct fdm *fdm;
    struct notif_mgr *mgr;

    /* The notification may be dismissed while we're waiting for the
     * action selection. So, store the ID, and re-retreieve the
     * notification when we're done */
    uint32_t notif_id;

    pid_t pid;
    int to_child;      /* Child's stdin */
    int from_child;    /* Child's stdout */

    char *input;       /* Data to be sent to child (action labels) */
    size_t input_idx;  /* Where to start next write() */
    size_t input_len;  /* Total amount of data */

    char *output;      /* Output from child */
    size_t output_len; /* Amount of output received (so far) */

    notif_select_action_cb completion_cb;
    void *data;
};

static bool
fdm_action_writer(struct fdm *fdm, int fd, int events, void *data)
{
    struct action_async *async = data;

    ssize_t count = write(
        async->to_child,
        &async->input[async->input_idx],
        async->input_len - async->input_idx);

    if (count < 0) {
        LOG_ERRNO("could not write actions to actions selection helper");
        goto done;
    }

    async->input_idx += count;
    if (async->input_idx >= async->input_len) {
        /* Close child's stdin, to signal there are no more labels */
        LOG_DBG("all input sent to child");
        goto done;
    }

    return true;

done:
    fdm_del(async->fdm, async->to_child);
    async->to_child = -1;
    return true;
}

static bool
fdm_action_reader(struct fdm *fdm, int fd, int events, void *data)
{
    struct action_async *async = data;

    const size_t chunk_sz = 128;
    char buf[chunk_sz];

    ssize_t count = read(async->from_child, buf, chunk_sz);
    if (count < 0) {
        LOG_ERRNO("failed to read from actions selection helper");
        goto check_pollhup;
    }

    /* Append to previously received response */
    size_t new_len = async->output_len + count;
    async->output = realloc(async->output, new_len);
    memcpy(&async->output[async->output_len], buf, count);
    async->output_len = new_len;

check_pollhup:

    if (!(events & EPOLLHUP))
        return true;

    /* Strip trailing spaces/newlines */
    while (async->output_len > 0 && isspace(async->output[--async->output_len]))
        async->output[async->output_len] = '\0';

    /* Extract the data we need from the info struct, then free it */
    struct notif_mgr *mgr = async->mgr;
    pid_t pid = async->pid;
    uint32_t notif_id = async->notif_id;
    notif_select_action_cb completion_cb = async->completion_cb;
    void *cb_data = async->data;
    char *choosen = async->output;
    size_t choosen_len = async->output_len;

    if (async->to_child != -1) {
        /* This is an error case - normally, the writer should have
         * completed and closed this already */
        fdm_del(async->fdm, async->to_child);
    }

    fdm_del(async->fdm, async->from_child);

    free(async->input);
    free(async);

    /* Wait for child to die */
    int status;
    waitpid(pid, &status, 0);
    LOG_DBG("child exited with status 0x%08x", status);

    const char *action_id = NULL;

    if (!WIFEXITED(status)) {
        LOG_ERR("child did not exit normally");
        goto done;
    }

    if (WEXITSTATUS(status) != 0) {
        uint8_t code = WEXITSTATUS(status);
        if (code >> 1)
            LOG_ERRNO_P("failed to execute action selection helper", code & 0x7f);

        goto done;
    }

    struct notif *notif = notif_mgr_get_notif(mgr, notif_id);
    if (notif == NULL) {
        LOG_WARN("notification was dismissed before we could signal action: %.*s",
                 (int)choosen_len, choosen);
        goto done;
    }

    /* Map returned label to action ID */
    tll_foreach(notif->actions, it) {
        if (strncmp(it->item.label, choosen, choosen_len) == 0) {
            action_id = it->item.id;
            goto done;
        }
    }

    LOG_WARN("could not map choosen action label to action ID: %.*s", (int)choosen_len, choosen);

done:
    completion_cb(notif_id, action_id, cb_data);
    free(choosen);
    return true;
}

static bool
push_argv(char ***argv, size_t *size, char *arg, size_t *argc)
{
    if (arg != NULL && arg[0] == '%')
        return true;

    if (*argc >= *size) {
        size_t new_size = *size > 0 ? 2 * *size : 10;
        char **new_argv = realloc(*argv, new_size * sizeof(new_argv[0]));

        if (new_argv == NULL)
            return false;

        *argv = new_argv;
        *size = new_size;
    }

    (*argv)[(*argc)++] = arg;
    return true;
}

static bool
tokenize_cmdline(char *cmdline, char ***argv)
{
    *argv = NULL;
    size_t argv_size = 0;

    bool first_token_is_quoted = cmdline[0] == '"' || cmdline[0] == '\'';
    char delim = first_token_is_quoted ? cmdline[0] : ' ';

    char *p = first_token_is_quoted ? &cmdline[1] : &cmdline[0];

    size_t idx = 0;
    while (*p != '\0') {
        char *end = strchr(p, delim);
        if (end == NULL) {
            if (delim != ' ') {
                LOG_ERR("unterminated %s quote\n", delim == '"' ? "double" : "single");
                free(*argv);
                return false;
            }

            if (!push_argv(argv, &argv_size, p, &idx) ||
                !push_argv(argv, &argv_size, NULL, &idx))
            {
                goto err;
            } else
                return true;
        }

        *end = '\0';

        if (!push_argv(argv, &argv_size, p, &idx))
            goto err;

        p = end + 1;
        while (*p == delim)
            p++;

        while (*p == ' ')
            p++;

        if (*p == '"' || *p == '\'') {
            delim = *p;
            p++;
        } else
            delim = ' ';
    }

    if (!push_argv(argv, &argv_size, NULL, &idx))
        goto err;

    return true;

err:
    free(*argv);
    return false;
}

void
notif_select_action(
    const struct notif *notif, notif_select_action_cb completion_cb, void *data)
{
    char *copy = strdup(notif->mgr->conf->selection_helper);
    char **argv = NULL;
    int to_child[2] = {-1, -1};    /* Pipe to child's STDIN */
    int from_child[2] = {-1, -1};  /* Pipe to child's STDOUT */

    if (tll_length(notif->actions) == 0)
        goto err_before_fork;

    if (!tokenize_cmdline(copy, &argv))
        goto err_before_fork;

    if (pipe(to_child) < 0 || pipe(from_child) < 0) {
        LOG_ERRNO("failed to create pipe");
        goto err_before_fork;
    }

    int pid = fork();
    if (pid == -1) {
        LOG_ERRNO("failed to fork");
        goto err_before_fork;
    }

    if (pid == 0) {
        /*
         * Child
         */

        close(to_child[1]);
        close(from_child[0]);

        /* Rewire pipes to child's STDIN/STDOUT */
        if (dup2(to_child[0], STDIN_FILENO) < 0 ||
            dup2(from_child[1], STDOUT_FILENO) < 0)
        {
            goto child_exit;
        }

        close(to_child[0]);
        close(from_child[1]);

        execvp(argv[0], argv);

    child_exit:
        _exit(1 << 7 | errno);
    }

    assert(pid > 0);

    /*
     * Parent
     */

    free(copy);
    free(argv);
    close(to_child[0]);
    close(from_child[1]);

    /*
     * Writing the action labels and waiting for the response can take
     * a *very* long time, and we can't block execution.
     *
     * Make our pipe ends non-blocking, and use the FDM to write/read
     * them asynchronously.
     */

    struct action_async *async = NULL;
    size_t input_len = 0;
    char *input = NULL;

    if (fcntl(to_child[1], F_SETFL, fcntl(to_child[1], F_GETFL) | O_NONBLOCK) < 0 ||
        fcntl(from_child[0], F_SETFL, fcntl(from_child[0], F_GETFL) | O_NONBLOCK) < 0)
    {
        LOG_ERRNO("failed to make pipes non blocking");
        goto err_in_parent;
    }

    /* Construct a single string consisting of all the action labels
     * separated by newlines */
    tll_foreach(notif->actions, it)
        input_len += strlen(it->item.label) + 1;

    input = malloc(input_len + 1);
    input[0] = '\0';
    tll_foreach(notif->actions, it) {
        strcat(input, it->item.label);
        strcat(input, "\n");
    }

    /* FDM callback data. Shared by both the write and read callback,
     * but *only* freed by the *read* handler. */
    async = malloc(sizeof(*async));
    *async = (struct action_async) {
        .fdm = notif->mgr->fdm,
        .mgr = notif->mgr,
        .notif_id = notif->id,
        .to_child = to_child[1],
        .from_child = from_child[0],
        .input = input,
        .input_len = input_len,
        .input_idx = 0,
        .output = NULL,
        .output_len = 0,
        .completion_cb = completion_cb,
        .data = data,
    };

    if (!fdm_add(notif->mgr->fdm, to_child[1], EPOLLOUT, &fdm_action_writer, async) ||
        !fdm_add(notif->mgr->fdm, from_child[0], EPOLLIN, &fdm_action_reader, async))
    {
        goto err_in_parent;
    }

    return;

err_before_fork:
    if (to_child[0] != -1) close(to_child[0]);
    if (to_child[1] != -1) close(to_child[1]);
    if (from_child[0] != -1) close(from_child[0]);
    if (from_child[1] != -1) close(from_child[1]);

    free(copy);
    free(argv);

    completion_cb(notif->id, NULL, data);
    return;

err_in_parent:
    free(async);
    free(input);
    fdm_del(notif->mgr->fdm, to_child[1]);
    fdm_del(notif->mgr->fdm, from_child[0]);
    completion_cb(notif->id, NULL, data);
    return;
}
