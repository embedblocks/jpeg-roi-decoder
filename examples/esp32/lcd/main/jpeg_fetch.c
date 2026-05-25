#include "jpeg_fetch.h"
#include "lcd_output.h"

#include "jpeg_roi_decoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "JPEG_FETCH";

/* ── Static decoder buffers ─────────────────────────────────────────────────
 *
 *  workbuf   — decoder scratch (Huffman tables, MCU state).
 *  chunk_buf — one RGB565 row; filled then sent to LCD immediately.
 *  input_buf — pre-read staging: one large read before header parsing
 *              so the server sees a single request, not many small ones.
 * ──────────────────────────────────────────────────────────────────────── */

#define MAX_LCD_W  320

static uint8_t  s_workbuf  [JPEG_DECODER_WORK_BUF_DEFAULT];
static uint16_t s_chunk_buf[JPEG_CHUNK_BUF_PIXELS(MAX_LCD_W)];
static uint8_t  s_input_buf[JPEG_INPUT_BUF_SIZE];

/* ── Completion semaphore ────────────────────────────────────────────────────
 *
 *  Starts at 0.  on_done_cb gives it.  jpeg_fetch_wait() takes it.
 *  Owned here; exposed to main only through jpeg_fetch_wait().
 * ──────────────────────────────────────────────────────────────────────── */

static SemaphoreHandle_t  s_done_sem = NULL;
static http_stream_ctx_t *s_http_ctx = NULL;

/* ── Done callback (fired by decoder when last ROI row is complete) ──────── */

static void on_done_cb(const jpeg_done_event_t *evt)
{
    lcd_on_done(evt);
    xSemaphoreGive(s_done_sem);   /* unblock jpeg_fetch_wait() in main */
}

/* ── One-time init ──────────────────────────────────────────────────────── */

bool jpeg_fetch_init(void)
{
    if (s_done_sem) return true;
    s_done_sem = xSemaphoreCreateBinary();
    if (!s_done_sem) {
        ESP_LOGE(TAG, "Failed to create completion semaphore");
        return false;
    }
    return true;
}

/* ── Start (non-blocking) ───────────────────────────────────────────────── */

bool jpeg_fetch_start(http_stream_ctx_t *ctx, const jpeg_fetch_params_t *p)
{
    s_http_ctx = ctx;

    if (!http_stream_request_open(ctx)) {
        ESP_LOGE(TAG, "request_open failed for pan(%d,%d)", p->pan_x, p->pan_y);
        return false;
    }

    lcd_output_frame_begin();

    jpeg_view_intent_t view = jpeg_view_default(p->lcd_w, p->lcd_h);
    view.out_format   = JPEG_OUTPUT_RGB565;
    view.chunk_buffer = s_chunk_buf;
    view.input_buffer = s_input_buf;
    view.scale        = JPEG_SCALE_AUTO;
    view.pan_x        = p->pan_x;
    view.pan_y        = p->pan_y;
    view.reader       = (jpeg_reader_t){
        .cb  = http_stream_read_cb,
        .ctx = ctx,
    };

    /* Kicks off the decode — returns immediately, runs in background */
    jpeg_decoder_decode_view(
        &view,
        s_workbuf, sizeof(s_workbuf),
        lcd_on_chunk,
        on_done_cb,
        NULL
    );

    return true;
}

/* ── Wait (blocks until on_done fires) ──────────────────────────────────── */

bool jpeg_fetch_wait(http_stream_ctx_t *ctx, uint32_t timeout_ms)
{

    uint32_t init_time = xTaskGetTickCount() / configTICK_RATE_HZ;
    bool ok = (xSemaphoreTake(s_done_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);

    uint32_t current_time = xTaskGetTickCount() / configTICK_RATE_HZ;
    ESP_LOGI(TAG, "jpeg_fetch_wait time : %lu ms", current_time - init_time);

    if (!ok) {
        ESP_LOGE(TAG, "Decode timed out after %lu ms", timeout_ms);
    }

    /* Drain leftover response bytes and close TCP — keep TLS handle alive */
    http_stream_request_close(ctx);

    ESP_LOGI(TAG, "%5d bytes consumed", ctx->bytes_total);

    return ok;
}