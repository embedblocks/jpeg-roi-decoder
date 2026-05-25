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

static SemaphoreHandle_t     s_done_sem    = NULL;
static jpeg_decode_result_t  s_last_result = JPEG_DECODE_OK;

/* ── Done callback (fires from worker task, always, on any outcome) ──────── */

static void on_done_cb(const jpeg_done_event_t *evt)
{
    s_last_result = evt->result;

    if (evt->result != JPEG_DECODE_OK) {
        ESP_LOGW(TAG, "on_done: %s", jpeg_decoder_err_to_str(evt->result));
    }

    lcd_on_done(evt);
    xSemaphoreGive(s_done_sem);
}

/* ── Init ───────────────────────────────────────────────────────────────── */

bool jpeg_fetch_init(void)
{
    if (s_done_sem) return true;
    s_done_sem = xSemaphoreCreateBinary();
    return s_done_sem != NULL;
}

/* ── Probe ──────────────────────────────────────────────────────────────── */

static jpeg_decode_scale_t auto_scale(uint16_t iw, uint16_t ih,
                                       uint16_t lw, uint16_t lh)
{
    static const jpeg_decode_scale_t c[] = {
        JPEG_SCALE_1_8, JPEG_SCALE_1_4, JPEG_SCALE_1_2, JPEG_SCALE_1_1,
    };
    for (int i = 0; i < 4; i++) {
        uint16_t d = 1u << (uint8_t)c[i];
        if ((iw / d) >= lw && (ih / d) >= lh) return c[i];
    }
    return JPEG_SCALE_1_1;
}

bool jpeg_fetch_probe(http_stream_ctx_t *ctx,
                      uint16_t lcd_w, uint16_t lcd_h,
                      jpeg_image_meta_t *meta)
{
    if (!http_stream_request_open(ctx)) return false;

    jpeg_reader_t     reader = { .cb = http_stream_read_cb, .ctx = ctx };
    jpeg_image_info_t info   = {0};

    jpeg_decode_result_t r = jpeg_decoder_probe(reader, &info,
                                                 s_workbuf, sizeof(s_workbuf));
    http_stream_request_close(ctx);

    if (r != JPEG_DECODE_OK) {
        ESP_LOGE(TAG, "Probe: %s", jpeg_decoder_err_to_str(r));
        return false;
    }

    meta->img_w  = info.width;
    meta->img_h  = info.height;
    meta->scale  = auto_scale(info.width, info.height, lcd_w, lcd_h);

    uint16_t div      = 1u << (uint8_t)meta->scale;
    meta->scaled_w    = info.width  / div;
    meta->scaled_h    = info.height / div;
    meta->effective_w = lcd_w < meta->scaled_w ? lcd_w : meta->scaled_w;
    meta->effective_h = lcd_h < meta->scaled_h ? lcd_h : meta->scaled_h;
    meta->pan_x_max   = (int)meta->scaled_w - lcd_w;
    meta->pan_y_max   = (int)meta->scaled_h - lcd_h;
    if (meta->pan_x_max < 0) meta->pan_x_max = 0;
    if (meta->pan_y_max < 0) meta->pan_y_max = 0;

    ESP_LOGI(TAG, "Probe: %ux%u → scale 1/%u → scaled %ux%u  "
                  "effective %ux%u  pan_max(%d,%d)",
             meta->img_w, meta->img_h, div,
             meta->scaled_w, meta->scaled_h,
             meta->effective_w, meta->effective_h,
             meta->pan_x_max, meta->pan_y_max);
    return true;
}

/* ── Start ──────────────────────────────────────────────────────────────── */

bool jpeg_fetch_start(http_stream_ctx_t *ctx, const jpeg_fetch_params_t *p)
{
    if (!http_stream_request_open(ctx)) {
        ESP_LOGE(TAG, "request_open failed pan(%d,%d)", p->pan_x, p->pan_y);
        return false;
    }

    lcd_output_frame_begin();

    jpeg_view_intent_t view = jpeg_view_default(p->effective_w, p->effective_h);
    view.out_format   = JPEG_OUTPUT_RGB565;
    view.chunk_buffer = s_chunk_buf;
    view.input_buffer = s_input_buf;
    view.scale        = p->scale;
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

bool jpeg_fetch_wait(http_stream_ctx_t *ctx)
{
    /*
     * Wait forever — safe because on_done_cb is guaranteed to fire:
     *
     *   success                → JPEG_DECODE_OK       → xSemaphoreGive
     *   chunk_cb returned false→ JPEG_DECODE_ABORTED  → xSemaphoreGive
     *   read_cb returned 0     → JPEG_DECODE_ERR_INTR → xSemaphoreGive
     *   ROI / param error      → JPEG_DECODE_ERR_PARAM→ xSemaphoreGive
     *   queue validation fail  → done fired inline    → xSemaphoreGive
     *
     * Network stalls resolve via the socket RCVTIMEO set in http_stream_init
     * (esp_http_client_set_timeout_ms).  When a read times out at the socket
     * layer, esp_http_client_read returns -1, read_cb returns 0, the decoder
     * treats it as EOF, fails, and fires on_done.  No cross-task socket
     * manipulation is needed or safe.
     */
    xSemaphoreTake(s_done_sem, portMAX_DELAY);

    http_stream_request_close(ctx);

    ESP_LOGI(TAG, "%5d bytes consumed  result=%s",
             ctx->bytes_total, jpeg_decoder_err_to_str(s_last_result));

    return (s_last_result == JPEG_DECODE_OK);
}