#pragma once

#include <stdbool.h>

#include "notification.h"

struct ctrl;
struct ctrl *ctrl_init(struct notif_mgr *notif_mgr);
void ctrl_destroy(struct ctrl *ctrl);

int ctrl_poll_fd(const struct ctrl *ctrl);
bool ctrl_dispatch(struct ctrl *ctrl);
