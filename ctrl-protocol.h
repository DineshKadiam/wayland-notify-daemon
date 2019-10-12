#pragma once
#include <stdint.h>

enum command_type {
    CMD_QUIT,
    CMD_DISMISS_CURRENT, CMD_DISMISS_BY_ID, CMD_DISMISS_ALL,
    CMD_GET_ACTIONS_CURRENT, CMD_GET_ACTIONS_BY_ID,
};

struct command {
    enum command_type cmd;
    uint32_t id;
} __attribute__((packed));

struct get_action_reply_action {
    uint32_t id_length;
    uint32_t label_length;
    char data[];
} __attribute__((packed));

struct get_action_reply {
    uint32_t count;
    struct get_action_reply_action actions[];
} __attribute__((packed));
