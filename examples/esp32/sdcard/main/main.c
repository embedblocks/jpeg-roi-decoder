#include <unistd.h>
#include <string.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h" // ← with this one
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpeg_roi_decoder.h"
#include "sd_mount.h"
//#include "heatmap.h" 

#define TAG     "JPEG_UART"
#define LCD_W   320
#define LCD_H   240

#define UART_SENT 1   // 1 = enable, 0 = disable



/* Embedded JPEG */
//extern const uint8_t test_jpg_start[] asm("_binary_flower_jpg_start");
//extern const uint8_t test_jpg_end[]   asm("_binary_flower_jpg_end");

/* Work buffer */
static uint8_t workbuf[JPEG_DECODER_WORK_BUF_DEFAULT];

/*
 * Chunk buffer — must hold JPEG_MCU_MAX_HEIGHT (16) full rows.
 * TJpgDec delivers MCU blocks left-to-right before moving down,
 * so each row needs its own slot in the buffer.
 * Use JPEG_CHUNK_BUF_PIXELS() to get the correct size.
 */
static uint16_t chunk_buf[JPEG_CHUNK_BUF_PIXELS(LCD_W)];


/* -------------------------------------------------------
 * on_chunk — called once per decoded row
 * ------------------------------------------------------- */
static bool on_chunk(const jpeg_chunk_event_t *evt)
{
    if (evt->width != LCD_W) {
        /* Can't use ESP_LOGE here — logs corrupt the binary stream! */
        return false;
    }

   
    ESP_LOGI(TAG, "Decoded byte count %d",evt->byte_count);
    int ret = fwrite(evt->pixels, evt->byte_count, 1, (FILE*)evt->user_data);
        
    if (ret != 1) {
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
    fclose((FILE*)evt->user_data);
}



void app_main(void)
{
    
    sd_mount_init();
    jpeg_decoder_init();

    FILE* fin = fopen("/sdcard/flower.jpg", "rb");

    if (!fin) {
        ESP_LOGE("APP", "Failed to open input image");
        return;
    }

    FILE *fout = fopen("/sdcard/rgb565.raw", "wb");
    if (!fout) {
        fclose(fin);
        ESP_LOGE("APP", "Failed to open output file");
        return;
    }

    
    jpeg_source_t src=jpeg_decoder_source_from_file(fin);

    jpeg_view_t view  = jpeg_view_default(LCD_W, LCD_H);
    view.out_format   = JPEG_OUTPUT_RGB565;
    view.chunk_buffer = chunk_buf;   /* provide buffer — no malloc inside */
    
    
    //- 6. Decode — rows stream via on_chunk() --- 
    jpeg_decoder_decode_view(
        src,
        &view,
        workbuf, sizeof(workbuf),
        on_chunk,
        on_done,
        (void*)fout   /* user_data, passed to callbacks — here we pass the output FILE* */
    );

    
    /* --- 7. Re-enable logs, done streaming --- */
    //esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "Streaming finished");

    while(1){
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }


}
