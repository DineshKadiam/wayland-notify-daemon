#pragma once

#include <pixman.h>

#include "font.h"

struct font_set {
    struct font *regular;
    struct font *bold;
    struct font *italic;
    struct font *bold_italic;
};

struct urgency_config {
    pixman_color_t bg;

    struct {
        pixman_color_t color;
        int size;
    } border;

    struct {
        struct font_set fonts;
        pixman_color_t color;
    } app;

    struct {
        struct font_set fonts;
        pixman_color_t color;
    } summary;

    struct {
        struct font_set fonts;
        pixman_color_t color;
    } body;

    struct {
        struct font_set fonts;
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
