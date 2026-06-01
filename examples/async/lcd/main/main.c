#include "esp_log.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "protocol_examples_common.h"
#include "jpeg_roi_decoder.h"
#include "lcd_init.h"
//#include "segger_sysview.h"

//#include "http_stream.h"
#include "lcd_output.h"
#include "pan_control.h"
//#include "jpeg_fetch.h"

static const char *TAG = "MAIN";

#define LCD_W  320
#define LCD_H  480

#define PAN_STEP_X   20
#define PAN_STEP_Y   20

#define RETRY_DELAY_MS   2000
#define MAX_FAIL_STREAK     5

#define JPEG_URL  CONFIG_JPEG_URL


static uint8_t  workbuf[JPEG_DECODER_WORK_BUF_DEFAULT];
static uint16_t chunk_buf[JPEG_CHUNK_BUF_PIXELS(LCD_W)] __attribute__((aligned(4))); 
//static uint8_t  input_buf[JPEG_INPUT_BUF_SIZE];
static uint8_t input_buf[JPEG_INPUT_BUF_SIZE] __attribute__((aligned(4)));

static TaskHandle_t main_task_handle = NULL;

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

    //ESP_LOGI(TAG, "about to read up to %d bytes", max);

    /* Normal read — decoder wants up to 'max' bytes into 'dst' */
    int r = esp_http_client_read(ctx->client, (char *)dst, max);
    //ESP_LOGI(TAG, "read %d bytes  , total buffer size: %d", r, max);


    //ESP_LOGI(TAG, "Reading %d bytes", r);
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

    //UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
    //ESP_LOGI(TAG, "Stack watermark: %u words remaining", watermark);

    #ifndef DEBUG

    bool ret=lcd_on_chunk(evt);

    return ret;

    
    
    #endif

    return true;
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
        // .timeout_ms = 10000,  <-- You can leave this here, but it won't fix the read hang
        
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

    // ========================================================
    // THE CRITICAL FIX: Forcefully set the READ timeout
    // This penetrates through to the mbedTLS layer
    // ========================================================
    esp_http_client_set_timeout_ms(http_ctx.client, 10000);

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

static void on_done(const jpeg_done_event_t *evt)
{

    ESP_LOGI(TAG,"closing");
    http_close();
    lcd_output_frame_begin();

    xTaskNotifyGive(main_task_handle);
}






static void wifi_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());
    ESP_LOGI(TAG, "WiFi connected");
}



void app_main(void)
{


     

    ESP_LOGI(TAG, "URL: %s", JPEG_URL);

    /* NVS + netif — required by example_connect() */

    wifi_init();

    
        /* LCD init */
    ESP_ERROR_CHECK(lcd_output_init());
    /* JPEG decoder */
    jpeg_decoder_init();

    pan_state_t p;

    pan_init(&p,
              30, 30,
              10, 10);


    main_task_handle = xTaskGetCurrentTaskHandle();

    /* Open HTTP stream (headers only, body streams on demand) */

    while (1) {

        pan_next(&p);

        ESP_LOGI(TAG, "Opening HTTP stream...");
        bool ret=http_open(JPEG_URL);
        if (ret) {

            
            ESP_LOGI(TAG, "Opened HTTP stream, starting decoder...");
            /* Build view intent — reader fetches data incrementally via http_read_cb */
            jpeg_view_intent_t view = jpeg_view_default(LCD_W, LCD_H);
            view.out_format   = JPEG_OUTPUT_RGB565;
            view.chunk_buffer = chunk_buf;
            view.input_buffer = input_buf;
            view.scale        = JPEG_SCALE_AUTO;
            view.pan_x        = p.x;
            view.pan_y        = p.y;
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
            //esp_log_level_set("*", ESP_LOG_WARN);
            ESP_LOGI(TAG, "Done — %d bytes from HTTP", http_ctx.bytes_total);


                //Wait for the decoder on done to start a new cycle
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }

            else {
                ESP_LOGE(TAG, "Failed to open HTTP stream, retrying in %d ms...", RETRY_DELAY_MS);
                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            }

        }
}