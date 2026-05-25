#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_client.h"

typedef struct {
    esp_http_client_handle_t client;
    int  bytes_total;
    bool eof;
    bool error;
} http_stream_ctx_t;

bool   http_stream_init         (http_stream_ctx_t *ctx, const char *url);
void   http_stream_deinit       (http_stream_ctx_t *ctx);
bool   http_stream_request_open (http_stream_ctx_t *ctx);
void   http_stream_request_close(http_stream_ctx_t *ctx);
size_t http_stream_read_cb      (uint8_t *dst, size_t max, void *vctx);