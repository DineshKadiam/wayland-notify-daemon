#pragma once

#include <stdbool.h>

struct ctrl;
struct ctrl *ctrl_init(void);
void ctrl_destroy(struct ctrl *ctrl);

int ctrl_poll_fd(const struct ctrl *ctrl);
bool ctrl_dispatch(struct ctrl *ctrl);
