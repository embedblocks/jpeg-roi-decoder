/*
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "http_stream.h"
#include "jpeg_roi_decoder.h"
*/
/* ============================================================
 *  jpeg_fetch — async JPEG fetch+decode, probe-then-decode model.
 *
 *  jpeg_fetch_wait() uses portMAX_DELAY — it never times out.
 *
 *  This is safe because jpeg_decoder_core always calls fire_done
 *  before returning, regardless of outcome:
 *    - successful decode       → JPEG_DECODE_OK
 *    - chunk_cb returned false → JPEG_DECODE_ABORTED
 *    - read error (network)    → JPEG_DECODE_ERR_INTR
 *    - ROI / param error       → JPEG_DECODE_ERR_PARAM
 *    - queue-level error       → done_callback fired inline
 *
 *  Network stalls are handled by the socket's RCVTIMEO (set via
 *  esp_http_client_set_timeout_ms).  When a read times out at the
 *  socket layer, read_cb returns 0, the decoder sees EOF, fails,
 *  and fires done_callback — unblocking jpeg_fetch_wait naturally.
 *  No cross-task socket close is needed or safe.
 * ============================================================ */

 /*
typedef struct {
    uint16_t            img_w;
    uint16_t            img_h;
    jpeg_decode_scale_t scale;
    uint16_t            scaled_w;
    uint16_t            scaled_h;
    uint16_t            effective_w;
    uint16_t            effective_h;
    int                 pan_x_max;
    int                 pan_y_max;
} jpeg_image_meta_t;

typedef struct {
    const char         *url;
    uint16_t            lcd_w;
    uint16_t            lcd_h;
    int                 pan_x;
    int                 pan_y;
    jpeg_decode_scale_t scale;
    uint16_t            effective_w;
    uint16_t            effective_h;
} jpeg_fetch_params_t;

bool jpeg_fetch_init  (void);

bool jpeg_fetch_probe (http_stream_ctx_t       *ctx,
                       uint16_t                 lcd_w,
                       uint16_t                 lcd_h,
                       jpeg_image_meta_t       *meta);

bool jpeg_fetch_start (http_stream_ctx_t       *ctx,
                       const jpeg_fetch_params_t *params);

*/

/*
 * Block until on_done fires (portMAX_DELAY — always safe).
 * Drains and closes the HTTP request before returning.
 * Returns true if the decode completed with JPEG_DECODE_OK.
 */
//bool jpeg_fetch_wait  (http_stream_ctx_t *ctx);

