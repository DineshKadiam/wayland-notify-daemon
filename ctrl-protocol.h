#pragma once
#include <stdint.h>

enum ctrl_command {
    CTRL_QUIT,
    CTRL_DISMISS_CURRENT, CTRL_DISMISS_BY_ID, CTRL_DISMISS_ALL
};

struct ctrl_request {
    enum ctrl_command cmd;
    uint32_t id;
} __attribute__((packed));

enum ctrl_result { CTRL_OK, CTRL_FAIL };
struct ctrl_reply {
    enum ctrl_result result;
} __attribute__((packed));
