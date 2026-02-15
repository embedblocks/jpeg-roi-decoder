/* jpeg_decoder_core.c */

#include "jpeg_roi_decoder.h"
#include "tjpgd.h"
#include "tjpgd_sys.h"

#include <string.h>

/* ---------------- internal context ---------------- */

#define JPEG_MAX_ROI_HEIGHT  512   // or something safe


typedef struct {
    FILE *fp;
    jpeg_roi_t roi;
    jpeg_decode_scale_t scale;

    uint16_t *chunk_buffer;
    size_t chunk_buffer_pixels;

    jpeg_chunk_cb_t chunk_cb;
    jpeg_done_cb_t  done_cb;
    void *user_data;

    uint16_t image_width;
    uint16_t image_height;

    uint16_t roi_width;
    uint16_t roi_height;
    uint16_t row_fill_count[JPEG_MAX_ROI_HEIGHT];
    bool     row_flushed[JPEG_MAX_ROI_HEIGHT];



    bool abort;
} decode_context_t;

/* ---------------- TJpgDec input ---------------- */

static size_t input_func(JDEC *jd, uint8_t *buff, size_t nbyte)
{
    decode_context_t *ctx = jd->device;

    if (buff) {
        return fread(buff, 1, nbyte, ctx->fp);
    } else {
        fseek(ctx->fp, nbyte, SEEK_CUR);
        return nbyte;
    }
}

/* ---------------- TJpgDec output ---------------- */

static int output_func(JDEC *jd, void *bitmap, JRECT *rect)
{
    decode_context_t *ctx = jd->device;
    uint16_t *src = (uint16_t *)bitmap;

    if (rect->right  < ctx->roi.left  ||
        rect->left   > ctx->roi.right ||
        rect->bottom < ctx->roi.top   ||
        rect->top    > ctx->roi.bottom) {
        return 1;
    }

    uint16_t mcu_w = rect->right - rect->left + 1;

    uint16_t y_start = rect->top    < ctx->roi.top    ? ctx->roi.top    : rect->top;
    uint16_t y_end   = rect->bottom > ctx->roi.bottom ? ctx->roi.bottom : rect->bottom;
    uint16_t x_start = rect->left   < ctx->roi.left   ? ctx->roi.left   : rect->left;
    uint16_t x_end   = rect->right  > ctx->roi.right  ? ctx->roi.right  : rect->right;

    uint16_t copy_w = x_end - x_start + 1;

    for (uint16_t y = y_start; y <= y_end; y++) {

        uint16_t roi_y = y - ctx->roi.top;
        uint16_t roi_x = x_start - ctx->roi.left;

        uint16_t src_off =
            (y - rect->top) * mcu_w + (x_start - rect->left);

        memcpy(&ctx->chunk_buffer[roi_x],
               &src[src_off],
               copy_w * sizeof(uint16_t));

        ctx->row_fill_count[roi_y] += copy_w;

        if (ctx->row_fill_count[roi_y] >= ctx->roi_width &&
            !ctx->row_flushed[roi_y]) {

            jpeg_chunk_info_t info = {
                .x = 0,
                .y = roi_y,
                .width  = ctx->roi_width,
                .height = 1
            };

            jpeg_chunk_event_t evt = {
                .fp = ctx->fp,
                .pixels = ctx->chunk_buffer,
                .chunk = &info,
                .pixel_count = ctx->roi_width,
                .byte_count  = ctx->roi_width * 2,
                .user_data = ctx->user_data
            };

            if (ctx->chunk_cb && !ctx->chunk_cb(&evt)) {
                ctx->abort = true;
                return 0;
            }

            ctx->row_flushed[roi_y] = true;
        }
    }

    return 1;
}

/* ---------------- core runner ---------------- */

jpeg_decode_result_t
jpeg_decoder_core_run(
    const jpeg_decode_request_t *req,
    jpeg_done_event_t *done_evt,
    void *workbuf,
    size_t workbuf_size
){

    decode_context_t ctx = {
        .fp = req->fp,
        .roi = req->roi,
        .scale = req->scale,
        .chunk_buffer = req->chunk_buffer,
        .chunk_buffer_pixels = req->chunk_buffer_pixels,
        .chunk_cb = req->chunk_callback,
        .done_cb  = req->done_callback,
        .user_data = req->user_data
    };

    JDEC jd;
    JRESULT jr = tjpgd_sys_prepare(&jd, input_func,
                                  workbuf,
                                  workbuf_size,
                                  &ctx);

    if (jr != JDR_OK)
        return JPEG_DECODE_ERR_INPUT;

    ctx.image_width  = jd.width;
    ctx.image_height = jd.height;

    uint16_t scale_div = 1 << ctx.scale;

    ctx.image_width  /= scale_div;
    ctx.image_height /= scale_div;
    // Convert ROI from original space to scaled space
    ctx.roi.left   /= scale_div;
    ctx.roi.top    /= scale_div;
    ctx.roi.right  /= scale_div;
    ctx.roi.bottom /= scale_div;

        fprintf(stderr,
        "CORE DEBUG:\n"
        "  Original image: %ux%u\n"
        "  Scaled image:   %ux%u\n"
        "  ROI scaled:     [%u,%u,%u,%u]\n",
        jd.width,
        jd.height,
        ctx.image_width,
        ctx.image_height,
        ctx.roi.left,
        ctx.roi.top,
        ctx.roi.right,
        ctx.roi.bottom
    );


    ctx.roi_width  = ctx.roi.right  - ctx.roi.left + 1;
    ctx.roi_height = ctx.roi.bottom - ctx.roi.top  + 1;

    if (ctx.roi.right >= ctx.image_width ||
        ctx.roi.bottom >= ctx.image_height)
        return JPEG_DECODE_ERR_PARAM;

    jr = tjpgd_sys_decomp(&jd, output_func, ctx.scale);

    jpeg_decode_result_t result =
        (jr == JDR_OK || ctx.abort) ? JPEG_DECODE_OK
                                   : JPEG_DECODE_ERR_INTR;

    if (done_evt) {
        done_evt->fp = req->fp;
        done_evt->result = result;
        done_evt->image.width = jd.width;
        done_evt->image.height = jd.height;
        done_evt->roi = ctx.roi;
        done_evt->scale = ctx.scale;
        done_evt->user_data = ctx.user_data;
    }

    return result;
}

typedef struct {
    FILE *fp;
} probe_context_t;

static size_t probe_input_func(JDEC *jd, uint8_t *buff, size_t nbyte)
{
    probe_context_t *ctx = (probe_context_t *)jd->device;

    if (buff) {
        return fread(buff, 1, nbyte, ctx->fp);
    } else {
        fseek(ctx->fp, nbyte, SEEK_CUR);
        return nbyte;
    }
}

jpeg_decode_result_t
jpeg_decoder_probe(FILE *fp,
                   jpeg_image_info_t *info,
                   void *workbuf,
                   size_t workbuf_size)
{
    if (!fp || !info || !workbuf)
        return JPEG_DECODE_ERR_PARAM;

    JDEC jd;

    probe_context_t ctx = {
        .fp = fp
    };

    JRESULT jr = tjpgd_sys_prepare(&jd,
                                   probe_input_func,
                                   workbuf,
                                   workbuf_size,
                                   &ctx);   // <-- NOT NULL

    if (jr != JDR_OK)
        return JPEG_DECODE_ERR_INPUT;

    info->width  = jd.width;
    info->height = jd.height;

    return JPEG_DECODE_OK;
}
