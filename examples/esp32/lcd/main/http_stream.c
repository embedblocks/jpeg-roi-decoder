#include "http_stream.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"

static const char *TAG = "HTTP_STREAM";

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
    if (!ctx->client) { ESP_LOGE(TAG, "init failed"); return false; }
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

bool http_stream_request_open(http_stream_ctx_t *ctx)
{
    if (!ctx->client) return false;
    ctx->bytes_total = 0;
    ctx->eof         = false;
    ctx->error       = false;

    if (esp_http_client_open(ctx->client, 0) != ESP_OK) {
        ESP_LOGE(TAG, "open failed");
        return false;
    }
    int len    = esp_http_client_fetch_headers(ctx->client);
    int status = esp_http_client_get_status_code(ctx->client);
    ESP_LOGI(TAG, "HTTP %d  content-length=%d", status, len);
    if (status != 200) {
        ESP_LOGE(TAG, "Non-200 (%d)", status);
        http_stream_request_close(ctx);
        return false;
    }
    return true;
}

void http_stream_request_close(http_stream_ctx_t *ctx)
{
    if (!ctx->client) return;
    if (!ctx->eof && !ctx->error) {
        uint8_t sink[512];
        int drained = 0;
        while (true) {
            int r = esp_http_client_read(ctx->client, (char *)sink, sizeof(sink));
            if (r <= 0) break;
            drained += r;
        }
        if (drained > 0) ESP_LOGD(TAG, "drained %d bytes", drained);
    }
    esp_http_client_close(ctx->client);
}

size_t http_stream_read_cb(uint8_t *dst, size_t max, void *vctx)
{
    http_stream_ctx_t *ctx = (http_stream_ctx_t *)vctx;
    if (ctx->error || ctx->eof || !ctx->client) return 0;

    if (dst == NULL) {
        uint8_t discard[256];
        size_t  skipped = 0;
        while (skipped < max) {
            size_t want = max - skipped;
            if (want > sizeof(discard)) want = sizeof(discard);
            int r = esp_http_client_read(ctx->client, (char *)discard, want);
            if (r <= 0) { ctx->eof = true; break; }
            skipped += r; ctx->bytes_total += r;
        }
        return skipped;
    }

    int r = esp_http_client_read(ctx->client, (char *)dst, max);
    if (r < 0) { ctx->error = true; return 0; }
    if (r == 0) { ctx->eof  = true; return 0; }
    ctx->bytes_total += r;
    return (size_t)r;
}