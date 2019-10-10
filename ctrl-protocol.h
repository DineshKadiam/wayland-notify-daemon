#pragma once
#include <stdint.h>

enum command_type { CMD_DISMISS_CURRENT, CMD_DISMISS_BY_ID, CMD_DISMISS_ALL};

struct command {
    enum command_type cmd;
    uint32_t id;
} __attribute__((packed));
