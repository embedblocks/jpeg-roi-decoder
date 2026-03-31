/* jpeg_decoder_core.c */

#include "jpeg_roi_decoder.h"
#include "tjpgd.h"
#include "tjpgd_sys.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================
 *  Internal context
 * ============================================================ */

#define JPEG_MAX_ROI_HEIGHT  512u

typedef struct {
    jpeg_source_t        source;
    jpeg_roi_t           roi;           /* scaled space after core_run fills it */
    jpeg_decode_scale_t  scale;
    jpeg_output_format_t out_format;

    uint16_t *chunk_buffer;
    size_t    chunk_buffer_pixels;

    jpeg_chunk_cb_t chunk_cb;
    jpeg_done_cb_t  done_cb;
    void           *user_data;

    uint16_t image_width;
    uint16_t image_height;
    uint16_t roi_width;
    uint16_t roi_height;

    uint16_t row_fill_count[JPEG_MAX_ROI_HEIGHT];
    bool     row_flushed[JPEG_MAX_ROI_HEIGHT];

    bool abort;
} decode_context_t;

/* ============================================================
 *  TJpgDec input callback — shared by decode and probe
 *  Forwards to jpeg_source_t, works for FILE*, buffer, anything.
 * ============================================================ */

static size_t input_func(JDEC *jd, uint8_t *buf, size_t nbyte)
{
    decode_context_t *ctx = (decode_context_t *)jd->device;
    return ctx->source.read(ctx->source.ctx, buf, nbyte);
}

/* ============================================================
 *  RGB565 -> RGB888 conversion  (inline, ~10 cycles/pixel)
 * ============================================================ */

static void rgb565_to_rgb888(const uint16_t *src, uint8_t *dst, uint16_t count)
{
    for (uint16_t i = 0; i < count; i++) {
        uint16_t p = src[i];
        dst[0] = ((p >> 11) & 0x1F) << 3;
        dst[1] = ((p >>  5) & 0x3F) << 2;
        dst[2] = ( p        & 0x1F) << 3;
        dst += 3;
    }
}

/* ============================================================
 *  TJpgDec output callback — called once per MCU block
 * ============================================================ */

static int output_func(JDEC *jd, void *bitmap, JRECT *rect)
{
    decode_context_t *ctx = (decode_context_t *)jd->device;
    const uint16_t *src = (const uint16_t *)bitmap;

    /* Skip MCUs that don't intersect the ROI */
    if (rect->right  < ctx->roi.left  ||
        rect->left   > ctx->roi.right ||
        rect->bottom < ctx->roi.top   ||
        rect->top    > ctx->roi.bottom) {
        return 1;
    }

    uint16_t mcu_w = rect->right - rect->left + 1;

    /* Clamp intersection */
    uint16_t y_start = rect->top  < ctx->roi.top    ? ctx->roi.top    : rect->top;
    uint16_t y_end   = rect->bottom > ctx->roi.bottom ? ctx->roi.bottom : rect->bottom;
    uint16_t x_start = rect->left < ctx->roi.left   ? ctx->roi.left   : rect->left;
    uint16_t x_end   = rect->right > ctx->roi.right  ? ctx->roi.right  : rect->right;

    uint16_t copy_w = x_end - x_start + 1;

    for (uint16_t y = y_start; y <= y_end; y++) {

        uint16_t roi_y = y - ctx->roi.top;
        uint16_t roi_x = x_start - ctx->roi.left;

        /* Bounds guard — should never fire if ROI validation passed */
        if (roi_y >= JPEG_MAX_ROI_HEIGHT) {
            ctx->abort = true;
            return 0;
        }

        uint16_t src_off = (y - rect->top) * mcu_w + (x_start - rect->left);

        /* Copy pixels into the chunk buffer at the correct column offset */
        memcpy(&ctx->chunk_buffer[roi_x],
               &src[src_off],
               copy_w * sizeof(uint16_t));

        ctx->row_fill_count[roi_y] += copy_w;

        /* Flush complete row to user callback */
        if (ctx->row_fill_count[roi_y] >= ctx->roi_width &&
            !ctx->row_flushed[roi_y]) {

            jpeg_chunk_event_t evt = {
                .x          = 0,
                .y          = roi_y,
                .width      = ctx->roi_width,
                .user_data  = ctx->user_data,
            };

            if (ctx->out_format == JPEG_OUTPUT_RGB888) {
                /*
                 * Convert row in-place into a temporary stack buffer.
                 * Max ROI width = image width at 1:1, typically <= 3000px.
                 * 3 bytes * roi_width. For large images consider heap.
                 */
                uint8_t rgb[ctx->roi_width * 3];
                rgb565_to_rgb888(ctx->chunk_buffer, rgb, ctx->roi_width);
                evt.pixels     = rgb;
                evt.byte_count = ctx->roi_width * 3;
            } else {
                evt.pixels     = ctx->chunk_buffer;
                evt.byte_count = ctx->roi_width * 2;
            }

            if (ctx->chunk_cb && !ctx->chunk_cb(&evt)) {
                ctx->abort = true;
                return 0;
            }

            ctx->row_flushed[roi_y] = true;
        }
    }

    return 1;
}

/* ============================================================
 *  Core runner — called by platform adapters (sync + async)
 * ============================================================ */

jpeg_decode_result_t
jpeg_decoder_core_run(
    const jpeg_decode_request_t *req,
    jpeg_done_event_t           *done_evt,
    void                        *workbuf,
    size_t                       workbuf_size
){
    if (!req || !workbuf)
        return JPEG_DECODE_ERR_PARAM;

    decode_context_t ctx = {
        .source              = req->source,       /* source abstraction wired in */
        .roi                 = req->roi,
        .scale               = req->scale,
        .out_format          = req->out_format,
        .chunk_buffer        = req->chunk_buffer,
        .chunk_buffer_pixels = req->chunk_buffer_pixels,
        .chunk_cb            = req->chunk_callback,
        .done_cb             = req->done_callback,
        .user_data           = req->user_data,
        .abort               = false,
    };

    memset(ctx.row_fill_count, 0, sizeof(ctx.row_fill_count));
    memset(ctx.row_flushed,    0, sizeof(ctx.row_flushed));

    JDEC jd;
    JRESULT jr = tjpgd_sys_prepare(&jd, input_func, workbuf, workbuf_size, &ctx);

    if (jr != JDR_OK)
        return JPEG_DECODE_ERR_INPUT;

    /* Scale ROI from original JPEG space to output space */
    uint16_t scale_div      = 1u << (uint8_t)ctx.scale;
    ctx.image_width         = jd.width  / scale_div;
    ctx.image_height        = jd.height / scale_div;
    ctx.roi.left            = req->roi.left   / scale_div;
    ctx.roi.top             = req->roi.top    / scale_div;
    ctx.roi.right           = req->roi.right  / scale_div;
    ctx.roi.bottom          = req->roi.bottom / scale_div;

    /* Validate before using dimensions */
    if (ctx.roi.left   >  ctx.roi.right         ||
        ctx.roi.top    >  ctx.roi.bottom         ||
        ctx.roi.right  >= ctx.image_width        ||
        ctx.roi.bottom >= ctx.image_height)
        return JPEG_DECODE_ERR_PARAM;

    ctx.roi_width  = ctx.roi.right  - ctx.roi.left  + 1;
    ctx.roi_height = ctx.roi.bottom - ctx.roi.top   + 1;

    if (ctx.roi_height > JPEG_MAX_ROI_HEIGHT)
        return JPEG_DECODE_ERR_PARAM;

    if (!ctx.chunk_buffer || ctx.chunk_buffer_pixels < ctx.roi_width)
        return JPEG_DECODE_ERR_PARAM;

    jr = tjpgd_sys_decomp(&jd, output_func, ctx.scale);

    jpeg_decode_result_t result;
    if (ctx.abort)
        result = JPEG_DECODE_ABORTED;
    else if (jr == JDR_OK)
        result = JPEG_DECODE_OK;
    else
        result = JPEG_DECODE_ERR_INTR;

    if (done_evt) {
        done_evt->result     = result;
        done_evt->image      = (jpeg_image_info_t){ jd.width, jd.height };
        done_evt->roi_scaled = ctx.roi;
        done_evt->scale      = ctx.scale;
        done_evt->out_format = ctx.out_format;
        done_evt->user_data  = ctx.user_data;
    }

    return result;
}

/* ============================================================
 *  Probe — read dimensions only, no pixel decode
 * ============================================================ */

jpeg_decode_result_t
jpeg_decoder_probe(
    jpeg_source_t      source,
    jpeg_image_info_t *info_out,
    void              *workbuf,
    size_t             workbuf_size
){
    if (!info_out || !workbuf)
        return JPEG_DECODE_ERR_PARAM;

    /*
     * Reuse decode_context_t so we can share input_func.
     * Only source needs to be populated for prepare.
     */
    decode_context_t ctx = {
        .source = source,
    };

    JDEC jd;
    JRESULT jr = tjpgd_sys_prepare(&jd, input_func, workbuf, workbuf_size, &ctx);

    if (jr != JDR_OK)
        return JPEG_DECODE_ERR_INPUT;

    info_out->width  = jd.width;
    info_out->height = jd.height;

    /* Restore source position so caller can decode immediately after */
    source.seek(source.ctx, 0);

    return JPEG_DECODE_OK;
}