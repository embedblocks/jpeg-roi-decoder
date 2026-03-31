/**
 * jpeg_roi_decoder — example usage
 *
 *   2. Low-level API   — for direct ROI control (advanced users)
 */

#include "jpeg_roi_decoder.h"
#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Shared setup
 * ---------------------------------------------------------------- */

#define LCD_W  480
#define LCD_H  320

static uint8_t  work_buf[JPEG_DECODER_WORK_BUF_DEFAULT];
static uint16_t framebuf[LCD_W * LCD_H];

/* ================================================================
 * EXAMPLE 2 — Low-level API
 *
 * Decode a specific region of a JPEG in original pixel coordinates.
 * Useful for map tiling, thumbnail pipelines, or any case where
 * you need precise control over the decoded region.
 *
 * You are responsible for:
 *   - knowing the image dimensions (use jpeg_decoder_probe)
 *   - computing the correct ROI coordinates
 *   - allocating a chunk buffer >= scaled ROI width
 * ================================================================ */

static bool on_tile_chunk(const jpeg_chunk_event_t *evt)
{
    uint16_t *dst = framebuf + (size_t)evt->y * LCD_W + evt->x;
    memcpy(dst, evt->pixels, evt->byte_count);
    return true;
}

/* One row of the ROI — sized for the expected output width */
static uint16_t chunk_buf[LCD_W];

void app_main(void)
{
    FILE *fp = fopen("map.jpg", "rb");
    if (!fp) {
        fprintf(stderr, "failed to open file\n");
        return;
    }

    /* Optional: probe first to inspect image dimensions */
    jpeg_image_info_t info;
    jpeg_decode_result_t res = jpeg_decoder_probe(
        jpeg_decoder_source_from_file(fp),
        &info,
        work_buf, sizeof(work_buf)
    );
    if (res != JPEG_DECODE_OK) {
        fprintf(stderr, "probe failed: %s\n", jpeg_decoder_err_to_str(res));
        fclose(fp);
        return;
    }
    fprintf(stderr, "image: %ux%u\n", info.width, info.height);

    /* Probe rewinds the source — reopen or rewind fp before decode */
    rewind(fp);

    /* ROI is in original (unscaled) JPEG coordinates.
     * At JPEG_SCALE_1_4, a 1920x1280 region -> 480x320 output. */
    jpeg_decode_request_t req = {
        .source              = jpeg_decoder_source_from_file(fp),
        .roi                 = { .left=0, .top=0, .right=1919, .bottom=1279 },
        .scale               = JPEG_SCALE_1_4,
        .out_format          = JPEG_OUTPUT_RGB565,
        .work_buffer         = work_buf,
        .work_buffer_size    = sizeof(work_buf),
        .chunk_buffer        = chunk_buf,
        .chunk_buffer_pixels = LCD_W,   /* must be >= scaled ROI width */
        .chunk_callback      = on_tile_chunk,
        .done_callback       = NULL,
        .user_data           = NULL,
    };

    res = jpeg_decoder_decode(&req);
    if (res != JPEG_DECODE_OK)
        fprintf(stderr, "error: %s\n", jpeg_decoder_err_to_str(res));

    fclose(fp);
}