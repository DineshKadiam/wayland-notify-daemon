#pragma once

#include <stdbool.h>

#include "notification.h"
#include "fdm.h"

struct ctrl;
struct ctrl *ctrl_init(struct fdm *fdm, struct notif_mgr *notif_mgr);
void ctrl_destroy(struct ctrl *ctrl);

int ctrl_poll_fd(const struct ctrl *ctrl);
