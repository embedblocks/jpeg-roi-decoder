/**
 * Example 1 — Minimal LCD decode (recommended starting point)
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

typedef struct {
    uint16_t *framebuf;
    uint16_t width;
    uint16_t height;
} display_ctx_t;

static bool on_chunk(const jpeg_chunk_event_t *evt)
{
    display_ctx_t *ctx = (display_ctx_t*)evt->user_data;

    uint16_t *dst = ctx->framebuf +
        (size_t)evt->y * ctx->width + evt->x;

    memcpy(dst, evt->pixels, evt->byte_count);
    return true;
}

static void on_done(const jpeg_done_event_t *evt)
{
    display_ctx_t *ctx = (display_ctx_t*)evt->user_data;

    if (evt->result != JPEG_DECODE_OK) {
        printf("decode failed: %s\n",
               jpeg_decoder_err_to_str(evt->result));
        return;
    }

    /* push ctx->framebuf to LCD here */
}

void app_main(void)
{
    FILE *fp = fopen("photo.jpg", "rb");
    if (!fp) return;

    //uint8_t workbuf[JPEG_DECODER_WORK_BUF_DEFAULT];
    //uint16_t framebuf[LCD_W * LCD_H];

    display_ctx_t ctx = {
        .framebuf = framebuf,
        .width    = LCD_W,
        .height   = LCD_H
    };

    jpeg_view_t view = jpeg_view_default(LCD_W, LCD_H);

    jpeg_decoder_decode_view(
        jpeg_decoder_source_from_file(fp),
        &view,
        work_buf, sizeof(work_buf),
        on_chunk, on_done,
        &ctx
    );

    fclose(fp);
}