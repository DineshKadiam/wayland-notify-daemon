#pragma once

#include <stdint.h>
#include "tllist.h"

/* forward declarations, to avoid circular includes */
struct wayland;
struct dbus;

struct notif;
typedef tll(struct notif *) notif_list_t;

struct notif *notif_new(struct wayland *wayl, struct dbus *bus);
void notif_destroy(struct notif *notif);

uint32_t notif_id(const struct notif *notif);
