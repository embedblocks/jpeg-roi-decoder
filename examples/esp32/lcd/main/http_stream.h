#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_client.h"

/* ============================================================
 *  http_stream — reusable HTTP streaming context for the
 *  JPEG ROI decoder's reader callback.
 *
 *  Lifecycle (THREE tiers, not two):
 *
 *    http_stream_init()          — create HTTP client handle ONCE
 *                                  (allocates TLS context, parses URL)
 *
 *    loop {
 *      http_stream_request_open()  — open one TCP connection + GET
 *      http_stream_request_close() — drain leftover bytes, close TCP
 *    }                             (TLS session object is KEPT alive)
 *
 *    http_stream_deinit()        — free handle (call on fatal error only)
 *
 *  Why reuse the handle?
 *    esp_http_client_init / esp_http_client_cleanup allocate and free
 *    the mbedTLS session context.  Doing this every frame in a tight
 *    loop causes heap fragmentation and leaves stale pointers inside
 *    the TLS transport layer, producing a LoadProhibited crash on the
 *    second or third open() call (EXCVADDR = garbage ALPN string ptr).
 *
 *    esp_http_client_close() closes only the TCP socket; the handle
 *    and all TLS objects remain valid and are reused on the next open.
 * ============================================================ */

typedef struct {
    esp_http_client_handle_t client;
    int  bytes_total;   /* bytes consumed by the decoder this request */
    bool eof;
    bool error;
} http_stream_ctx_t;

/* ── Tier 1: one-time setup / teardown ──────────────────────────────────── */

/**
 * Allocate the HTTP client handle and TLS context.
 * Call ONCE at startup, not per-frame.
 */
bool http_stream_init  (http_stream_ctx_t *ctx, const char *url);

/**
 * Free the HTTP client handle.
 * Only needed on fatal error or clean shutdown — never inside the fetch loop.
 */
void http_stream_deinit(http_stream_ctx_t *ctx);

/* ── Tier 2: per-frame request open / close ─────────────────────────────── */

/**
 * Open a TCP connection and issue the GET request.
 * The response body is NOT read — it streams via http_stream_read_cb.
 * Returns true on HTTP 200, false on any error.
 */
bool http_stream_request_open (http_stream_ctx_t *ctx);

/**
 * Drain any unread response bytes, then close the TCP connection.
 *
 * The JPEG decoder stops reading as soon as the ROI is decoded, leaving
 * the remainder of the response body unread in the socket.  If we close
 * without draining, the HTTP client's state machine is left inconsistent
 * and the next request_open() corrupts the TLS context → crash.
 *
 * The handle itself is kept alive; the next request_open() reuses it.
 */
void http_stream_request_close(http_stream_ctx_t *ctx);

/* ── Tier 3: decoder callback ───────────────────────────────────────────── */

/**
 * Reader callback — signature matches jpeg_reader_cb_t.
 *
 *   dst != NULL  →  read up to max bytes into dst
 *   dst == NULL  →  skip/drain max bytes (decoder advancing past non-ROI data)
 *
 * Returns bytes consumed (0 = EOF or error).
 */
size_t http_stream_read_cb(uint8_t *dst, size_t max, void *vctx);