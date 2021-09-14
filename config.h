#pragma once

#include <stdbool.h>
#include <pixman.h>

struct urgency_config {
    pixman_color_t bg;

    struct {
        pixman_color_t color;
        char *font;
        int size;
    } border;

    struct {
        char *font;
        pixman_color_t color;
    } app;

    struct {
        char *font;
        pixman_color_t color;
    } summary;

    struct {
        char *font;
        pixman_color_t color;
    } body;

    struct {
        char *font;
        pixman_color_t color;
    } action;

    int timeout_secs;
    int default_timeout_secs;
    char *sound_file;  /* Path to user-configured sound to play on notification */
};

struct config_spawn_template {
    char *raw_cmd;
    char **argv;
};

struct config {
    char *output;
    int min_width;
    int max_width;
    int max_height;

    char *icon_theme_name;
    int max_icon_size;

    enum {
        STACK_BOTTOM_UP,
        STACK_TOP_DOWN,
    } stacking_order;

    enum {
        ANCHOR_TOP_LEFT,
        ANCHOR_TOP_RIGHT,
        ANCHOR_BOTTOM_LEFT,
        ANCHOR_BOTTOM_RIGHT,
    } anchor;

    struct {
        int vertical;
        int horizontal;
        int between;
    } margins;

    struct urgency_config by_urgency[3];

    char *selection_helper;
    struct config_spawn_template play_sound;
};

bool config_load(struct config *conf, const char *path);
void config_destroy(struct config conf);
