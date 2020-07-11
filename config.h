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
};

struct config {
    char *output;
    int max_width;
    int max_height;

    char *icon_theme_name;
    int max_icon_size;

    int margin;

    struct urgency_config by_urgency[3];

    char *selection_helper;
};

bool config_load(struct config *conf);
void config_destroy(struct config conf);
