#pragma once

#include <stdint.h>

#include "config.h"
#include "tllist.h"

/* forward declarations, to avoid circular includes */
struct wayland;
struct dbus;

struct notif_mgr;
struct notif_mgr *notif_mgr_new(const struct config *conf);
void notif_mgr_destroy(struct notif_mgr *mgr);

void notif_mgr_configure(
    struct notif_mgr *mgr, struct wayland *wayl, struct dbus *bus);

void notif_mgr_refresh(struct notif_mgr *mgr);

bool notif_mgr_dismiss_current(struct notif_mgr *mgr);
bool notif_mgr_dismiss_id(struct notif_mgr *mgr, uint32_t id);
bool notif_mgr_dismiss_all(struct notif_mgr *mgr);

struct notif;
struct notif *notif_mgr_get_notif(struct notif_mgr *mgr, uint32_t id);
bool notif_mgr_del_notif(struct notif_mgr *mgr, uint32_t id);

void notif_destroy(struct notif *notif);
uint32_t notif_id(const struct notif *notif);

void notif_set_application(struct notif *notif, const char *text);
void notif_set_summary(struct notif *notif, const char *text);
void notif_set_body(struct notif *notif, const char *text);
