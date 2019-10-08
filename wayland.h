#pragma once

#include <stdbool.h>

struct wayland;

struct wayland *wayl_init(void);
void wayl_destroy(struct wayland *wayl);

int wayl_poll_fd(const struct wayland *wayl);
bool wayl_dispatch(struct wayland *wayl);
