/* jpeg_decoder_core.c
 *
 * Core decode logic. No source backends live here — those are caller
 * responsibility. The only source abstraction is decode_context_t.reader.
 *
 * Single-pass flow for both high-level and low-level paths:
 *   tjpgd_sys_prepare()  →  scale / ROI resolution  →  tjpgd_sys_decomp()
 *
 * No rewind, no double-header-read, no seek() required from the source.
 */

#include "jpeg_decoder_internal.h"
#include "tjpgd.h"
#include "tjpgd_sys.h"
#include <esp_log.h>
#include <string.h>

static const char *TAG = "JD_CORE";

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

jpeg_view_intent_t jpeg_view_default(uint16_t lcd_width, uint16_t lcd_height)
{
    return (jpeg_view_intent_t){
        .lcd_width    = lcd_width,
        .lcd_height   = lcd_height,
        .pan_x        = 0,
        .pan_y        = 0,
        .scale        = JPEG_SCALE_AUTO,
        .out_format   = JPEG_OUTPUT_RGB565,
        .reader       = { .cb = NULL, .ctx = NULL },
        .chunk_buffer = NULL,   /* caller must set before decoding */
        .input_buffer = NULL,   /* optional — only for sources such as HTTP where server
                                who can close connections on small request by tjpgd during header stage*/
    };
}

/* ============================================================
 *  Auto scale selection
 * ============================================================ */

static jpeg_decode_scale_t jpeg_decoder_auto_scale(
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
 *  TJpgDec input callback
 *
 *  Passes TJpgDec's own buffer directly to reader.cb — zero copy.
 *  When buf is NULL, TJpgDec wants to skip forward; NULL is passed
 *  straight through so seekable sources can fseek instead of reading.
 *  A retry loop handles partial reads without any internal staging buffer.
 * ============================================================ */

static size_t input_func(JDEC *jd, uint8_t *buf, size_t nbyte)
{
    decode_context_t *ctx = (decode_context_t *)jd->device;
    size_t total = 0;

    /* ---- Direct pass-through (no prefetch buffer) ---- */
    if (!ctx->input_buf) {
        while (total < nbyte) {
            size_t got = ctx->reader.cb(buf ? buf + total : NULL,
                                        nbyte - total,
                                        ctx->reader.ctx);
            if (got == 0)
                break;
            total += got;
        }
        return total;
    }

    /* ---- Prefetch buffer path ----
     *
     * reader.cb is always called with a real dst pointer (never NULL),
     * so non-seekable sources (HTTP, UART) are handled correctly.
     *
     * When buf == NULL (TJpgDec skip request), we drain bytes from the
     * prefetch buffer without copying, then refill and drain again as
     * needed — consuming and discarding bytes without any fseek.
     */
    while (total < nbyte) {
        size_t avail = ctx->input_buf_len - ctx->input_buf_pos;

        if (avail == 0) {
            /* Buffer empty — pull one full chunk from the source */
            ctx->input_buf_pos = 0;
            ctx->input_buf_len = 0;
            size_t got = ctx->reader.cb(ctx->input_buf, JPEG_INPUT_BUF_SIZE,
                                        ctx->reader.ctx);
            if (got == 0)
                break;   /* EOF or transport error */
            ctx->input_buf_len = got;
            avail = got;
        }

        size_t n = (nbyte - total) < avail ? (nbyte - total) : avail;

        if (buf)
            memcpy(buf + total, ctx->input_buf + ctx->input_buf_pos, n);
        /* buf == NULL: skip — advance cursor only, no copy */

        ctx->input_buf_pos += n;
        total              += n;
    }
    return total;
}

/* ============================================================
 *  RGB565 → RGB888
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
 *
 *  TJpgDec scans left-to-right across the full MCU row band before
 *  moving down. Each MCU block is JPEG_MCU_MAX_HEIGHT rows tall.
 *
 *  Fix for band overwrite: use (roi_y % JPEG_MCU_MAX_HEIGHT) as a
 *  slot index so each row accumulates in its own dedicated buffer
 *  slot. A row is flushed exactly once its slot reaches roi_width.
 * ============================================================ */

static int output_func(JDEC *jd, void *bitmap, JRECT *rect)
{
    decode_context_t *ctx = (decode_context_t *)jd->device;
    const uint16_t   *src = (const uint16_t *)bitmap;

    /* Quick reject — MCU entirely outside ROI */
    if (rect->right  < ctx->roi.left  ||
        rect->left   > ctx->roi.right ||
        rect->bottom < ctx->roi.top   ||
        rect->top    > ctx->roi.bottom)
        return 1;

    if (rect->left == 304 && rect->top == 48) {
        ESP_LOGD("MCU", "last MCU of band 3 — "
                 "chunk_buf[12*w]=%u chunk_buf[12*w+160]=%u",
                 ctx->chunk_buffer[12 * ctx->roi_width],
                 ctx->chunk_buffer[12 * ctx->roi_width + 160]);
    }

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

        /* Each row gets its own slot in the band buffer */
        uint16_t slot    = roi_y % JPEG_MCU_MAX_HEIGHT;
        uint16_t src_off = (y - rect->top) * mcu_w + (x_start - rect->left);

        memcpy(&ctx->chunk_buffer[slot * ctx->roi_width + roi_x],
               &src[src_off],
               copy_w * sizeof(uint16_t));

        ctx->row_fill_count[roi_y] += copy_w;

        if (ctx->row_fill_count[roi_y] >= ctx->roi_width &&
            !ctx->row_flushed[roi_y]) {

            if (roi_y >= 55 && roi_y <= 65) {
                ESP_LOGD("FLUSH", "roi_y=%u slot=%u fill=%u "
                         "buf[slot*w+0]=%u buf[slot*w+160]=%u buf[slot*w+319]=%u",
                         roi_y, slot, ctx->row_fill_count[roi_y],
                         ctx->chunk_buffer[slot * ctx->roi_width + 0],
                         ctx->chunk_buffer[slot * ctx->roi_width + 160],
                         ctx->chunk_buffer[slot * ctx->roi_width + 319]);
            }

            jpeg_chunk_event_t evt = {
                .x         = 0,
                .y         = roi_y,
                .width     = ctx->roi_width,
                .user_data = ctx->user_data,
            };

            if (ctx->out_format == JPEG_OUTPUT_RGB888) {
                uint8_t rgb[ctx->roi_width * 3];
                rgb565_to_rgb888(&ctx->chunk_buffer[slot * ctx->roi_width],
                                 rgb, ctx->roi_width);
                evt.pixels     = rgb;
                evt.byte_count = ctx->roi_width * 3;
            } else {
                evt.pixels     = &ctx->chunk_buffer[slot * ctx->roi_width];
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
 *  Shared helper: fire done_callback with a fully-populated event
 * ============================================================ */

static void fire_done(jpeg_done_cb_t done_cb, void *user_data,
                      jpeg_decode_result_t result,
                      const JDEC *jd,              /* may be NULL on early error */
                      const decode_context_t *ctx, /* may be NULL on early error */
                      jpeg_decode_scale_t scale,
                      jpeg_output_format_t out_format)
{
    if (!done_cb)
        return;

    jpeg_done_event_t evt = {
        .result     = result,
        .image      = { jd ? jd->width : 0, jd ? jd->height : 0 },
        .roi_scaled = ctx ? ctx->roi  : (jpeg_roi_t){0},
        .scale      = scale,
        .out_format = out_format,
        .user_data  = user_data,
    };
    done_cb(&evt);
}

/* ============================================================
 *  High-level core runner
 *
 *  Single forward pass:
 *    prepare → auto-scale resolution → ROI computation → decomp
 *
 *  done_callback is always fired, even on early param errors.
 * ============================================================ */

jpeg_decode_result_t
jpeg_decoder_core_run_view(
    const jpeg_view_intent_t *intent,
    void                     *workbuf,
    size_t                    workbuf_size,
    jpeg_chunk_cb_t           chunk_cb,
    jpeg_done_cb_t            done_cb,
    void                     *user_data
){
    jpeg_decode_result_t result = JPEG_DECODE_ERR_PARAM;

    if (!intent || !workbuf || !intent->reader.cb || !intent->chunk_buffer) {
        fire_done(done_cb, user_data, JPEG_DECODE_ERR_PARAM,
                  NULL, NULL, JPEG_SCALE_1_1, JPEG_OUTPUT_RGB565);
        return JPEG_DECODE_ERR_PARAM;
    }

    decode_context_t ctx = {
        .reader              = intent->reader,
        .out_format          = intent->out_format,
        .chunk_buffer        = intent->chunk_buffer,
        .chunk_buffer_pixels = JPEG_CHUNK_BUF_PIXELS(intent->lcd_width),
        .chunk_cb            = chunk_cb,
        .done_cb             = done_cb,
        .user_data           = user_data,
        .abort               = false,
        .input_buf           = intent->input_buffer,
        .input_buf_len       = 0,
        .input_buf_pos       = 0,
    };
    memset(ctx.row_fill_count, 0, sizeof(ctx.row_fill_count));
    memset(ctx.row_flushed,    0, sizeof(ctx.row_flushed));

    JDEC jd;
    JRESULT jr = tjpgd_sys_prepare(&jd, input_func, workbuf, workbuf_size, &ctx);
    if (jr != JDR_OK) {
        fire_done(done_cb, user_data, JPEG_DECODE_ERR_INPUT,
                  NULL, NULL, JPEG_SCALE_1_1, intent->out_format);
        return JPEG_DECODE_ERR_INPUT;
    }

    jpeg_decode_scale_t scale = intent->scale;
    if (scale == JPEG_SCALE_AUTO)
        scale = jpeg_decoder_auto_scale(jd.width, jd.height,
                                        intent->lcd_width, intent->lcd_height);
    ctx.scale = scale;

    uint16_t div      = 1u << (uint8_t)scale;
    uint16_t scaled_w = jd.width  / div;
    uint16_t scaled_h = jd.height / div;
    uint16_t lcd_w    = intent->lcd_width;
    uint16_t lcd_h    = intent->lcd_height;

    ctx.image_width  = scaled_w;
    ctx.image_height = scaled_h;

    int32_t cx     = ((int32_t)scaled_w - lcd_w) / 2 + intent->pan_x;
    int32_t cy     = ((int32_t)scaled_h - lcd_h) / 2 + intent->pan_y;
    int32_t max_cx = (int32_t)scaled_w - lcd_w;
    int32_t max_cy = (int32_t)scaled_h - lcd_h;

    ESP_LOGD(TAG, "pre-clamp:  cx=%ld cy=%ld  max_cx=%ld max_cy=%ld",
           cx, cy, max_cx, max_cy);

    if (cx < 0)                      cx = 0;
    if (cy < 0)                      cy = 0;
    if (max_cx >= 0 && cx > max_cx)  cx = max_cx;
    if (max_cy >= 0 && cy > max_cy)  cy = max_cy;

    ctx.roi.left   = (uint16_t)cx;
    ctx.roi.top    = (uint16_t)cy;
    ctx.roi.right  = (uint16_t)(cx + lcd_w - 1);
    ctx.roi.bottom = (uint16_t)(cy + lcd_h - 1);

    /* Clamp ROI to actual scaled image bounds instead of hard-rejecting */
    if (ctx.roi.right  >= ctx.image_width)   ctx.roi.right  = ctx.image_width  - 1;
    if (ctx.roi.bottom >= ctx.image_height)  ctx.roi.bottom = ctx.image_height - 1;

    ESP_LOGD(TAG, "post-clamp: cx=%ld cy=%ld", cx, cy);
    ESP_LOGD(TAG, "ROI(scaled): left=%u top=%u right=%u bottom=%u",
           ctx.roi.left, ctx.roi.top, ctx.roi.right, ctx.roi.bottom);

    if (ctx.roi.left   >  ctx.roi.right  ||
        ctx.roi.top    >  ctx.roi.bottom) {
        fire_done(done_cb, user_data, JPEG_DECODE_ERR_PARAM,
                  &jd, &ctx, scale, intent->out_format);
        return JPEG_DECODE_ERR_PARAM;
    }

    ctx.roi_width  = ctx.roi.right  - ctx.roi.left  + 1;
    ctx.roi_height = ctx.roi.bottom - ctx.roi.top   + 1;

    if (ctx.roi_height > JPEG_MAX_ROI_HEIGHT) {
        fire_done(done_cb, user_data, JPEG_DECODE_ERR_PARAM,
                  &jd, &ctx, scale, intent->out_format);
        return JPEG_DECODE_ERR_PARAM;
    }

    if (ctx.chunk_buffer_pixels < (size_t)ctx.roi_width * JPEG_MCU_MAX_HEIGHT) {
        fire_done(done_cb, user_data, JPEG_DECODE_ERR_PARAM,
                  &jd, &ctx, scale, intent->out_format);
        return JPEG_DECODE_ERR_PARAM;
    }

    jr = tjpgd_sys_decomp(&jd, output_func, scale);

    if      (ctx.abort)    result = JPEG_DECODE_OK;
    else if (jr == JDR_OK) result = JPEG_DECODE_OK;
    else                   result = JPEG_DECODE_ERR_INTR;

    fire_done(done_cb, user_data, result, &jd, &ctx, scale, intent->out_format);
    return result;
}
/* ============================================================
 *  Low-level core runner
 *
 *  ROI is pre-supplied in unscaled JPEG coords; JPEG_SCALE_AUTO rejected.
 *  Single forward pass: prepare → scale divisor applied → validate → decomp.
 *  done_callback is always fired, even on early param errors.
 * ============================================================ */

jpeg_decode_result_t
jpeg_decoder_core_run_request(const jpeg_decode_request_t *req)
{
    if (!req || !req->work_buffer || !req->chunk_buffer || !req->reader.cb) {
        if (req && req->done_callback) {
            jpeg_done_event_t evt = { .result     = JPEG_DECODE_ERR_PARAM,
                                      .user_data  = req->user_data };
            req->done_callback(&evt);
        }
        return JPEG_DECODE_ERR_PARAM;
    }
    if (req->scale == JPEG_SCALE_AUTO) {
        fire_done(req->done_callback, req->user_data, JPEG_DECODE_ERR_PARAM,
                  NULL, NULL, JPEG_SCALE_1_1, req->out_format);
        return JPEG_DECODE_ERR_PARAM;
    }

    decode_context_t ctx = {
        .reader              = req->reader,
        .roi                 = req->roi,
        .scale               = req->scale,
        .out_format          = req->out_format,
        .chunk_buffer        = req->chunk_buffer,
        .chunk_buffer_pixels = req->chunk_buffer_pixels,
        .chunk_cb            = req->chunk_callback,
        .done_cb             = req->done_callback,
        .user_data           = req->user_data,
        .abort               = false,
        .input_buf           = req->input_buffer,
        .input_buf_len       = 0,
        .input_buf_pos       = 0,
    };
    memset(ctx.row_fill_count, 0, sizeof(ctx.row_fill_count));
    memset(ctx.row_flushed,    0, sizeof(ctx.row_flushed));

    JDEC jd;
    JRESULT jr = tjpgd_sys_prepare(&jd, input_func,
                                    req->work_buffer, req->work_buffer_size,
                                    &ctx);
    if (jr != JDR_OK) {
        fire_done(req->done_callback, req->user_data, JPEG_DECODE_ERR_INPUT,
                  NULL, NULL, req->scale, req->out_format);
        return JPEG_DECODE_ERR_INPUT;
    }

    uint16_t div       = 1u << (uint8_t)ctx.scale;
    ctx.image_width    = jd.width  / div;
    ctx.image_height   = jd.height / div;
    ctx.roi.left       = req->roi.left   / div;
    ctx.roi.top        = req->roi.top    / div;
    ctx.roi.right      = req->roi.right  / div;
    ctx.roi.bottom     = req->roi.bottom / div;

    /* Clamp ROI to actual scaled image bounds instead of hard-rejecting */
    if (ctx.roi.right  >= ctx.image_width)   ctx.roi.right  = ctx.image_width  - 1;
    if (ctx.roi.bottom >= ctx.image_height)  ctx.roi.bottom = ctx.image_height - 1;

    if (ctx.roi.left   >  ctx.roi.right  ||
        ctx.roi.top    >  ctx.roi.bottom) {
        fire_done(req->done_callback, req->user_data, JPEG_DECODE_ERR_PARAM,
                  &jd, &ctx, ctx.scale, ctx.out_format);
        return JPEG_DECODE_ERR_PARAM;
    }

    ctx.roi_width  = ctx.roi.right  - ctx.roi.left  + 1;
    ctx.roi_height = ctx.roi.bottom - ctx.roi.top   + 1;

    if (ctx.roi_height > JPEG_MAX_ROI_HEIGHT) {
        fire_done(req->done_callback, req->user_data, JPEG_DECODE_ERR_PARAM,
                  &jd, &ctx, ctx.scale, ctx.out_format);
        return JPEG_DECODE_ERR_PARAM;
    }

    if (ctx.chunk_buffer_pixels < (size_t)ctx.roi_width * JPEG_MCU_MAX_HEIGHT) {
        fire_done(req->done_callback, req->user_data, JPEG_DECODE_ERR_PARAM,
                  &jd, &ctx, ctx.scale, ctx.out_format);
        return JPEG_DECODE_ERR_PARAM;
    }

    jr = tjpgd_sys_decomp(&jd, output_func, ctx.scale);

    jpeg_decode_result_t result;
    if      (ctx.abort)    result = JPEG_DECODE_OK;
    else if (jr == JDR_OK) result = JPEG_DECODE_OK;
    else                   result = JPEG_DECODE_ERR_INTR;

    fire_done(req->done_callback, req->user_data, result,
              &jd, &ctx, ctx.scale, ctx.out_format);
    return result;
}
/* ============================================================
 *  Probe — reads only headers, returns dimensions
 *
 *  The caller must reset their own source after this call.
 *  The component provides no reset mechanism.
 * ============================================================ */

jpeg_decode_result_t
jpeg_decoder_probe(
    jpeg_reader_t      reader,
    jpeg_image_info_t *info_out,
    void              *workbuf,
    size_t             workbuf_size
){
    if (!info_out || !workbuf || !reader.cb)
        return JPEG_DECODE_ERR_PARAM;

    decode_context_t ctx = { .reader = reader };
    JDEC jd;
    JRESULT jr = tjpgd_sys_prepare(&jd, input_func, workbuf, workbuf_size, &ctx);
    if (jr != JDR_OK)
        return JPEG_DECODE_ERR_INPUT;

    info_out->width  = jd.width;
    info_out->height = jd.height;
    return JPEG_DECODE_OK;
    /* caller resets their own source — no seek() call here */
}