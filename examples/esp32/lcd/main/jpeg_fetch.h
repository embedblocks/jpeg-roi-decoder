#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "http_stream.h"
#include "jpeg_roi_decoder.h"

/* ============================================================
 *  jpeg_fetch — async JPEG fetch+decode, probe-then-decode model.
 *
 *  Flow:
 *    1. jpeg_fetch_probe()  — one HTTP request, reads only headers,
 *                             returns actual image dimensions + chosen scale.
 *    2. From those dims, compute:
 *         effective_w = min(lcd_w, scaled_w)
 *         effective_h = min(lcd_h, scaled_h)
 *         pan_x_max   = max(0, scaled_w - lcd_w)
 *         pan_y_max   = max(0, scaled_h - lcd_h)
 *    3. jpeg_fetch_start() / jpeg_fetch_wait() — per-frame decode loop
 *       using effective_w/h so the ROI never exceeds image bounds.
 *
 *  Why probe first?
 *    jpeg_decoder_core_run_view validates:
 *      roi.bottom < image_height  (scaled)
 *    If lcd_h > scaled_h (image shorter than LCD), roi.bottom overflows,
 *    the component fires done(ERR_PARAM) before touching a single MCU,
 *    and no chunk callbacks ever run.
 * ============================================================ */

typedef struct {
    uint16_t            img_w;        /* original JPEG width  (pixels) */
    uint16_t            img_h;        /* original JPEG height (pixels) */
    jpeg_decode_scale_t scale;        /* auto-selected scale factor    */
    uint16_t            scaled_w;     /* img_w >> scale                */
    uint16_t            scaled_h;     /* img_h >> scale                */
    uint16_t            effective_w;  /* min(lcd_w, scaled_w)          */
    uint16_t            effective_h;  /* min(lcd_h, scaled_h)          */
    int                 pan_x_max;    /* max valid pan_x (0 if image fits) */
    int                 pan_y_max;    /* max valid pan_y (0 if image fits) */
} jpeg_image_meta_t;

typedef struct {
    const char         *url;
    uint16_t            lcd_w;
    uint16_t            lcd_h;
    int                 pan_x;
    int                 pan_y;
    jpeg_decode_scale_t scale;        /* set from jpeg_image_meta_t.scale */
    uint16_t            effective_w;  /* set from jpeg_image_meta_t       */
    uint16_t            effective_h;  /* set from jpeg_image_meta_t       */
} jpeg_fetch_params_t;

/** One-time semaphore setup. Call before the pan loop. */
bool jpeg_fetch_init(void);

/**
 * Open one HTTP request, read only JPEG headers, close.
 * Fills meta with actual image dimensions, chosen scale, effective
 * view size, and valid pan bounds.
 * Returns true on success.
 */
bool jpeg_fetch_probe(http_stream_ctx_t *ctx,
                      uint16_t           lcd_w,
                      uint16_t           lcd_h,
                      jpeg_image_meta_t *meta);

/**
 * Open HTTP request and enqueue async decode.
 * Returns immediately — decode runs in jpeg_worker_task.
 * Must be followed by jpeg_fetch_wait() before calling again.
 */
bool jpeg_fetch_start(http_stream_ctx_t       *ctx,
                      const jpeg_fetch_params_t *params);

/**
 * Block until on_done fires, drain HTTP, close TCP.
 * Returns true on successful decode.
 */
bool jpeg_fetch_wait(http_stream_ctx_t *ctx, uint32_t timeout_ms);