#include "svg.h"

#include <stdlib.h>

#define LOG_MODULE "svg"
#define LOG_ENABLE_DBG 1
#include "log.h"

#include "3rd-party/nanosvg.h"
#include "3rd-party/nanosvgrast.h"

pixman_image_t *
svg_load(const char *path, int size)
{
    NSVGimage *svg = nsvgParseFromFile(path, "px", 96);
    if (svg == NULL)
        return NULL;

    if (svg->width == 0 || svg->height == 0) {
        nsvgDelete(svg);
        return NULL;
    }

    struct NSVGrasterizer *rast = nsvgCreateRasterizer();

    const int w = size;
    const int h = size;
    float scale = w > h ? w / svg->width : h / svg->height;

    uint8_t *data = malloc(h * w * 4);
    nsvgRasterize(rast, svg, 0, 0, scale, data, w, h, w * 4);

    nsvgDeleteRasterizer(rast);
    nsvgDelete(svg);

    return pixman_image_create_bits_no_clear(
        PIXMAN_a8b8g8r8, w, h, (uint32_t *)data, w * 4);
}
