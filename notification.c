#include "notification.h"

#include <stdlib.h>
#include <stdint.h>

struct notif {
    struct wayland *wayl;
    struct dbus *bus;

    uint32_t id;
};

static size_t next_id = 0;

struct notif *
notif_new(struct wayland *wayl, struct dbus *bus)
{
    struct notif *notif = malloc(sizeof(*notif));
    *notif = (struct notif) {
        .wayl = wayl,
        .bus = bus,
        .id = next_id++,
    };

    return notif;
}

void
notif_destroy(struct notif *notif)
{
    if (notif == NULL)
        return;

    free(notif);
}

uint32_t
notif_id(const struct notif *notif)
{
    return notif->id;
}
