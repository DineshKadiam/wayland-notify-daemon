#pragma once

#include <stdbool.h>

#include "notification.h"
#include "wayland.h"

struct dbus;

struct dbus *dbus_init(struct wayland *wayl, notif_list_t *notifs);
void dbus_destroy(struct dbus *bus);

int dbus_poll_fd(const struct dbus *bus);
bool dbus_dispatch(struct dbus *bus);
