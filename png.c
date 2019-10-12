#include "png-fnott.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include <png.h>
#include <pixman.h>

#define LOG_MODULE "png"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "stride.h"

pixman_image_t *
png_load(const char *path)
{
    pixman_image_t *pix = NULL;

    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;
    png_bytep *row_pointers = NULL;

    int width = 0;
    int height = 0;

    /* open file and test for it being a png */
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        LOG_ERRNO("%s: failed to open", path);
        goto err;
    }

    uint8_t header[8] = {0};
    if (fread(header, 1, 8, fp) != 8 || png_sig_cmp(header, 0, 8)) {
        LOG_ERR("%s: not a PNG", path);
        goto err;
    }

    if ((png_ptr = png_create_read_struct(
             PNG_LIBPNG_VER_STRING, NULL, NULL, NULL)) == NULL ||
        (info_ptr = png_create_info_struct(png_ptr)) == NULL)
    {
        LOG_ERR("%s: failed to initialize libpng", path);
        goto err;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        LOG_ERR("%s: error during libpng init_io", path);
        goto err;
    }

    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, 8);

    png_read_info(png_ptr, info_ptr);

    /* Get meta data */
    width = png_get_image_width(png_ptr, info_ptr);
    height = png_get_image_height(png_ptr, info_ptr);
    png_byte color_type = png_get_color_type(png_ptr, info_ptr);
    png_byte bit_depth = png_get_bit_depth(png_ptr, info_ptr);

    png_read_update_info(png_ptr, info_ptr);

    LOG_DBG("%s: %dx%d@%hhubpp (color-type=%hhu)",
            path, width, height, bit_depth, color_type);

    if (setjmp(png_jmpbuf(png_ptr))) {
        LOG_ERR("%s: error while loading image", path);
        goto err;
    }

    /* Allocat *rows* */
    row_pointers = malloc(height * sizeof(row_pointers[0]));
    for (int i = 0; i < height; i++)
        row_pointers[i] = malloc(png_get_rowbytes(png_ptr, info_ptr));

    png_read_image(png_ptr, row_pointers);

    /* Translate PNG "color" code to pixman format */
    pixman_format_code_t format;
    switch (color_type) {
    case PNG_COLOR_TYPE_RGB:  format = PIXMAN_b8g8r8; assert(false && "untested"); break;
    case PNG_COLOR_TYPE_RGBA: format = PIXMAN_a8b8g8r8; break;
    default:
        LOG_ERR("%s: unsupported PNG color type: %d", path, color_type);
        goto err;
    }

    int stride = stride_for_format_and_width(format, width);
    assert(stride >= png_get_rowbytes(png_ptr, info_ptr));

    /* Copy row-based data to a single chunk, suitable for pixman */
    uint8_t *image_data = malloc(height * stride);
    for (int i = 0; i < height; i++)
        memcpy(&image_data[i * stride], row_pointers[i], width * 4); /* 4 for RGB/BGR too? */

    pix = pixman_image_create_bits_no_clear(
        format, width, height, (uint32_t *)image_data, stride);

err:
    if (row_pointers != NULL) {
        for (int i = 0; i < height; i++)
            free(row_pointers[i]);
        free(row_pointers);
    }

    if (png_ptr != NULL)
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
    if (fp != NULL)
        fclose(fp);

    return pix;
}
