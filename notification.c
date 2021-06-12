#include "notification.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include <wctype.h>
#include <regex.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <fcntl.h>

#include <pixman.h>
#include <wayland-client.h>
#include <wlr-layer-shell-unstable-v1.h>

#define LOG_MODULE "notification"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "dbus.h"
#include "wayland.h"

#define max(x, y) ((x) > (y) ? (x) : (y))
#define min(x, y) ((x) < (y) ? (x) : (y))

#ifndef CLOCK_BOOTTIME
#ifdef CLOCK_UPTIME
/* DragonFly and FreeBSD */
#define CLOCK_BOOTTIME CLOCK_UPTIME
#else
#define CLOCK_BOOTTIME CLOCK_MONOTONIC
#endif
#endif

struct notif_mgr;

struct font_set {
    struct fcft_font *regular;
    struct fcft_font *bold;
    struct fcft_font *italic;
    struct fcft_font *bold_italic;
};

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
    bool is_configured;

    uint32_t id;
    enum urgency urgency;

    wchar_t *app;
    wchar_t *summary;
    wchar_t *body;
    tll(struct action) actions;

    int timeout_fd;

    struct {
        float dpi;
        enum urgency urgency;
        struct font_set app;
        struct font_set summary;
        struct font_set body;
        struct font_set action;
    } fonts;

    pixman_image_t *pix;
    int image_width;
    int image_height;
    int scale;

    struct buffer *pending;
    struct wl_callback *frame_callback;

    int y;

    const struct monitor *mon;
};

struct notif_mgr {
    struct config *conf;
    struct fdm *fdm;
    struct wayland *wayl;
    struct dbus *bus;
    regex_t html_entity_re;

    tll(struct notif *) notifs;
};

static size_t next_id = 1;

struct notif_mgr *
notif_mgr_new(struct config *conf, struct fdm *fdm)
{
    struct notif_mgr *mgr = malloc(sizeof(*mgr));
    *mgr = (struct notif_mgr) {
        .conf = conf,
        .fdm = fdm,
        .wayl = NULL,   /* notif_mgr_configure() */
        .bus = NULL,    /* notif_mgr_configure() */
        .notifs = tll_init(),
    };

    int r = regcomp(
        &mgr->html_entity_re,
        /* Entity names (there's a *lot* of these - we only support the common ones */
        "&\\(nbsp\\|lt\\|gt\\|amp\\|quot\\|apos\\|cent\\|pound\\|yen\\|euro\\|copy\\|reg\\);\\|"

        /* Decimal entity number: &#39; */
        "&#\\([0-9]\\+\\);\\|"

        /* Hexadecimal entity number: &#x27; */
        "&#x\\([0-9a-fA-F]\\+\\);" , 0);

    if (r != 0) {
        char err[1024];
        regerror(r, &mgr->html_entity_re, err, sizeof(err));

        LOG_ERR("failed to compile HTML entity regex: %s (%d)", err, r);
        regfree(&mgr->html_entity_re);
        free(mgr);
        return NULL;
    }

    return mgr;
}

void
notif_mgr_destroy(struct notif_mgr *mgr)
{
    if (mgr == NULL)
        return;

    regfree(&mgr->html_entity_re);

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

static bool notif_reload_fonts(struct notif *notif);
static int notif_show(struct notif *notif, int y);

static void
surface_enter(void *data, struct wl_surface *wl_surface,
              struct wl_output *wl_output)
{
    struct notif *notif = data;

    const struct monitor *mon = wayl_monitor_get(notif->mgr->wayl, wl_output);
    if (notif->mon == mon)
        return;

    notif->mon = mon;
    notif_reload_fonts(notif);

    int new_scale = mon != NULL
        ? mon->scale : wayl_guess_scale(notif->mgr->wayl);

    if (notif->scale == new_scale)
        return;

    notif->scale = new_scale;
    notif_show(notif, notif->y);
}

static void
surface_leave(void *data, struct wl_surface *wl_surface,
              struct wl_output *wl_output)
{
    struct notif *notif = data;
    notif->mon = NULL;
}

static const struct wl_surface_listener surface_listener = {
    .enter = &surface_enter,
    .leave = &surface_leave,
};

static void
layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                        uint32_t serial, uint32_t w, uint32_t h)
{
    LOG_DBG("configure: width=%u, height=%u", w, h);
    struct notif *notif = data;
    notif->is_configured = true;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
}

static void
layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    struct notif *notif = data;
    struct notif_mgr *mgr = notif->mgr;
    notif_mgr_dismiss_id(mgr, notif->id);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = &layer_surface_configure,
    .closed = &layer_surface_closed,
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
notif_mgr_get_notif_for_surface(struct notif_mgr *mgr,
                                const struct wl_surface *surface)
{
    tll_foreach(mgr->notifs, it) {
        if (it->item->surface == surface)
            return it->item;
    }

    return NULL;
}

/* Instantiates a new notification. You *must* call
 * notif_mgr_refresh() "soon" (after configuring the notification). */
struct notif *
notif_mgr_create_notif(struct notif_mgr *mgr, uint32_t replaces_id)
{
    if (replaces_id != 0) {
        struct notif *old_notif = notif_mgr_get_notif(mgr, replaces_id);
        if (old_notif != NULL)
            return old_notif;
    }

    struct wayland *wayl = mgr->wayl;
    const struct monitor *mon = wayl_preferred_monitor(wayl);

    struct wl_surface *surface = wl_compositor_create_surface(wayl_compositor(wayl));
    if (surface == NULL) {
        LOG_ERR("failed to create wayland surface");
        return NULL;
    }

    struct zwlr_layer_surface_v1 *layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        wayl_layer_shell(wayl), surface, mon != NULL ? mon->output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP, "notification-daemon");

    if (layer_surface == NULL) {
        LOG_ERR("failed to create layer shell surface");
        wl_surface_destroy(surface);
        return NULL;
    }

    const struct config *conf = mgr->conf;
    enum zwlr_layer_surface_v1_anchor anchor
        = (conf->anchor == ANCHOR_TOP_LEFT || conf->anchor == ANCHOR_TOP_RIGHT
           ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
           : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM)
        | (conf->anchor == ANCHOR_TOP_LEFT || conf->anchor == ANCHOR_BOTTOM_LEFT
           ? ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
           : ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);

    zwlr_layer_surface_v1_set_anchor(layer_surface, anchor);
    zwlr_layer_surface_v1_set_size(layer_surface, 1, 1);

    struct notif *notif = malloc(sizeof(*notif));
    *notif = (struct notif) {
        .mgr = mgr,
        .surface = surface,
        .layer_surface = layer_surface,
        .id = next_id++,
        .urgency = URGENCY_NORMAL,
        .actions = tll_init(),
        .timeout_fd = -1,
        .mon = mon,
        .scale = mon != NULL ? mon->scale : wayl_guess_scale(wayl),
    };

    notif_reload_fonts(notif);

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

    wl_surface_add_listener(surface, &surface_listener, notif);

    zwlr_layer_surface_v1_add_listener(
        layer_surface, &layer_surface_listener, notif);
    wl_surface_commit(notif->surface);

    /*
     * Ensure the surface has been configured before we
     * continue.
     *
     * Without this, a subsequent call to notif_show() (either
     * directly, or via notif_mgr_refresh()) will result in
     * "zwlr_layer_surface_v1@15: error 2: layer_surface has never
     * been configured".
     */
    wayl_roundtrip(mgr->wayl);
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

    fdm_del(notif->mgr->fdm, notif->timeout_fd);

    if (notif->frame_callback != NULL)
        wl_callback_destroy(notif->frame_callback);

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

static float
get_dpi(const struct notif *notif)
{
    if (notif->mon != NULL)
        return notif->mon->dpi > 0 ? notif->mon->dpi : 96.;
    else
        return wayl_dpi_guess(notif->mgr->wayl);
}

static void
font_set_destroy(struct font_set *set)
{
    fcft_destroy(set->regular);
    fcft_destroy(set->bold);
    fcft_destroy(set->italic);
    fcft_destroy(set->bold_italic);

    set->regular = set->bold = set->italic = set->bold_italic = NULL;
}

static bool
reload_one_font_set(const struct notif *notif, const char *font_name,
                    struct font_set *set, float dpi)
{
    char attrs0[256], attrs1[256], attrs2[256], attrs3[256];
    snprintf(attrs0, sizeof(attrs0), "dpi=%.2f", dpi);
    snprintf(attrs1, sizeof(attrs1), "dpi=%.2f:weight=bold", dpi);
    snprintf(attrs2, sizeof(attrs2), "dpi=%.2f:slant=italic", dpi);
    snprintf(attrs3, sizeof(attrs3), "dpi=%.2f:weight=bold:slant=italic", dpi);

    const char *names[1] = {font_name};

    struct fcft_font *regular = fcft_from_name(1, names, attrs0);
    if (regular == NULL) {
        LOG_ERR("%s: failed to load font", font_name);
        return false;
    }

    struct fcft_font *bold = fcft_from_name(1, names, attrs1);
    struct fcft_font *italic = fcft_from_name(1, names, attrs2);
    struct fcft_font *bold_italic = fcft_from_name(1, names, attrs3);

    set->regular = regular;
    set->bold = bold;
    set->italic = italic;
    set->bold_italic = bold_italic;
    return true;
}

static bool
notif_reload_fonts(struct notif *notif)
{
    float dpi = get_dpi(notif);

    if (notif->fonts.app.regular != NULL &&
        notif->fonts.dpi == dpi &&
        notif->fonts.urgency == notif->urgency)
    {
        return true;
    }

    const struct urgency_config *urgency
        = &notif->mgr->conf->by_urgency[notif->urgency];

    struct font_set app;
    struct font_set summary;
    struct font_set body;
    struct font_set action;

    if (!reload_one_font_set(notif, urgency->app.font, &app, dpi) ||
        !reload_one_font_set(notif, urgency->summary.font, &summary, dpi) ||
        !reload_one_font_set(notif, urgency->body.font, &body, dpi) ||
        !reload_one_font_set(notif, urgency->action.font, &action, dpi))
    {
        return false;
    }

    font_set_destroy(&notif->fonts.app);
    font_set_destroy(&notif->fonts.summary);
    font_set_destroy(&notif->fonts.body);
    font_set_destroy(&notif->fonts.action);

    notif->fonts.dpi = dpi;
    notif->fonts.urgency = notif->urgency;
    notif->fonts.app = app;
    notif->fonts.summary = summary;
    notif->fonts.body = body;
    notif->fonts.action = action;

    return true;
}

uint32_t
notif_id(const struct notif *notif)
{
    return notif->id;
}

const struct monitor *
notif_monitor(const struct notif *notif)
{
    return notif->mon;
}

static wchar_t *
decode_html_entities(const struct notif_mgr *mgr, const char *s)
{
    /* Guesstimate initial size */
    size_t sz = strlen(s) + 1;
    wchar_t *result = malloc(sz * sizeof(wchar_t));

    /* Output so far */
    size_t len = 0;
    wchar_t *out = result;

#define ensure_size(new_size)                               \
    do {                                                    \
        while (sz < (new_size)) {                           \
            sz *= 2;                                        \
            result = realloc(result, sz * sizeof(wchar_t)); \
            out = &result[len];                             \
        }                                                   \
    } while (0)

#define append(wc)                                           \
    do {                                                     \
        ensure_size(len + 1);                                \
        *out++ = wc;                                         \
        len++;                                               \
    } while (0)

#define append_u8(s, s_len)                                      \
    do {                                                         \
        const char *_s = (s);                                    \
        mbstate_t _ps = {0};                                     \
        size_t _w_len = mbsnrtowcs(NULL, &_s, s_len, 0, &_ps);   \
        if (_w_len > 0) {                                        \
            ensure_size(len + _w_len + 1);                       \
            memset(&_ps, 0, sizeof(_ps));                        \
            _s = (s);                                            \
            mbsnrtowcs(out, &_s, s_len, _w_len + 1, &_ps);       \
            out += _w_len;                                       \
            len += _w_len;                                       \
        }                                                        \
    } while (0)

    while (true) {
        regmatch_t matches[mgr->html_entity_re.re_nsub + 1];
        if (regexec(&mgr->html_entity_re, s, mgr->html_entity_re.re_nsub + 1, matches, 0) == REG_NOMATCH) {
            append_u8(s, strlen(s));
            break;
        }

        const regmatch_t *all = &matches[0];
        const regmatch_t *named = &matches[1];
        const regmatch_t *decimal = &matches[2];
        const regmatch_t *hex = &matches[3];

        append_u8(s, all->rm_so);

        if (named->rm_so >= 0) {
            size_t match_len = named->rm_eo - named->rm_so;
            const char *match = &s[named->rm_so];

            if (strncmp(match, "nbsp", match_len) == 0)       append(L' ');
            else if (strncmp(match, "lt", match_len) == 0)    append(L'<');
            else if (strncmp(match, "gt", match_len) == 0)    append(L'>');
            else if (strncmp(match, "amp", match_len) == 0)   append(L'&');
            else if (strncmp(match, "quot", match_len) == 0)  append(L'"');
            else if (strncmp(match, "apos", match_len) == 0)  append(L'\'');
            else if (strncmp(match, "cent", match_len) == 0)  append(L'¢');
            else if (strncmp(match, "pound", match_len) == 0) append(L'£');
            else if (strncmp(match, "yen", match_len) == 0)   append(L'¥');
            else if (strncmp(match, "euro", match_len) == 0)  append(L'€');
            else if (strncmp(match, "copy", match_len) == 0)  append(L'©');
            else if (strncmp(match, "reg", match_len) == 0)   append(L'®');
            else assert(false);
        }

        else if (decimal->rm_so >= 0 || hex->rm_so >= 0) {
            bool is_hex = hex->rm_so >= 0;
            const char *match = is_hex ? &s[hex->rm_so] : &s[decimal->rm_so];

            /* Convert string to integer */
            errno = 0;

            char *end;
            unsigned long v = strtoul(match, &end, is_hex ? 16 : 10);

            if (errno == 0) {
                assert(*end == ';');
                append((wchar_t)v);
            }
        }

        s += all->rm_eo;
    }

#undef append
#undef append_n
#undef ensure_size

    result[len] = L'\0';
    return result;
}

void
notif_set_application(struct notif *notif, const char *text)
{
    size_t chars = mbstowcs(NULL, text, 0);
    if (chars == (size_t) -1)
        return;

    free(notif->app);
    notif->app = malloc((chars + 1) * sizeof(wchar_t));
    mbstowcs(notif->app, text, chars + 1);
}

void
notif_set_summary(struct notif *notif, const char *text)
{
    notif->summary = decode_html_entities(notif->mgr, text);
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
    notif->body = decode_html_entities(notif->mgr, text);
}

void
notif_set_urgency(struct notif *notif, enum urgency urgency)
{
    if (notif->urgency == urgency)
        return;

    notif->urgency = urgency;
    notif_set_timeout(notif, 0);  /* Start user-configured timeout, if set */
    notif_reload_fonts(notif);

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

static bool
fdm_timeout(struct fdm *fdm, int fd, int events, void *data)
{
    if (events & EPOLLHUP)
        return false;

    struct notif *notif = data;

    uint64_t unused;
    ssize_t ret = read(fd, &unused, sizeof(unused));

    if (ret < 0) {
        if (errno == EAGAIN)
            return true;

        LOG_ERRNO("failed to read notification timeout timer");
        return false;
    }

    notif_mgr_expire_id(notif->mgr, notif->id);
    return true;
}

void
notif_set_timeout(struct notif *notif, int timeout_ms)
{
    /* Delete previous timer, if any */
    if (notif->timeout_fd != -1) {
        fdm_del(notif->mgr->fdm, notif->timeout_fd);
        notif->timeout_fd = -1;
    }

    const int user_timeout_ms =
        notif->mgr->conf->by_urgency[notif->urgency].timeout_secs * 1000;

    /* Use the smallest timeout */
    timeout_ms = user_timeout_ms > 0 && (user_timeout_ms < timeout_ms || timeout_ms == 0)
        ? user_timeout_ms : timeout_ms;

    if (timeout_ms == 0)
        return;

    notif->timeout_fd = timerfd_create(
        CLOCK_BOOTTIME, TFD_CLOEXEC | TFD_NONBLOCK);

    if (notif->timeout_fd == -1) {
        LOG_ERRNO("failed to create notification timeout timer FD");
        return;
    }

    long nsecs = (long)timeout_ms * 1000000;
    time_t secs = nsecs / 1000000000l;
    nsecs %= 1000000000l;

    LOG_DBG("ms=%d: secs=%ld, nsecs=%ld", timeout_ms, secs, nsecs);

    struct itimerspec timeout = {
        .it_value = {.tv_sec = secs, .tv_nsec = nsecs}
    };

    if (timerfd_settime(notif->timeout_fd, 0, &timeout, NULL) < 0) {
        LOG_ERRNO("failed to configure notification timeout timer FD");
        goto fail;
    }

    if (!fdm_add(notif->mgr->fdm, notif->timeout_fd, EPOLLIN,
                 &fdm_timeout, notif))
    {
        LOG_ERR("failed to add notification timeout timer to FDM");
        goto fail;
    }

    return;

fail:
    if (notif->timeout_fd != -1)
        close(notif->timeout_fd);
    notif->timeout_fd = -1;
}

void
notif_add_action(struct notif *notif, const char *id, const char *label)
{

    size_t wid_chars = mbstowcs(NULL, id, 0);
    size_t label_chars = mbstowcs(NULL, label, 0);

    if (wid_chars == (size_t)-1 || label_chars == (size_t)-1)
        return;

    wchar_t *wid = malloc((wid_chars + 1) * sizeof(wchar_t));
    mbstowcs(wid, id, wid_chars + 1);

    wchar_t *wlabel = malloc((label_chars + 1) * sizeof(wchar_t));
    mbstowcs(wlabel, label, label_chars + 1);

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

    assert(notif->scale >= 1);
    assert(buf->busy);

    wl_surface_set_buffer_scale(notif->surface, notif->scale);
    wl_surface_attach(notif->surface, buf->wl_buf, 0, 0);
    wl_surface_damage_buffer(notif->surface, 0, 0, buf->width, buf->height);

    assert(notif->frame_callback == NULL);
    notif->frame_callback = wl_surface_frame(notif->surface);
    wl_callback_add_listener(notif->frame_callback, &frame_listener, notif);

    wl_surface_commit(notif->surface);
    wayl_flush(wayl);
}

static void
frame_callback(void *data, struct wl_callback *wl_callback, uint32_t callback_data)
{
    struct notif *notif = data;

    LOG_DBG("frame callback");
    assert(notif->frame_callback == wl_callback);
    notif->frame_callback = NULL;
    wl_callback_destroy(wl_callback);

    if (notif->pending != NULL) {
        commit_buffer(notif, notif->pending);
        notif->pending = NULL;
    }
}

struct glyph_run {
    size_t count;
    int *cluster;
    const struct fcft_glyph **glyphs;

    bool underline;
    struct fcft_font *font;

    struct fcft_text_run *run;
};

static struct glyph_run
notify_rasterize_text_run(struct fcft_font *font, enum fcft_subpixel subpixel,
                          size_t len, const wchar_t text[static len], size_t ofs)
{
    struct fcft_text_run *run = fcft_text_run_rasterize(
        font, len, text, subpixel);

    if (run == NULL)
        return (struct glyph_run){0};

    for (size_t i = 0; i < run->count; i++)
        run->cluster[i] += ofs;

    return (struct glyph_run){
        .count = run->count,
        .cluster = run->cluster,
        .glyphs = run->glyphs,
        .font = font,
        .run = run,
    };
}

static struct glyph_run
notify_rasterize_glyphs(struct fcft_font *font, enum fcft_subpixel subpixel,
                        size_t len, const wchar_t text[static len], size_t ofs)
{
    int *cluster = malloc(len * sizeof(cluster[0]));
    const struct fcft_glyph **glyphs = malloc(len * sizeof(glyphs[0]));

    struct glyph_run run = {
        .count = 0,
        .cluster = cluster,
        .glyphs = glyphs,
        .font = font,
        .run = NULL,
    };

    for (size_t i = 0; i < len; i++) {
        const struct fcft_glyph *glyph = fcft_glyph_rasterize(
            font, text[i], subpixel);

        if (glyph == NULL)
            continue;

        cluster[run.count] = ofs + i;
        glyphs[run.count] = glyph;
        run.count++;
    }

    return run;
}

static struct glyph_run
notify_rasterize(struct fcft_font *font, enum fcft_subpixel subpixel,
                 size_t len, const wchar_t text[static len], size_t ofs)
{
    if (len == 0)
        return (struct glyph_run){0};

    return fcft_capabilities() & FCFT_CAPABILITY_TEXT_RUN_SHAPING
        ? notify_rasterize_text_run(font, subpixel, len, text, ofs)
        : notify_rasterize_glyphs(font, subpixel, len, text, ofs);
}

struct glyph_layout {
    const struct fcft_glyph *glyph;
    const pixman_color_t *color;
    int x, y;
    struct {
        bool draw;
        int y;
        int thickness;
    } underline;
    struct fcft_text_run *run;
};

typedef tll(struct glyph_layout) glyph_list_t;

static void
notif_layout(struct notif *notif, struct font_set *fonts,
             const pixman_color_t *color, enum fcft_subpixel subpixel,
             const wchar_t *text, int left_pad, int right_pad,
             int y, int *width, int *height, glyph_list_t *glyph_list)
{
    *width = 0;
    *height = 0;

    const struct config *conf = notif->mgr->conf;

    bool bold = false;
    bool italic = false;
    bool underline = false;

    tll(struct glyph_run) runs = tll_init();
    size_t total_glyph_count = 0;

    /* Rasterize whole runs, if possible. Need to look for font
     * formatters since we need to use different fonts for those */
    const wchar_t *_t = text;
    for (const wchar_t *wc = _t; true; wc++) {
        if (!(*wc == L'\0' ||
              wcsncasecmp(wc, L"<b>", 3) == 0 ||
              wcsncasecmp(wc, L"<i>", 3) == 0 ||
              wcsncasecmp(wc, L"<u>", 3) == 0 ||
              wcsncasecmp(wc, L"</b>", 4) == 0 ||
              wcsncasecmp(wc, L"</i>", 4) == 0 ||
              wcsncasecmp(wc, L"</u>", 4) == 0))
        {
            continue;
        }

        /* Select font based on formatters currently enabled */
        struct fcft_font *font = NULL;
        if (bold && italic)
            font = fonts->bold_italic;
        else if (bold)
            font = fonts->bold;
        else if (italic)
            font = fonts->italic;

        if (font == NULL)
            font = fonts->regular;

        size_t len = wc - _t;
        size_t ofs = _t - text;

        struct glyph_run run = notify_rasterize(font, subpixel, len, _t, ofs);
        total_glyph_count += run.count;

        if (run.count > 0) {
            run.underline = underline;
            tll_push_back(runs, run);
        }

        if (*wc == L'\0')
            break;

        /* Update formatter state */
        bool new_value = wc[1] == L'/' ? false : true;
        wchar_t formatter = wc[1] == L'/' ? wc[2] : wc[1];

        if (formatter == L'b' || formatter == L'B')
            bold = new_value;
        if (formatter == L'i' || formatter == L'I')
            italic = new_value;
        if (formatter == L'u' || formatter == L'U')
            underline = new_value;

        _t = wc + (wc[1] == L'/' ? 4 : 3);
    }

    /* Distance from glyph to next word boundary. Note: only the
     * *first* glyph in a word has a non-zero distance */
    int distance[total_glyph_count];

    {
        /* Need flat cluster+glyph arrays for this... */
        int cluster[total_glyph_count];
        const struct fcft_glyph *glyphs[total_glyph_count];

        size_t idx = 0;
        tll_foreach(runs, it) {
            const struct glyph_run *run = &it->item;

            for (size_t i = 0; i < run->count; i++, idx++) {
                cluster[idx] = it->item.cluster[i];
                glyphs[idx] = it->item.glyphs[i];
            }
        }

        /* Loop glyph runs, looking for word boundaries */
        idx = 0;
        tll_foreach(runs, it) {
            const struct glyph_run *run = &it->item;

            for (size_t i = 0; i < run->count; i++, idx++) {
                distance[idx] = 0;

                if (!iswspace(text[run->cluster[i]]))
                    continue;

                /* Calculate distance to *this* space for all
                 * preceding glyphs (up til the previous space) */
                for (ssize_t j = idx - 1, dist = 0; j >= 0; j--) {
                    if (iswspace(text[cluster[j]]))
                        break;

                    if (j == 0 || (j > 0 && iswspace(text[cluster[j - 1]]))) {
                        /*
                         * Store non-zero distance only in first character in a word
                         * This ensures the layouting doesn't produce output like:
                         *
                         * x
                         * x
                         * x
                         * x
                         *
                         * for very long words, that doesn't fit at all on single line.
                         */
                        distance[j] = dist;
                    }

                    dist += glyphs[j]->advance.x;
                }
            }
        }

        /* Calculate distance for the last word */
        for (ssize_t j = total_glyph_count - 1, dist = 0; j >= 0; j--) {
            if (iswspace(text[cluster[j]]))
                break;

            if (j == 0 || (j > 0 && iswspace(text[cluster[j - 1]]))) {
                /* Store non-zero distance only in first character in a word */
                distance[j] = dist;
            }

            dist += glyphs[j]->advance.x;
        }
    }

    int x = left_pad;

    if (conf->min_width != 0)
        *width = conf->min_width;

    /*
     * Finally, lay out the glyphs
     *
     * This is done by looping the glyphs, and inserting a newline
     * whenever a word cannot be fitted in the remaining space.
     */
    size_t idx = 0;
    tll_foreach(runs, it) {
        struct glyph_run *run = &it->item;

        for (size_t i = 0; i < run->count; i++, idx++) {

            const wchar_t wc = text[run->cluster[i]];
            const struct fcft_glyph *glyph = run->glyphs[i];
            struct fcft_font *font = run->font;
            const int dist = distance[idx];

            if ((x > left_pad &&
                 x + glyph->advance.x + dist + right_pad > conf->max_width) ||
                wc == L'\n')
            {
                *width = max(*width, x + right_pad);
                *height += fonts->regular->height;

                x = left_pad;
                y += fonts->regular->height;

                if (iswspace(wc)) {
                    /* Don't render trailing whitespace */
                    continue;
                }
            }

            if (glyph->cols <= 0)
                continue;

            struct glyph_layout layout = {
                .glyph = glyph,
                .color = color,
                .x = x,
                .y = y + font->ascent,
                .underline = {
                    .draw = run->underline,
                    .y = y + font->ascent - font->underline.position,
                    .thickness = font->underline.thickness,
                },
            };

            tll_push_back(*glyph_list, layout);
            x += glyph->advance.x;
        }

        if (run->run == NULL) {
            free(run->cluster);
            free(run->glyphs);
        }

        /* Track text-run in the *last* glyph belonging to this run */
        tll_back(*glyph_list).run = run->run;
        tll_remove(runs, it);
    }

    *width = max(*width, x + right_pad);
    *height += fonts->regular->height;
}

static int
notif_show(struct notif *notif, int y)
{
    struct notif_mgr *mgr = notif->mgr;
    struct config *conf = mgr->conf;
    struct urgency_config *urgency = &conf->by_urgency[notif->urgency];

    struct wayland *wayl = notif->mgr->wayl;
    const struct monitor *mon
        = notif->mon != NULL ? notif->mon : wayl_preferred_monitor(wayl);

    const enum fcft_subpixel subpixel = urgency->bg.alpha == 0xffff
        ? mon != NULL ? (enum fcft_subpixel)mon->subpixel : FCFT_SUBPIXEL_DEFAULT
        : FCFT_SUBPIXEL_NONE;

    const int pad_horizontal = notif->fonts.body.regular->height;
    const int pad_vertical = pad_horizontal;

    int width = 0;
    int height = pad_vertical;
    glyph_list_t glyphs = tll_init();

    int indent = pad_horizontal;
    int _w, _h;

    if (notif->pix != NULL)
        indent += notif->image_width + pad_horizontal;

    /* App */
    notif_layout(notif, &notif->fonts.app, &urgency->app.color, subpixel, notif->app, indent, pad_horizontal, height, &_w, &_h, &glyphs);
    if (tll_length(notif->actions) > 0) {
        /* TODO: better 'action' indicator */
        int _a, _b;
        notif_layout(notif, &notif->fonts.app, &urgency->app.color, subpixel, L"*", _w - pad_horizontal, pad_horizontal, height, &_a, &_b, &glyphs);
    }
    width = max(width, _w);
    height += _h;

    /* Summary */
    notif_layout(notif, &notif->fonts.summary, &urgency->summary.color, subpixel, notif->summary, indent, pad_horizontal, height, &_w, &_h, &glyphs);
    width = max(width, _w);
    height += _h;

    /* Body */
    notif_layout(notif, &notif->fonts.body, &urgency->body.color, subpixel, notif->body, indent, pad_horizontal, height, &_w, &_h, &glyphs);
    width = max(width, _w);
    height += _h;

#if 0
    tll_foreach(notif->actions, it) {
        notif_layout(notif, &notif->fonts.action, &urgency->action.color, subpixel, it->item.label, indent, pad_horizontal, height, &_w, &_h, &glyphs);
        width = max(width, _w);
        height += _h;
    }
#endif

    if (notif->pix != NULL)
        height = max(height, pad_vertical + notif->image_height);
    height += pad_vertical;

    height = min(height, conf->max_height);

    assert(notif->scale >= 1);
    const int scale = notif->scale;

    LOG_DBG("show: y = %d, width = %d, height = %d (scale = %d)",
            y, width, height, scale);

    bool top_anchored
        = conf->anchor == ANCHOR_TOP_LEFT || conf->anchor == ANCHOR_TOP_RIGHT;

    /* Resize and position */
    zwlr_layer_surface_v1_set_size(notif->layer_surface, width / scale, height / scale);
    zwlr_layer_surface_v1_set_margin(
        notif->layer_surface,
        (top_anchored
         ? y
         : conf->margins.vertical) / scale, /* top */
        conf->margins.horizontal / scale,   /* right */
        (!top_anchored
         ? y
         : conf->margins.between) / scale,  /* bottom */
        conf->margins.horizontal / scale);  /* left */

    struct buffer *buf = wayl_get_buffer(wayl, width, height);
    const int brd_sz = urgency->border.size;;

    pixman_region32_t clip;
    pixman_region32_init_rect(&clip, 0, 0, width, height);
    pixman_image_set_clip_region32(buf->pix, &clip);
    pixman_region32_fini(&clip);

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
        const struct fcft_glyph *glyph = it->item.glyph;
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

        if (it->item.underline.draw) {
            pixman_image_fill_rectangles(
                PIXMAN_OP_OVER, buf->pix, it->item.color,
                1, &(pixman_rectangle16_t){
                    it->item.x, it->item.underline.y,
                    glyph->advance.x, it->item.underline.thickness});
        }

        if (it->item.run != NULL)
            fcft_text_run_destroy(it->item.run);
        tll_remove(glyphs, it);
    }

    if (notif->frame_callback != NULL) {
        if (notif->pending != NULL)
            notif->pending->busy = false;
        notif->pending = buf;
    } else {

        assert(notif->pending == NULL);
        commit_buffer(notif, buf);
    }

    notif->y = y;
    return height;
}

void
notif_mgr_refresh(struct notif_mgr *mgr)
{
    int y = mgr->conf->margins.vertical;
    tll_rforeach(mgr->notifs, it)
        y += notif_show(it->item, y) + mgr->conf->margins.between;
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
notif_expire(struct notif *notif)
{
    dbus_signal_expired(notif->mgr->bus, notif->id);
    notif_destroy(notif);
    return true;
}

static bool
notif_mgr_expire_current(struct notif_mgr *mgr)
{
    if (tll_length(mgr->notifs) == 0)
        return false;

    bool ret = notif_expire(tll_pop_front(mgr->notifs));
    notif_mgr_refresh(mgr);
    return ret;
}

bool
notif_mgr_expire_id(struct notif_mgr *mgr, uint32_t id)
{
    if (id == 0)
        return notif_mgr_expire_current(mgr);

    tll_foreach(mgr->notifs, it) {
        if (it->item->id != id)
            continue;

        struct notif *notif = it->item;
        tll_remove(mgr->notifs, it);

        bool ret = notif_expire(notif);
        notif_mgr_refresh(mgr);
        return ret;
    }

    return false;
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

void
notif_mgr_monitor_removed(struct notif_mgr *mgr, const struct monitor *mon)
{
    tll_foreach(mgr->notifs, it) {
        if (it->item->mon == mon)
            it->item->mon = NULL;
    }
}

/* Returns true if the update is a reason to refresh */
bool
notif_mgr_monitor_updated(struct notif_mgr *mgr, const struct monitor *mon)
{
    bool refresh_needed = false;

    tll_foreach(mgr->notifs, it) {
        if (it->item->mon != mon)
            continue;

        if (it->item->scale != mon->scale) {
            it->item->scale = mon->scale;
            notif_reload_fonts(it->item);
            refresh_needed = true;
        }
    }

    return refresh_needed;
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
    char *chosen = async->output;
    size_t chosen_len = async->output_len;

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
                 (int)chosen_len, chosen);
        goto done;
    }

    /* Map returned label to action ID */
    tll_foreach(notif->actions, it) {
        if (strncmp(it->item.label, chosen, chosen_len) == 0) {
            action_id = it->item.id;
            goto done;
        }
    }

    LOG_WARN("could not map chosen action label to action ID: %.*s", (int)chosen_len, chosen);

done:
    completion_cb(notif_id, action_id, cb_data);
    free(chosen);
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

size_t
notif_action_count(const struct notif *notif)
{
    return tll_length(notif->actions);
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
