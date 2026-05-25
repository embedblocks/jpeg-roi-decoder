#include "http_stream.h"

#include "esp_log.h"
#include "esp_crt_bundle.h"

static const char *TAG = "HTTP_STREAM";

/* ── Tier 1: init / deinit ───────────────────────────────────────────────── */

bool http_stream_init(http_stream_ctx_t *ctx, const char *url)
{
    *ctx = (http_stream_ctx_t){0};

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .buffer_size       = 2048,
        .buffer_size_tx    = 512,
        .user_agent        = "Mozilla/5.0 (ESP32)",
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    ctx->client = esp_http_client_init(&cfg);
    if (!ctx->client) {
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return false;
    }

    /*
     * Push timeout down to the mbedTLS socket layer.
     * Must be called after init — the config struct doesn't reach this far.
     */
    esp_http_client_set_timeout_ms(ctx->client, 10000);

    ESP_LOGI(TAG, "HTTP client ready");
    return true;
}

void http_stream_deinit(http_stream_ctx_t *ctx)
{
    if (!ctx->client) return;
    esp_http_client_cleanup(ctx->client);
    ctx->client = NULL;
    ESP_LOGI(TAG, "HTTP client freed");
}

/* ── Tier 2: per-frame request ───────────────────────────────────────────── */

bool http_stream_request_open(http_stream_ctx_t *ctx)
{
    if (!ctx->client) {
        ESP_LOGE(TAG, "request_open called on uninitialised context");
        return false;
    }

    /* Reset per-request state; the client handle itself is reused */
    ctx->bytes_total = 0;
    ctx->eof         = false;
    ctx->error       = false;

    if (esp_http_client_open(ctx->client, 0 /* no request body */) != ESP_OK) {
        ESP_LOGE(TAG, "esp_http_client_open failed");
        return false;
    }

    int content_length = esp_http_client_fetch_headers(ctx->client);
    int status         = esp_http_client_get_status_code(ctx->client);

    ESP_LOGI(TAG, "HTTP %d  content-length=%d", status, content_length);

    if (status != 200) {
        ESP_LOGE(TAG, "Non-200 status (%d) — closing", status);
        http_stream_request_close(ctx);
        return false;
    }

    return true;
}

void http_stream_request_close(http_stream_ctx_t *ctx)
{
    if (!ctx->client) return;

    /*
     * Drain any bytes the decoder didn't consume.
     *
     * The ROI decoder stops reading as soon as the viewport is decoded.
     * For a top-left pan on a large image, that can mean 90%+ of the
     * response body is still sitting in the socket.
     *
     * esp_http_client_close() on a partially-read response leaves the
     * HTTP client's internal state machine inconsistent, which corrupts
     * the TLS session on the next open() call.  Draining first ensures
     * the client sees a clean EOF before we close the TCP socket.
     *
     * Small stack buffer — no heap allocation.
     */
    if (!ctx->eof && !ctx->error) {
        uint8_t sink[512];
        int     drained = 0;

        while (true) {
            int r = esp_http_client_read(ctx->client, (char *)sink, sizeof(sink));
            if (r <= 0) break;
            drained += r;
        }

        if (drained > 0) {
            ESP_LOGD(TAG, "drained %d unread bytes", drained);
        }
    }

    /*
     * Close the TCP connection — the handle and TLS context stay alive.
     * The next request_open() reuses them without re-doing the TLS handshake
     * (if the server supports keep-alive) or at minimum without re-allocating
     * the mbedTLS session objects that were crashing us.
     */
    esp_http_client_close(ctx->client);
}

/* ── Tier 3: decoder read callback ──────────────────────────────────────── */

size_t http_stream_read_cb(uint8_t *dst, size_t max, void *vctx)
{
    http_stream_ctx_t *ctx = (http_stream_ctx_t *)vctx;

    if (ctx->error || ctx->eof || !ctx->client) return 0;

    /*
     * dst == NULL: skip / drain.
     * The decoder needs to advance past bytes it doesn't care about
     * (EXIF, non-ROI MCUs).  We still drain them from the socket to
     * keep the read position in sync.
     */
    if (dst == NULL) {
        uint8_t discard[256];
        size_t  skipped = 0;

        while (skipped < max) {
            size_t want = max - skipped;
            if (want > sizeof(discard)) want = sizeof(discard);

            int r = esp_http_client_read(ctx->client, (char *)discard, want);
            if (r <= 0) { ctx->eof = true; break; }

            skipped          += (size_t)r;
            ctx->bytes_total += r;
        }
        return skipped;
    }

    /* Normal read — fill dst with up to max bytes */
    int r = esp_http_client_read(ctx->client, (char *)dst, max);

    if (r < 0) { ctx->error = true; return 0; }
    if (r == 0) { ctx->eof  = true; return 0; }

    ctx->bytes_total += r;
    return (size_t)r;
}