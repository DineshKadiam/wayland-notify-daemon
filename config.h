#pragma once

#include <pixman.h>

#include "font.h"

struct urgency_config {
    pixman_color_t bg;

    struct {
        pixman_color_t color;
        int size;
    } border;

    struct {
        struct font *font;
        pixman_color_t color;
    } app;

    struct {
        struct font *font;
        pixman_color_t color;
    } summary;

    struct {
        struct font *font;
        pixman_color_t color;
    } body;

    struct {
        struct font *font;
        pixman_color_t color;
    } action;
};

struct config {
    int max_width;
    int max_height;

    struct {
        int top;
        int right;
        int bottom;
        int left;
    } margin;

    struct {
        int top;
        int right;
        int bottom;
        int left;
    } padding;

    struct urgency_config by_urgency[3];
};
