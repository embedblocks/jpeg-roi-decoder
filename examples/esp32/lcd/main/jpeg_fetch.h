#pragma once

#include <stdbool.h>
#include "http_stream.h"

/* ============================================================
 *  jpeg_fetch — async JPEG fetch + decode, two-call API.
 *
 *  jpeg_fetch_start()  — opens HTTP, kicks off the decode, returns
 *                        immediately (non-blocking).
 *
 *  jpeg_fetch_wait()   — blocks until on_done fires, then closes
 *                        the HTTP request. Returns true on success.
 *
 *  Main owns the sync boundary explicitly:
 *
 *      jpeg_fetch_start(&http, &params);   // fire
 *      // optionally do other work here
 *      ok = jpeg_fetch_wait(&http, 30000); // wait
 *      if (ok) pan_next(&pan);
 * ============================================================ */

typedef struct {
    const char *url;
    int         lcd_w;
    int         lcd_h;
    int         pan_x;
    int         pan_y;
} jpeg_fetch_params_t;

/** One-time setup — call before the pan loop. */
bool jpeg_fetch_init(void);

/**
 * Open HTTP request and start the async decode.
 * Returns immediately — decode runs in the background.
 * Must be followed by jpeg_fetch_wait() before calling again.
 */
bool jpeg_fetch_start(http_stream_ctx_t *ctx, const jpeg_fetch_params_t *params);

/**
 * Block until the decode completes (on_done fires).
 * Closes the HTTP request before returning.
 *
 * @param ctx         Same context passed to jpeg_fetch_start.
 * @param timeout_ms  Maximum wait time in milliseconds.
 * @return true on success, false on timeout or decode error.
 */
bool jpeg_fetch_wait(http_stream_ctx_t *ctx, uint32_t timeout_ms);