#pragma once

#include <stdint.h>
#include "tllist.h"

/* forward declarations, to avoid circular includes */
struct wayland;
struct dbus;

struct notif_mgr;
struct notif_mgr *notif_mgr_new(void);
void notif_mgr_destroy(struct notif_mgr *mgr);

void notif_mgr_configure(
    struct notif_mgr *mgr, struct wayland *wayl, struct dbus *bus);

void notif_mgr_refresh(struct notif_mgr *mgr);

struct notif;
struct notif *notif_mgr_get_notif(struct notif_mgr *mgr, uint32_t id);

void notif_destroy(struct notif *notif);
uint32_t notif_id(const struct notif *notif);
//void notif_show(struct notif *notif);
