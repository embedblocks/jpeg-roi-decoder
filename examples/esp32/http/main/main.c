#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "protocol_examples_common.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpeg_roi_decoder.h"

#define TAG     "JPEG_HTTP"
#define LCD_W   320
#define LCD_H   240

#define JPEG_URL    CONFIG_JPEG_URL
#define DEBUG 1
/* ============================================================
 *  Static buffers
 * ============================================================ */

static uint8_t  workbuf[JPEG_DECODER_WORK_BUF_DEFAULT];
static uint16_t chunk_buf[JPEG_CHUNK_BUF_PIXELS(LCD_W)];
static uint8_t  input_buf[JPEG_INPUT_BUF_SIZE];

/* ============================================================
 *  Image header for UART sync
 * ============================================================ */

typedef struct {
    uint8_t  sync[3];     // 0xAA 0xAA 0xAA
    uint32_t magic;       // 0xDEADBEEF
    uint16_t width;
    uint16_t height;
    uint8_t  format;
} __attribute__((packed)) img_header_t;

/* ============================================================
 *  HTTP streaming context
 * ============================================================ */

typedef struct {
    esp_http_client_handle_t client;
    int      bytes_total;
    bool     eof;
    bool     error;
} http_stream_ctx_t;

static http_stream_ctx_t http_ctx = {0};

/* ============================================================
 *  HTTP reader callback — called by JPEG decoder as needed
 *  
 *  This is identical in spirit to buf_read_cb: the decoder
 *  calls us saying "give me up to N bytes", and we return
 *  however many we actually got (0 = EOF).
 *  
 *  esp_http_client_read() reads from the socket incrementally,
 *  it does NOT buffer the entire response.
 * ============================================================ */

static size_t http_read_cb(uint8_t *dst, size_t max, void *vctx)
{
    http_stream_ctx_t *ctx = vctx;

    if (ctx->error || ctx->eof || !ctx->client) {
        return 0;
    }

    /*
     * Skip operation (dst == NULL) — JPEG decoder needs to advance
     * past bytes it doesn't care about (e.g. EXIF, other MCUs).
     * We must still drain them from the HTTP stream.
     */
    if (dst == NULL) {
        uint8_t discard[256];
        size_t skipped = 0;
        while (skipped < max) {
            size_t chunk = max - skipped;
            if (chunk > sizeof(discard)) chunk = sizeof(discard);
            int r = esp_http_client_read(ctx->client, (char *)discard, chunk);
            #ifdef DEBUG
            ESP_LOGI(TAG, "Skipping %d bytes", r);
            #endif
            if (r <= 0) {
                ctx->eof = true;
                break;
            }
            skipped += r;
            ctx->bytes_total += r;
        }
        return skipped;
    }

    /* Normal read — decoder wants up to 'max' bytes into 'dst' */
    int r = esp_http_client_read(ctx->client, (char *)dst, max);
    #ifdef DEBUG
            ESP_LOGI(TAG, "Reading %d bytes", r);
    #endif
    if (r < 0) {
        ctx->error = true;
        return 0;
    }
    if (r == 0) {
        ctx->eof = true;
        return 0;
    }

    ctx->bytes_total += r;
    return (size_t)r;
}

/* ============================================================
 *  JPEG decoder callbacks
 * ============================================================ */

static bool on_chunk(const jpeg_chunk_event_t *evt)
{
    if (evt->width != LCD_W) {
        return false;
    }

    #ifndef DEBUG
    size_t written = uart_write_bytes(UART_NUM_0,
                                      (const char *)evt->pixels,
                                      evt->byte_count);
    if (written != evt->byte_count) {
        return false;
    }

    uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(1000));
    #endif

    return true;
}

static void on_done(const jpeg_done_event_t *evt)
{
    (void)evt;
}

/* ============================================================
 *  HTTP stream open / close
 *  open:   connect + read headers only (body NOT fetched yet)
 *  close:  cleanup
 * ============================================================ */

static bool http_open(const char *url)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        
        /* 
         * Set to 2048 to match the component's internal max request size.
         * - Not 0: Prevents HTTPS/chunked parsing crashes.
         * - Not 16384: Prevents greedy blocking/hangs.
         */
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        
        .user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    http_ctx.client = esp_http_client_init(&config);
    if (!http_ctx.client) {
        ESP_LOGE(TAG, "HTTP client init failed");
        return false;
    }

    if (esp_http_client_open(http_ctx.client, 0) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed");
        esp_http_client_cleanup(http_ctx.client);
        http_ctx.client = NULL;
        return false;
    }

    int content_length = esp_http_client_fetch_headers(http_ctx.client);
    int status = esp_http_client_get_status_code(http_ctx.client);

    ESP_LOGI(TAG, "HTTP %d, len=%d", status, content_length);

    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d — aborting", status);
        esp_http_client_close(http_ctx.client);
        esp_http_client_cleanup(http_ctx.client);
        http_ctx.client = NULL;
        return false;
    }

    http_ctx.bytes_total = 0;
    http_ctx.eof = false;
    http_ctx.error = false;
    return true;
}

static void http_close(void)
{
    if (http_ctx.client) {
        esp_http_client_close(http_ctx.client);
        esp_http_client_cleanup(http_ctx.client);
        http_ctx.client = NULL;
    }
}

/* ============================================================
 *  UART initialisation
 * ============================================================ */

void uart_comm_init(void)
{
    const int tx_buf_size = LCD_W * 2 + 64;


    uart_driver_install(UART_NUM_0, 1024, tx_buf_size, 0, NULL, 0);

    ESP_LOGI(TAG, "=== READY, waiting for trigger ===");
    fflush(stdout);

    uint8_t trigger = 0;
    uart_read_bytes(UART_NUM_0, &trigger, 1, portMAX_DELAY);

    ESP_LOGI(TAG, "Trigger 0x%02X — switching baud", trigger);
    fflush(stdout);

    vTaskDelay(pdMS_TO_TICKS(600));
    uart_wait_tx_done(UART_NUM_0, portMAX_DELAY);

    uart_config_t uart_cfg = {
        .baud_rate  = 921600,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    uart_param_config(UART_NUM_0, &uart_cfg);

    vTaskDelay(pdMS_TO_TICKS(100));

    img_header_t hdr = {
        .sync   = {0xAA, 0xAA, 0xAA},
        .magic  = 0xDEADBEEF,
        .width  = LCD_W,
        .height = LCD_H,
        .format = 0,
    };
    esp_log_level_set("*", ESP_LOG_NONE);
    vTaskDelay(pdMS_TO_TICKS(50));

    uart_write_bytes(UART_NUM_0, (const char *)&hdr, sizeof(hdr));
    uart_wait_tx_done(UART_NUM_0, portMAX_DELAY);
}

/* ============================================================
 *  Entry point
 * ============================================================ */

void app_main(void)
{
    ESP_LOGI(TAG, "URL: %s", JPEG_URL);

    /* NVS + netif — required by example_connect() */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());
    ESP_LOGI(TAG, "WiFi connected");

    /* UART sync */
    #ifndef DEBUG
    uart_comm_init();
    #endif

    /* JPEG decoder */
    jpeg_decoder_init();

    /* Open HTTP stream (headers only, body streams on demand) */
    if (!http_open(JPEG_URL)) {
        ESP_LOGE(TAG, "HTTP open failed");
        while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    /* Build view intent — reader fetches data incrementally via http_read_cb */
    jpeg_view_intent_t view = jpeg_view_default(LCD_W, LCD_H);
    view.out_format   = JPEG_OUTPUT_RGB565;
    view.chunk_buffer = chunk_buf;
    view.input_buffer = input_buf;
    view.scale        = JPEG_SCALE_AUTO;
    view.pan_x        = -100;
    view.pan_y        = -100;
    view.reader       = (jpeg_reader_t){
        .cb  = http_read_cb,
        .ctx = &http_ctx,
    };

    /*
     * Decode starts here. The decoder will call http_read_cb()
     * repeatedly, each time asking for only the bytes it needs.
     * Data flows: socket → http_read_cb → decoder → on_chunk → UART
     */
    jpeg_decoder_decode_view(
        &view,
        workbuf, sizeof(workbuf),
        on_chunk,
        on_done,
        NULL
    );

    http_close();

    esp_log_level_set("*", ESP_LOG_WARN);
    ESP_LOGI(TAG, "Done — %d bytes from HTTP", http_ctx.bytes_total);

    while (1) vTaskDelay(1000 / portTICK_PERIOD_MS);
}