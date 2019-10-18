#include "dbus.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <sys/epoll.h>

#include <dbus/dbus.h>

#define LOG_MODULE "dbus"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "notification.h"
#include "icon.h"

struct dbus {
    DBusConnection *conn;
    const struct config *conf;
    struct fdm *fdm;
    struct wayland *wayl;
    struct notif_mgr *notif_mgr;
    const icon_theme_list_t *icon_theme;
    int bus_fd;
};

bool
get_server_information(struct dbus *bus, DBusMessage *msg)
{
    LOG_DBG("get_server_information");

    bool ret = false;

    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (reply == NULL)
        return false;

    DBusMessageIter iter;
    dbus_message_iter_init_append(reply, &iter);

    if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &(const char *){"fnott"}) ||
        !dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &(const char *){"dnkl"}) ||
        !dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &(const char *){FNOTT_VERSION}) ||
        !dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &(const char *){"1.2"}))
    {
        goto err;
    }

    if (!dbus_connection_send(bus->conn, reply, NULL))
        goto err;

    ret = true;

err:
    dbus_message_unref(reply);
    return ret;
}

static bool
get_capabilities(struct dbus *bus, DBusMessage *msg)
{
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (reply == NULL)
        return false;

    bool ret = false;
    DBusMessageIter iter, arr;
    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING_AS_STRING, &arr);

    dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &(const char *){"body"});
    dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &(const char *){"body-markup"});
    dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &(const char *){"actions"});
#if 0
    dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &(const char *){"icon-static"});

    dbus_message_iter_close_container(&iter, &arr);
    if (!dbus_connection_send(bus->conn, reply, NULL))
        goto err;

    ret = true;

err:
    dbus_message_unref(reply);
    return ret;
}

static bool
notify(struct dbus *bus, DBusMessage *msg)
{
    DBusError dbus_error;
    dbus_error_init(&dbus_error);

    DBusMessageIter args_iter;
    dbus_message_iter_init(msg, &args_iter);

    pixman_image_t *pix = NULL;

    uint32_t replaces_id;
    char *app_name, *app_icon, *summary, *body;
    if (!dbus_message_get_args(
            msg, &dbus_error,
            DBUS_TYPE_STRING, &app_name,
            DBUS_TYPE_UINT32, &replaces_id,
            DBUS_TYPE_STRING, &app_icon,
            DBUS_TYPE_STRING, &summary,
            DBUS_TYPE_STRING, &body,
            DBUS_TYPE_INVALID))
    {
        return false;
    }

    size_t len = strlen(app_name);
    while (len > 0 && isspace(app_name[len - 1]))
        app_name[--len] = '\0';

    len = strlen(summary);
    while (len > 0 && isspace(summary[len - 1]))
        summary[--len] = '\0';

    len = strlen(body);
    while (len > 0 && isspace(body[len - 1]))
        body[--len] = '\0';

    if (dbus_error_is_set(&dbus_error)) {
        LOG_ERR("Notify: failed to parse arguments: %s", dbus_error.message);
        dbus_error_free(&dbus_error);
        return false;
    }

    bool ret = false;
    LOG_DBG("app: %s, icon: %s, summary: %s, body: %s", app_name, app_icon, summary, body);

    {
        char *app_name_locase = NULL;
        const char *icon_name = NULL;

        if (strlen(app_icon) > 0)
            icon_name = app_icon;
        else {
            app_name_locase = malloc(strlen(app_name) + 1);
            for (size_t i = 0; i < strlen(app_name); i++)
                app_name_locase[i] = tolower(app_name[i]);
            app_name_locase[strlen(app_name)] = '\0';
            icon_name = app_name_locase;
        }

        pix = icon_load(
            icon_name,
            bus->conf->max_icon_size, *bus->icon_theme);

        free(app_name_locase);
    }

    dbus_message_iter_next(&args_iter);
    dbus_message_iter_next(&args_iter);
    dbus_message_iter_next(&args_iter);
    dbus_message_iter_next(&args_iter);
    dbus_message_iter_next(&args_iter);

    assert(dbus_message_iter_get_arg_type(&args_iter) == DBUS_TYPE_ARRAY);

    DBusMessageIter actions_iter;
    dbus_message_iter_recurse(&args_iter, &actions_iter);

    struct action {
        const char *id;
        const char *label;
    };

    tll(struct action) actions = tll_init();
    while (dbus_message_iter_get_arg_type(&actions_iter) != DBUS_TYPE_INVALID) {
        const char *id;
        const char *label;
        dbus_message_iter_get_basic(&actions_iter, &id);
        dbus_message_iter_next(&actions_iter);
        dbus_message_iter_get_basic(&actions_iter, &label);
        dbus_message_iter_next(&actions_iter);
        LOG_DBG("action: %s %s", id, label);
        tll_push_back(actions, ((struct action){id, label}));
    }
    dbus_message_iter_next(&args_iter);

    DBusMessageIter hints_iter;
    dbus_message_iter_recurse(&args_iter, &hints_iter);

    enum urgency urgency = URGENCY_NORMAL;

    while (dbus_message_iter_get_arg_type(&hints_iter) != DBUS_TYPE_INVALID) {
        DBusMessageIter entry_iter;
        dbus_message_iter_recurse(&hints_iter, &entry_iter);
        dbus_message_iter_next(&hints_iter);

        const char *name;
        dbus_message_iter_get_basic(&entry_iter, &name);
        dbus_message_iter_next(&entry_iter);

        DBusMessageIter value_iter;
        dbus_message_iter_recurse(&entry_iter, &value_iter);
        dbus_message_iter_next(&entry_iter);

        LOG_DBG("hint: %s", name);

        if (strcmp(name, "urgency") == 0) {
            /* low=0, normal=1, critical=2 */
            uint8_t level;
            dbus_message_iter_get_basic(&value_iter, &level);
            urgency = level;
        }

        else if (strcmp(name, "image-data") == 0 ||
                 strcmp(name, "image_data") == 0 ||
                 strcmp(name, "icon_data") == 0)
        {
            DBusMessageIter img_iter;
            dbus_message_iter_recurse(&value_iter, &img_iter);

            bool has_alpha;
            uint32_t width, height, stride, bpp, channels;
            dbus_message_iter_get_basic(&img_iter, &width); dbus_message_iter_next(&img_iter);
            dbus_message_iter_get_basic(&img_iter, &height); dbus_message_iter_next(&img_iter);
            dbus_message_iter_get_basic(&img_iter, &stride); dbus_message_iter_next(&img_iter);
            dbus_message_iter_get_basic(&img_iter, &has_alpha); dbus_message_iter_next(&img_iter);
            dbus_message_iter_get_basic(&img_iter, &bpp); dbus_message_iter_next(&img_iter);
            dbus_message_iter_get_basic(&img_iter, &channels); dbus_message_iter_next(&img_iter);

            LOG_DBG("image: width=%u, height=%u, stride=%u, has-alpha=%d, bpp=%u, channels=%u",
                    width, height, stride, has_alpha, bpp, channels);

            assert(dbus_message_iter_get_arg_type(&img_iter) == DBUS_TYPE_ARRAY);

            if (width * channels * bpp / 8 > stride)
                LOG_WARN("image width exceeds image stride");

            size_t image_size = stride * height;
            uint8_t *image_data = malloc(image_size);

            DBusMessageIter data_iter;
            dbus_message_iter_recurse(&img_iter, &data_iter);
            for (size_t i = 0; i < image_size; i++)
            {
                if (dbus_message_iter_get_arg_type(&data_iter) == DBUS_TYPE_INVALID) {
                    LOG_WARN("image data truncated");
                    break;
                }

                dbus_message_iter_get_basic(&data_iter, &image_data[i]);
                dbus_message_iter_next(&data_iter);
            }

            if (dbus_message_iter_get_arg_type(&data_iter) != DBUS_TYPE_INVALID)
                LOG_WARN("image data exceeds specified size");

            pixman_format_code_t format = 0;
            if (bpp == 8 && channels == 4)
                format = has_alpha ? PIXMAN_a8b8g8r8 : PIXMAN_x8b8g8r8;
            else if (bpp == 8 && channels == 3) {
                /* Untested */
                format = PIXMAN_b8g8r8;
            } else {
                LOG_WARN("unimplemented image format: bpp=%u, channels=%u",
                         bpp, channels);
                free(image_data);
            }

            if (format != 0) {
                if (pix != NULL) {
                    free(pixman_image_get_data(pix));
                    pixman_image_unref(pix);
                    pix = NULL;
                }

                pix = pixman_image_create_bits_no_clear(
                    format, width, height, (uint32_t *)image_data, stride);
            }
        }
    }

    dbus_message_iter_next(&args_iter);

    /* -1 - up to server (us), 0 - never expire */
    int32_t timeout_ms;
    dbus_message_iter_get_basic(&args_iter, &timeout_ms);
    LOG_DBG("timeout = %dms", timeout_ms);

    struct notif *notif = notif_mgr_create_notif(bus->notif_mgr, replaces_id);
    notif_set_application(notif, app_name);
    notif_set_summary(notif, summary);
    notif_set_body(notif, body);
    notif_set_urgency(notif, urgency);

    if (pix != NULL)
        notif_set_image(notif, pix);

    tll_foreach(actions, it)
        notif_add_action(notif, it->item.id, it->item.label);

    notif_mgr_refresh(bus->notif_mgr);

    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (reply == NULL)
        goto err;

    DBusMessageIter iter;
    dbus_message_iter_init_append(reply, &iter);

    if (!dbus_message_iter_append_basic(
            &iter, DBUS_TYPE_UINT32, &(uint32_t){notif_id(notif)}))
        goto err;

    if (!dbus_connection_send(bus->conn, reply, NULL))
        goto err;

    ret = true;
err:
    tll_free(actions);
    dbus_message_unref(reply);
    return ret;
}

static bool
close_notification(struct dbus *bus, DBusMessage *msg)
{
    DBusError dbus_error;
    dbus_error_init(&dbus_error);

    uint32_t id;
    if (!dbus_message_get_args(
            msg, &dbus_error,
            DBUS_TYPE_UINT32, &id,
            DBUS_TYPE_INVALID))
    {
        return false;
    }

    if (dbus_error_is_set(&dbus_error)) {
        LOG_ERR("CloseNotification: failed to parse arguments: %s", dbus_error.message);
        dbus_error_free(&dbus_error);
        return false;
    }

    LOG_DBG("CloseNotification: id=%u", id);
    bool success  = notif_mgr_del_notif(bus->notif_mgr, id);

    if (success) {
        notif_mgr_refresh(bus->notif_mgr);
        dbus_signal_closed(bus, id);
    }

    bool ret = false;
    DBusMessage *reply = success
        ? dbus_message_new_method_return(msg)
        : dbus_message_new_error(msg, DBUS_ERROR_FAILED, "invalid notification ID");

    if (reply == NULL)
        goto err;

    if (!dbus_connection_send(bus->conn, reply, NULL))
        goto err;

    ret = true;

err:
    dbus_message_unref(reply);
    return ret;
}

static DBusHandlerResult
dbus_handler(DBusConnection *conn, DBusMessage *msg, void *data)
{
    struct dbus *bus = data;

    const char *iface __attribute__((unused)) = dbus_message_get_interface(msg);
    const char *member = dbus_message_get_member(msg);
    LOG_DBG("%s:%s", iface, member);

    static const struct {
        const char *name;
        bool (*handler)(struct dbus *bus, DBusMessage *msg);
    } handlers[] = {
        {"GetServerInformation", &get_server_information},
        {"GetCapabilities", &get_capabilities},
        {"Notify", &notify},
        {"CloseNotification", &close_notification},
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        if (strcmp(handlers[i].name, member) != 0)
            continue;

        return handlers[i].handler(bus, msg) ?
            DBUS_HANDLER_RESULT_HANDLED:
            DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static bool
signal_notification_closed(struct dbus *bus, uint32_t id, uint32_t reason)
{
    DBusMessage *signal = dbus_message_new_signal(
        "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications",
        "NotificationClosed");

    if (signal == NULL)
        return false;

    DBusMessageIter iter;
    dbus_message_iter_init_append(signal, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &id);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_UINT32, &reason);
    dbus_connection_send(bus->conn, signal, NULL);
    dbus_message_unref(signal);
    return true;
}

bool
dbus_signal_expired(struct dbus *bus, uint32_t id)
{
    return signal_notification_closed(bus, id, 0);
}

bool
dbus_signal_dismissed(struct dbus *bus, uint32_t id)
{
    return signal_notification_closed(bus, id, 1);
}

bool
dbus_signal_closed(struct dbus *bus, uint32_t id)
{
    return signal_notification_closed(bus, id, 2);
}

static bool
fdm_handler(struct fdm *fdm, int fd, int events, void *data)
{
    bool ret = false;
    struct dbus *bus = data;

    if (!dbus_connection_read_write(bus->conn, 0)) {
        LOG_WARN("failed to read/write dbus connection");
        goto err;
    }

    while (dbus_connection_dispatch(bus->conn) != DBUS_DISPATCH_COMPLETE)
        ;

    ret = true;

err:
    if (events & EPOLLHUP) {
        LOG_INFO("disconnected from Wayland");
        return false;
    }

    return ret;
}

struct dbus *
dbus_init(const struct config *conf, struct fdm *fdm, struct wayland *wayl,
          struct notif_mgr *notif_mgr, const icon_theme_list_t *icon_theme)
{
    struct dbus *bus = NULL;

    DBusError dbus_error;
    dbus_error_init(&dbus_error);

    DBusConnection *conn;
    conn = dbus_bus_get(DBUS_BUS_SESSION, &dbus_error);
    if (dbus_error_is_set(&dbus_error)) {
        LOG_ERR("failed to connect to D-Bus session bus: %s", dbus_error.message);
        dbus_error_free(&dbus_error);
    }

    if (conn == NULL)
        return NULL;

    int ret = dbus_bus_request_name(
        conn, "org.freedesktop.Notifications", DBUS_NAME_FLAG_DO_NOT_QUEUE,
        &dbus_error);

    if (dbus_error_is_set(&dbus_error)) {
        LOG_ERR("failed to acquire service name: %s", dbus_error.message);
        dbus_error_free(&dbus_error);
        goto err;
    }

    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        LOG_ERR(
            "failed to acquire service name: not primary owner, ret = %d", ret);
        if (ret == DBUS_REQUEST_NAME_REPLY_EXISTS)
            LOG_ERR("is a notification daemon already running?");
        goto err;
    }

    bus = malloc(sizeof(*bus));
    *bus = (struct dbus) {
        .conn = conn,
        .conf = conf,
        .fdm = fdm,
        .wayl = wayl,
        .notif_mgr = notif_mgr,
        .icon_theme = icon_theme,
        .bus_fd = -1,  /* TODO: use watches */
    };

    static const DBusObjectPathVTable handler = {
        .message_function = &dbus_handler,
    };

    if (!dbus_connection_register_object_path(
            conn, "/org/freedesktop/Notifications", &handler, bus)) {
        LOG_ERR("failed to register vtable");
        goto err;
    }

    /* TODO: use watches */
    if (!dbus_connection_get_unix_fd(conn, &bus->bus_fd)) {
        if (!dbus_connection_get_socket(conn, &bus->bus_fd)) {
            LOG_ERR("failed to get socket or UNIX FD");
            goto err;
        }
    }

    assert(bus->bus_fd != -1);
    if (!fdm_add(fdm, bus->bus_fd, fdm_handler, bus)) {
        LOG_ERR("failed to register with FDM");
        goto err;
    }

    return bus;

err:
    dbus_connection_close(conn);
    if (bus != NULL)
        free(bus);
    return NULL;
}

void
dbus_destroy(struct dbus *bus)
{
    if (bus == NULL)
        return;

    fdm_del(bus->fdm, bus->bus_fd);
    dbus_connection_close(bus->conn);
    free(bus);
}

int
dbus_poll_fd(const struct dbus *bus)
{
    /* TODO: use watches */
    int fd = -1;
    if (!dbus_connection_get_unix_fd(bus->conn, &fd)) {
        if (!dbus_connection_get_socket(bus->conn, &fd)) {
            LOG_ERR("failed to get socket or UNIX FD");
            return -1;
        }
    }

    assert(fd != -1);
    return fd;
}
