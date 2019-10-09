#pragma once

#include <stdbool.h>

#include "notification.h"
#include "wayland.h"
#include "fdm.h"

struct dbus;

struct dbus *dbus_init(
    struct fdm *fdm, struct wayland *wayl, struct notif_mgr *notif_mgr);
void dbus_destroy(struct dbus *bus);

bool dbus_signal_expired(struct dbus *bus, uint32_t id);
bool dbus_signal_dismissed(struct dbus *bus, uint32_t id);
bool dbus_signal_closed(struct dbus *bus, uint32_t id);

int dbus_poll_fd(const struct dbus *bus);
