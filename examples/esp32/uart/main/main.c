#include <unistd.h>
#include <string.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h" // ← with this one
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpeg_roi_decoder.h"
//#include "heatmap.h" 

#define TAG     "JPEG_UART"
#define LCD_W   320
#define LCD_H   240

#define UART_SENT 1   // 1 = enable, 0 = disable


///#define DEBUG 1

/* Embedded JPEG */
extern const uint8_t test_jpg_start[] asm("_binary_test_image_jpg_start");
extern const uint8_t test_jpg_end[]   asm("_binary_test_image_jpg_end");

static uint8_t workbuf[JPEG_DECODER_WORK_BUF_DEFAULT];
static uint16_t chunk_buf[JPEG_CHUNK_BUF_PIXELS(LCD_W)];

typedef struct {
    uint8_t  sync[3];     // 0xAA 0xAA 0xAA - sync pattern
    uint32_t magic;       // 0xDEADBEEF
    uint16_t width;
    uint16_t height;
    uint8_t  format;
} __attribute__((packed)) img_header_t;

static bool on_chunk(const jpeg_chunk_event_t *evt)
{
    if (evt->width != LCD_W) {
        ESP_LOGI(TAG, "Unexpected width %d", evt->width);
        return false;
    }
    
    //ESP_LOGI(TAG, "Decoded byte count %d",evt->byte_count);
    // Use uart_write_bytes_with_break to ensure clean transmission

    #ifndef DEBUG
    size_t written = uart_write_bytes(UART_NUM_0, (const char*)evt->pixels, evt->byte_count);
    
    if (written != evt->byte_count) {
        return false;
    }
    
    // Wait for TX complete BEFORE returning
    uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(1000));
    #endif
    
    return true;
}

static void on_done(const jpeg_done_event_t *evt)
{
    (void)evt;
}

void uart_comm_init()
{
    // FIXED: Increase TX buffer size to hold at least one full row
    // LCD_W * 2 bytes per pixel, rounded up
    const int tx_buf_size = LCD_W * 2 + 64;
    
    uart_driver_install(UART_NUM_0, 1024, tx_buf_size, 0, NULL, 0);
    uart_vfs_dev_use_driver(0);
    
    ESP_LOGI(TAG, "=== READY, waiting for trigger ===");
    fflush(stdout);
    
    uint8_t trigger = 0;
    uart_read_bytes(UART_NUM_0, &trigger, 1, portMAX_DELAY);
    
    ESP_LOGI(TAG, "Trigger 0x%02X received — switching baud", trigger);
    fflush(stdout);
    
    // Wait for log to fully transmit at 115200
    vTaskDelay(pdMS_TO_TICKS(600));
    
    // Flush any remaining data before switching
    uart_wait_tx_done(UART_NUM_0, portMAX_DELAY);
    
    // Now switch to 921600
    uart_config_t uart_cfg = {
        .baud_rate = 921600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,  // Explicit clock source
    };
    uart_param_config(UART_NUM_0, &uart_cfg);
    
    // IMPORTANT: Small delay after baud switch for stabilization
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Send header with sync pattern
    img_header_t hdr = {
        .sync   = {0xAA, 0xAA, 0xAA},
        .magic  = 0xDEADBEEF,
        .width  = LCD_W,
        .height = LCD_H,
        .format = 0,
    };
    esp_log_level_set("*", ESP_LOG_NONE);
    // Wait to ensure Python has switched baud rate
    vTaskDelay(pdMS_TO_TICKS(50));
    
    uart_write_bytes(UART_NUM_0, (const char*)&hdr, sizeof(hdr));
    uart_wait_tx_done(UART_NUM_0, portMAX_DELAY);
}

void app_main(void)
{
    #ifndef DEBUG
    uart_comm_init();
    #endif
    jpeg_decoder_init();
    
    static jpeg_source_t src;
    jpeg_decoder_source_from_buffer(&src, test_jpg_start,
                                    test_jpg_end - test_jpg_start);
    
    jpeg_view_t view = jpeg_view_default(LCD_W, LCD_H);
    view.out_format   = JPEG_OUTPUT_RGB565;
    view.chunk_buffer = chunk_buf;
    view.scale        = JPEG_SCALE_AUTO;
    view.pan_x        = -100;
    view.pan_y        = -100;
    
    jpeg_decoder_decode_view(
        src,
        &view,
        workbuf, sizeof(workbuf),
        on_chunk,
        on_done,
        NULL
    );
    
    ESP_LOGI(TAG, "Streaming finished");
    while(1){
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
    
}