#pragma once

#include <stdbool.h>

struct dbus;

struct dbus *dbus_init(void);
void dbus_destroy(struct dbus *bus);

int dbus_poll_fd(const struct dbus *bus);
bool dbus_dispatch(struct dbus *bus);
