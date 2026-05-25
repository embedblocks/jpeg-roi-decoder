#include "jpeg_fetch.h"
#include "lcd_output.h"

#include "jpeg_roi_decoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "JPEG_FETCH";

#define MAX_LCD_W  320

static uint8_t  s_workbuf  [JPEG_DECODER_WORK_BUF_DEFAULT];
static uint16_t s_chunk_buf[JPEG_CHUNK_BUF_PIXELS(MAX_LCD_W)];
static uint8_t  s_input_buf[JPEG_INPUT_BUF_SIZE];

static SemaphoreHandle_t  s_done_sem = NULL;
static http_stream_ctx_t *s_http_ctx = NULL;

/* ── Done callback ──────────────────────────────────────────────────────── */

static void on_done_cb(const jpeg_done_event_t *evt)
{
    if (evt->result != JPEG_DECODE_OK) {
        ESP_LOGE(TAG, "Decode failed: %s", jpeg_decoder_err_to_str(evt->result));
    }
    lcd_on_done(evt);
    xSemaphoreGive(s_done_sem);
}

/* ── Init ───────────────────────────────────────────────────────────────── */

bool jpeg_fetch_init(void)
{
    if (s_done_sem) return true;
    s_done_sem = xSemaphoreCreateBinary();
    if (!s_done_sem) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return false;
    }
    return true;
}

/* ── Probe ──────────────────────────────────────────────────────────────── */

/*
 * Auto-scale replication — must match jpeg_decoder_auto_scale() in core.
 * We need it here so we can compute effective view dimensions before decoding.
 */
static jpeg_decode_scale_t auto_scale(uint16_t img_w, uint16_t img_h,
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

bool jpeg_fetch_probe(http_stream_ctx_t *ctx,
                      uint16_t           lcd_w,
                      uint16_t           lcd_h,
                      jpeg_image_meta_t *meta)
{
    if (!http_stream_request_open(ctx)) {
        ESP_LOGE(TAG, "probe: HTTP open failed");
        return false;
    }

    jpeg_image_info_t info = {0};
    jpeg_reader_t reader = {
        .cb  = http_stream_read_cb,
        .ctx = ctx,
    };

    /*
     * jpeg_decoder_probe reads only the JPEG headers (SOI, APP, DQT, DHT,
     * SOF markers) — a few hundred bytes at most — then stops.
     * The rest of the response body is unread; we drain it in request_close.
     */
    jpeg_decode_result_t r = jpeg_decoder_probe(reader, &info,
                                                 s_workbuf, sizeof(s_workbuf));

    http_stream_request_close(ctx);   /* drain + close, keeps TLS handle */

    if (r != JPEG_DECODE_OK) {
        ESP_LOGE(TAG, "probe failed: %s", jpeg_decoder_err_to_str(r));
        return false;
    }

    meta->img_w = info.width;
    meta->img_h = info.height;

    meta->scale    = auto_scale(info.width, info.height, lcd_w, lcd_h);
    uint16_t div   = 1u << (uint8_t)meta->scale;
    meta->scaled_w = info.width  / div;
    meta->scaled_h = info.height / div;

    /*
     * Clamp view dimensions to image size.
     *
     * If the LCD is larger than the image in either axis, we must reduce
     * the effective view dimension — the component validates:
     *   roi.bottom < scaled_h  AND  roi.right < scaled_w
     * Passing lcd_h > scaled_h causes roi.bottom to overflow, the component
     * fires done(ERR_PARAM) before any MCU is decoded, and no chunk
     * callbacks ever run.
     */
    meta->effective_w = lcd_w < meta->scaled_w ? lcd_w : meta->scaled_w;
    meta->effective_h = lcd_h < meta->scaled_h ? lcd_h : meta->scaled_h;

    meta->pan_x_max = (int)meta->scaled_w - lcd_w;
    if (meta->pan_x_max < 0) meta->pan_x_max = 0;

    meta->pan_y_max = (int)meta->scaled_h - lcd_h;
    if (meta->pan_y_max < 0) meta->pan_y_max = 0;

    ESP_LOGI(TAG, "Probe: image=%ux%u  scale=1/%u  scaled=%ux%u",
             meta->img_w, meta->img_h, div, meta->scaled_w, meta->scaled_h);
    ESP_LOGI(TAG, "       effective=%ux%u  pan_max=(%d,%d)",
             meta->effective_w, meta->effective_h,
             meta->pan_x_max,   meta->pan_y_max);

    return true;
}

/* ── Start ──────────────────────────────────────────────────────────────── */

bool jpeg_fetch_start(http_stream_ctx_t *ctx, const jpeg_fetch_params_t *p)
{
    s_http_ctx = ctx;

    if (!http_stream_request_open(ctx)) {
        ESP_LOGE(TAG, "request_open failed pan(%d,%d)", p->pan_x, p->pan_y);
        return false;
    }

    lcd_output_frame_begin();

    jpeg_view_intent_t view = jpeg_view_default(p->effective_w, p->effective_h);
    view.out_format   = JPEG_OUTPUT_RGB565;
    view.chunk_buffer = s_chunk_buf;
    view.input_buffer = s_input_buf;
    view.scale        = p->scale;      /* use probed scale — no re-auto */
    view.pan_x        = p->pan_x;
    view.pan_y        = p->pan_y;
    view.reader       = (jpeg_reader_t){
        .cb  = http_stream_read_cb,
        .ctx = ctx,
    };

    jpeg_decoder_decode_view(
        &view,
        s_workbuf, sizeof(s_workbuf),
        lcd_on_chunk,
        on_done_cb,
        NULL
    );

    return true;
}

/* ── Wait ───────────────────────────────────────────────────────────────── */

bool jpeg_fetch_wait(http_stream_ctx_t *ctx, uint32_t timeout_ms)
{
    bool ok = (xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);

    if (!ok) {
        ESP_LOGE(TAG, "Decode timed out after %lu ms", timeout_ms);
    }

    http_stream_request_close(ctx);

    ESP_LOGI(TAG, "%5d bytes consumed", ctx->bytes_total);
    return ok;
}