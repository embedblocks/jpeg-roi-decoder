#include <unistd.h>
#include <string.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpeg_roi_decoder.h"

#define TAG     "JPEG_UART"
#define LCD_W   320
#define LCD_H   240

/* Embedded JPEG */
extern const uint8_t test_jpg_start[] asm("_binary_test_jpg_start");
extern const uint8_t test_jpg_end[]   asm("_binary_test_jpg_end");

/* Work buffer */
static uint8_t workbuf[JPEG_DECODER_WORK_BUF_DEFAULT];

/*
 * Chunk buffer — must hold JPEG_MCU_MAX_HEIGHT (16) full rows.
 * TJpgDec delivers MCU blocks left-to-right before moving down,
 * so each row needs its own slot in the buffer.
 * Use JPEG_CHUNK_BUF_PIXELS() to get the correct size.
 */
static uint16_t chunk_buf[JPEG_CHUNK_BUF_PIXELS(LCD_W)];

/* Stream header */
typedef struct {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint8_t  format;   /* 0 = RGB565 */
} __attribute__((packed)) img_header_t;

/* -------------------------------------------------------
 * on_chunk — called once per decoded row
 * ------------------------------------------------------- */
static bool on_chunk(const jpeg_chunk_event_t *evt)
{
    if (evt->width != LCD_W) {
        /* Can't use ESP_LOGE here — logs corrupt the binary stream! */
        return false;
    }
    ssize_t written = write(1, evt->pixels, evt->byte_count);
    if (written != (ssize_t)evt->byte_count) {
        return false;
    }
    return true;
}

/* -------------------------------------------------------
 * on_done — called when decode finishes
 * ------------------------------------------------------- */
static void on_done(const jpeg_done_event_t *evt)
{
    /* !! Do NOT write() any text here — it would corrupt the binary stream */
    /* !! Do NOT ESP_LOGI here — stdout = fd1 = same pipe as image data    */
    (void)evt;
}

/* -------------------------------------------------------
 * app_main
 * ------------------------------------------------------- */
void app_main(void)
{
    /* --- 1. Reconfigure console UART to 921600 --- */
    uart_config_t uart_config = {
        .baud_rate  = 921600,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(UART_NUM_0, &uart_config);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* --- 2. Signal ready and wait for trigger --- */
    ESP_LOGI(TAG, "=== READY, waiting for trigger ===");
    fflush(stdout);
    uint8_t start = 0;
    read(0, &start, 1);
    ESP_LOGI(TAG, "Trigger received (0x%02X), starting stream", start);

    /* --- 3. Send binary header --- */
    img_header_t hdr = {
        .magic  = 0xDEADBEEF,
        .width  = LCD_W,
        .height = LCD_H,
        .format = 0,
    };
    write(1, &hdr, sizeof(hdr));

    /* --- 4. Silence ALL logs before binary streaming ---
     *        ESP_LOGI/LOGE write to stdout (fd 1) which is the same
     *        pipe Python reads — any log text corrupts the image!    */
    esp_log_level_set("*", ESP_LOG_NONE);

    /* --- 5. Init decoder and set up source --- */
    jpeg_decoder_init();

    static jpeg_source_t src;   /* static — ctx points into struct */
    jpeg_decoder_source_from_buffer(&src, test_jpg_start,
                                    test_jpg_end - test_jpg_start);

    jpeg_view_t view  = jpeg_view_default(LCD_W, LCD_H);
    view.out_format   = JPEG_OUTPUT_RGB565;
    view.chunk_buffer = chunk_buf;   /* provide buffer — no malloc inside */

    /* --- 6. Decode — rows stream via on_chunk() --- */
    jpeg_decoder_decode_view(
        src,
        &view,
        workbuf, sizeof(workbuf),
        on_chunk,
        on_done,
        NULL
    );

    /* --- 7. Re-enable logs, done streaming --- */
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "Streaming finished");
}