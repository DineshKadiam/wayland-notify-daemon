#pragma once

struct ctrl;
struct ctrl *ctrl_init(void);
void ctrl_destroy(struct ctrl *ctrl);

int ctrl_poll_fd(const struct ctrl *ctrl);
