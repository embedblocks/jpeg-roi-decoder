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



/* Embedded JPEG */
extern const uint8_t test_jpg_start[] asm("_binary_flower_jpg_start");
extern const uint8_t test_jpg_end[]   asm("_binary_flower_jpg_end");

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

    
    static int count=0;
    //ESP_LOGI("CHK", "offset=%d chunk=%d", count, evt->byte_count);
    
    //size_t written= uart_write_bytes(UART_NUM_0, (const char*)&heatmap_map[count], evt->byte_count);
    //count=count+evt->byte_count;
    
    size_t written= uart_write_bytes(UART_NUM_0, (const char*)evt->pixels, evt->byte_count);
    uart_wait_tx_done(UART_NUM_0, portMAX_DELAY);
    
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


void uart_comm_init(){

    // Install driver FIRST — keeps VFS console working
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    uart_vfs_dev_use_driver(0);      // new

    // ESP_LOGI still works, AND uart_read_bytes now works
    ESP_LOGI(TAG, "=== READY, waiting for trigger ===");
    fflush(stdout);

    uint8_t trigger = 0;
    uart_read_bytes(UART_NUM_0, &trigger, 1, portMAX_DELAY);  // truly blocking
    ESP_LOGI(TAG, "Trigger 0x%02X received — switching baud", trigger);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(600));  // let that log line finish at 115200

    // Now switch to 921600
    uart_config_t uart_cfg = {
        .baud_rate = 921600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(UART_NUM_0, &uart_cfg);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Send header + pixels
    img_header_t hdr = {
        .magic  = 0xDEADBEEF,
        .width  = LCD_W,
        .height = LCD_H,
        .format = 0,
    };
    uart_write_bytes(UART_NUM_0, (const char*)&hdr, sizeof(hdr));

}
/* -------------------------------------------------------
 * app_main
 * ------------------------------------------------------- */
void app_main(void)
{
    
    uart_comm_init();
    jpeg_decoder_init();

    static jpeg_source_t src;   /* static — ctx points into struct */
    jpeg_decoder_source_from_buffer(&src, test_jpg_start,
                                    test_jpg_end - test_jpg_start);

    jpeg_view_t view  = jpeg_view_default(LCD_W, LCD_H);
    view.out_format   = JPEG_OUTPUT_RGB565;
    view.chunk_buffer = chunk_buf;   /* provide buffer — no malloc inside */

    view.scale= JPEG_SCALE_1_1;  // no scaling

    view.pan_x= 10; // pan right by 10 pixels
    view.pan_y= 10; // pan up by 10 pixels
    

    
    //- 6. Decode — rows stream via on_chunk() --- 
    jpeg_decoder_decode_view(
        src,
        &view,
        workbuf, sizeof(workbuf),
        on_chunk,
        on_done,
        NULL
    );

    
    /* --- 7. Re-enable logs, done streaming --- */
    //esp_log_level_set("*", ESP_LOG_INFO);
        ESP_LOGI(TAG, "Streaming finished");

}
