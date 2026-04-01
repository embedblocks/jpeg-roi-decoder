/* jpeg_decoder_core.c
 *
 * Platform-agnostic engine. Contains:
 *   - jpeg_decoder_core_run()         called by both platform adapters
 *   - jpeg_decoder_probe()            public, synchronous
 *   - jpeg_decoder_prepare_view_request()  shared helper for decode_view
 *   - jpeg_view_default()             public helper
 *   - jpeg_decoder_err_to_str()       public helper
 */

#include "jpeg_decoder_internal.h"
#include "tjpgd.h"
#include "tjpgd_sys.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================
 *  Public helpers
 * ============================================================ */

const char *jpeg_decoder_err_to_str(jpeg_decode_result_t r)
{
    switch (r) {
        case JPEG_DECODE_OK:        return "OK";
        case JPEG_DECODE_ABORTED:   return "aborted by callback";
        case JPEG_DECODE_ERR_PARAM: return "invalid parameter or ROI";
        case JPEG_DECODE_ERR_INPUT: return "source read / prepare failed";
        case JPEG_DECODE_ERR_MEM:   return "work buffer too small";
        case JPEG_DECODE_ERR_FMT:   return "unsupported JPEG format";
        case JPEG_DECODE_ERR_INTR:  return "tjpgd internal error";
        default:                    return "unknown error";
    }
}

jpeg_view_t jpeg_view_default(uint16_t lcd_width, uint16_t lcd_height)
{
    return (jpeg_view_t){
        .lcd_width  = lcd_width,
        .lcd_height = lcd_height,
        .pan_x      = 0,
        .pan_y      = 0,
        .scale      = JPEG_SCALE_AUTO,
        .out_format = JPEG_OUTPUT_RGB565,
    };
}

/* ============================================================
 *  Auto scale selection  (internal + shared)
 * ============================================================ */

jpeg_decode_scale_t jpeg_decoder_auto_scale(
    uint16_t img_w, uint16_t img_h,
    uint16_t lcd_w, uint16_t lcd_h)
{
    static const jpeg_decode_scale_t candidates[] = {
        JPEG_SCALE_1_8, JPEG_SCALE_1_4, JPEG_SCALE_1_2, JPEG_SCALE_1_1,
    };
    for (int i = 0; i < 4; i++) {
        uint16_t div = 1u << (uint8_t)candidates[i];
        if ((img_w / div) >= lcd_w && (img_h / div) >= lcd_h)
            return candidates[i];
    }
    return JPEG_SCALE_1_1;
}

/* ============================================================
 *  Internal decode context
 * ============================================================ */

#define JPEG_MAX_ROI_HEIGHT  512u

typedef struct {
    jpeg_source_t        source;
    jpeg_roi_t           roi;
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
    bool     abort;
} decode_context_t;

/* ============================================================
 *  TJpgDec input callback
 * ============================================================ */

static size_t input_func(JDEC *jd, uint8_t *buf, size_t nbyte)
{
    decode_context_t *ctx = (decode_context_t *)jd->device;
    return ctx->source.read(ctx->source.ctx, buf, nbyte);
}

/* ============================================================
 *  RGB565 -> RGB888
 * ============================================================ */

static void rgb565_to_rgb888(const uint16_t *src, uint8_t *dst, uint16_t n)
{
    for (uint16_t i = 0; i < n; i++) {
        uint16_t p = src[i];
        *dst++ = ((p >> 11) & 0x1F) << 3;
        *dst++ = ((p >>  5) & 0x3F) << 2;
        *dst++ = ( p        & 0x1F) << 3;
    }
}

/* ============================================================
 *  TJpgDec output callback
 * ============================================================ */

static int output_func(JDEC *jd, void *bitmap, JRECT *rect)
{
    decode_context_t *ctx = (decode_context_t *)jd->device;
    const uint16_t *src   = (const uint16_t *)bitmap;

    if (rect->right  < ctx->roi.left  ||
        rect->left   > ctx->roi.right ||
        rect->bottom < ctx->roi.top   ||
        rect->top    > ctx->roi.bottom)
        return 1;

    uint16_t mcu_w   = rect->right - rect->left + 1;
    uint16_t y_start = rect->top    < ctx->roi.top    ? ctx->roi.top    : rect->top;
    uint16_t y_end   = rect->bottom > ctx->roi.bottom ? ctx->roi.bottom : rect->bottom;
    uint16_t x_start = rect->left   < ctx->roi.left   ? ctx->roi.left   : rect->left;
    uint16_t x_end   = rect->right  > ctx->roi.right  ? ctx->roi.right  : rect->right;
    uint16_t copy_w  = x_end - x_start + 1;

    for (uint16_t y = y_start; y <= y_end; y++) {

        uint16_t roi_y = y - ctx->roi.top;
        uint16_t roi_x = x_start - ctx->roi.left;

        if (roi_y >= JPEG_MAX_ROI_HEIGHT) {
            ctx->abort = true;
            return 0;
        }

        uint16_t src_off = (y - rect->top) * mcu_w + (x_start - rect->left);
        memcpy(&ctx->chunk_buffer[roi_x], &src[src_off],
               copy_w * sizeof(uint16_t));

        ctx->row_fill_count[roi_y] += copy_w;

        if (ctx->row_fill_count[roi_y] >= ctx->roi_width &&
            !ctx->row_flushed[roi_y]) {

            jpeg_chunk_event_t evt = {
                .x         = 0,
                .y         = roi_y,
                .width     = ctx->roi_width,
                .user_data = ctx->user_data,
            };

            if (ctx->out_format == JPEG_OUTPUT_RGB888) {
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
 *  Core runner
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
    if (req->scale == JPEG_SCALE_AUTO)
        return JPEG_DECODE_ERR_PARAM;

    decode_context_t ctx = {
        .source              = req->source,
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

    uint16_t div       = 1u << (uint8_t)ctx.scale;
    ctx.image_width    = jd.width  / div;
    ctx.image_height   = jd.height / div;
    ctx.roi.left       = req->roi.left   / div;
    ctx.roi.top        = req->roi.top    / div;
    ctx.roi.right      = req->roi.right  / div;
    ctx.roi.bottom     = req->roi.bottom / div;

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
    if      (ctx.abort)    result = JPEG_DECODE_ABORTED;
    else if (jr == JDR_OK) result = JPEG_DECODE_OK;
    else                   result = JPEG_DECODE_ERR_INTR;

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
 *  Probe
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

    decode_context_t ctx = { .source = source };
    JDEC jd;
    JRESULT jr = tjpgd_sys_prepare(&jd, input_func, workbuf, workbuf_size, &ctx);
    if (jr != JDR_OK)
        return JPEG_DECODE_ERR_INPUT;

    info_out->width  = jd.width;
    info_out->height = jd.height;
    source.seek(source.ctx, 0);
    return JPEG_DECODE_OK;
}

/* ============================================================
 *  View request preparation  (shared by both platform adapters)
 *
 *  Probes, resolves scale, computes ROI, mallocs chunk_buffer.
 *  Sets up a wrapper done_callback that frees chunk_buffer then
 *  calls the original done_callback — safe on both sync and async
 *  paths because it always fires from the task that ran core_run.
 * ============================================================ */

typedef struct {
    uint16_t       *chunk_buffer;
    jpeg_done_cb_t  user_done_cb;
    void           *user_data;
} view_wrapper_ctx_t;

static void view_done_wrapper(const jpeg_done_event_t *evt)
{
    view_wrapper_ctx_t *wc = (view_wrapper_ctx_t *)evt->user_data;

    /* Free the chunk buffer allocated in jpeg_decoder_prepare_view_request */
    free(wc->chunk_buffer);

    /* Fire user callback with original user_data restored */
    if (wc->user_done_cb) {
        jpeg_done_event_t patched  = *evt;
        patched.user_data          = wc->user_data;
        wc->user_done_cb(&patched);
    }

    /* Free the wrapper context itself */
    free(wc);
}

jpeg_decode_result_t
jpeg_decoder_prepare_view_request(
    jpeg_source_t         source,
    const jpeg_view_t    *view,
    void                 *work_buffer,
    size_t                work_buffer_size,
    jpeg_chunk_cb_t       chunk_callback,
    jpeg_done_cb_t        done_callback,
    void                 *user_data,
    jpeg_decode_request_t *req_out
){
    if (!view || !work_buffer || !req_out)
        return JPEG_DECODE_ERR_PARAM;

    /* Step 1: probe */
    jpeg_image_info_t info;
    jpeg_decode_result_t res = jpeg_decoder_probe(
        source, &info, work_buffer, work_buffer_size);
    if (res != JPEG_DECODE_OK)
        return res;

    /* Step 2: resolve scale */
    jpeg_decode_scale_t scale = view->scale;
    if (scale == JPEG_SCALE_AUTO)
        scale = jpeg_decoder_auto_scale(
            info.width, info.height,
            view->lcd_width, view->lcd_height);

    uint16_t div          = 1u << (uint8_t)scale;
    uint16_t scaled_img_w = info.width  / div;
    uint16_t scaled_img_h = info.height / div;
    uint16_t lcd_w        = view->lcd_width;
    uint16_t lcd_h        = view->lcd_height;

    /* Step 3: compute centered + clamped ROI in scaled space */
    int32_t cx = ((int32_t)scaled_img_w - lcd_w) / 2 + view->pan_x;
    int32_t cy = ((int32_t)scaled_img_h - lcd_h) / 2 + view->pan_y;
    int32_t max_x = (int32_t)scaled_img_w - lcd_w;
    int32_t max_y = (int32_t)scaled_img_h - lcd_h;
    if (cx < 0)                   cx = 0;
    if (cy < 0)                   cy = 0;
    if (max_x > 0 && cx > max_x)  cx = max_x;
    if (max_y > 0 && cy > max_y)  cy = max_y;

    /* Convert back to original JPEG space */
    jpeg_roi_t roi = {
        .left   = (uint16_t)( cx              * div),
        .top    = (uint16_t)( cy              * div),
        .right  = (uint16_t)((cx + lcd_w - 1) * div),
        .bottom = (uint16_t)((cy + lcd_h - 1) * div),
    };

    /* Step 4: alloc chunk buffer (one row) */
    uint16_t *chunk_buf = (uint16_t *)malloc(lcd_w * sizeof(uint16_t));
    if (!chunk_buf)
        return JPEG_DECODE_ERR_MEM;

    /* Step 5: alloc wrapper context — freed inside view_done_wrapper */
    view_wrapper_ctx_t *wc = (view_wrapper_ctx_t *)malloc(sizeof(view_wrapper_ctx_t));
    if (!wc) {
        free(chunk_buf);
        return JPEG_DECODE_ERR_MEM;
    }
    wc->chunk_buffer  = chunk_buf;
    wc->user_done_cb  = done_callback;
    wc->user_data     = user_data;

    /* Step 6: fill request */
    *req_out = (jpeg_decode_request_t){
        .source              = source,
        .roi                 = roi,
        .scale               = scale,
        .out_format          = view->out_format,
        .work_buffer         = work_buffer,
        .work_buffer_size    = work_buffer_size,
        .chunk_buffer        = chunk_buf,
        .chunk_buffer_pixels = lcd_w,
        .chunk_callback      = chunk_callback,
        .done_callback       = view_done_wrapper,  /* wrapper, not user cb */
        .user_data           = wc,                 /* wrapper context      */
    };

    return JPEG_DECODE_OK;
}