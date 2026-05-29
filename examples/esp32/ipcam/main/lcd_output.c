#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lcd_output.h"
#include "lcd_init.h"
#include "esp_log.h"

static const char *TAG = "LCD_OUTPUT";

#define RGB565(r,g,b) __builtin_bswap16((uint16_t)((((r)&0xF8)<<8)|(((g)&0xFC)<<3)|((b)>>3)))

static esp_lcd_panel_handle_t s_panel       = NULL;
static int                    s_current_row = 0;
static SemaphoreHandle_t      signal_color_done  = NULL;



static bool on_color_trans_done_cb(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *ctx){

   
    xSemaphoreGive(signal_color_done);
    return true;


                                }

/* ── Init ─────────────────────────────────────────────────────────────────── */

esp_err_t lcd_output_init()
{
    esp_err_t ret=0;
    s_current_row = 0;
    signal_color_done=xSemaphoreCreateBinary();
    if (signal_color_done == NULL) {
        ESP_LOGE(TAG, "Failed to create semaphore");    
        return ESP_FAIL;
    }
    xSemaphoreGive(signal_color_done); // Start "available"

    
    
    ret=ili9486_display_init(on_color_trans_done_cb);
    if(ret!=ESP_OK)
        return ESP_FAIL;

    s_panel = ili9486_display_get_panel();
    return ESP_OK;

}

/* ── Frame begin ─────────────────────────────────────────────────────────── */

void lcd_output_frame_begin(void)
{
    s_current_row = 0;
}

/* ── Chunk callback ───────────────────────────────────────────────────────── */

bool lcd_on_chunk(const jpeg_chunk_event_t *evt)
{
    if (!s_panel) return false;

    xSemaphoreTake(signal_color_done, portMAX_DELAY);

    uint16_t *pixels = (uint16_t *)evt->pixels;

    
    for (int i = 0; i < evt->width; i++) {
        pixels[i] = __builtin_bswap16(pixels[i]);
    }

   ESP_LOGI(TAG, "Drawing bitmap: (%d, %d) - (%d, %d), size: %d bytes",
             evt->x, evt->y, evt->x + evt->width, evt->y + 1, evt->width * 2);
    // Just draw as normal portrait row — swap_xy handles the rotation
    esp_lcd_panel_draw_bitmap(
        s_panel,
        0,           evt->y,
        evt->width,  evt->y + 1,
        evt->pixels
    );

    return true;
}/* ── Done callback ────────────────────────────────────────────────────────── */

void lcd_on_done(const jpeg_done_event_t *evt)
{
    //ESP_LOGD(TAG, "Frame complete — %d rows drawn", s_current_row);
    ESP_LOGI(TAG, "Frame complete");
    (void)evt;
}
