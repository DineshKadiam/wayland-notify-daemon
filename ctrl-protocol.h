#pragma once
#include <stdint.h>

enum ctrl_command { CMD_QUIT, CMD_DISMISS_CURRENT, CMD_DISMISS_BY_ID, CMD_DISMISS_ALL};

struct ctrl_request {
    enum ctrl_command cmd;
    uint32_t id;
} __attribute__((packed));

enum ctrl_result { CTRL_OK, CTRL_FAIL };
struct ctrl_reply {
    enum ctrl_result result;
} __attribute__((packed));
