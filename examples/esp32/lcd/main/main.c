#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "protocol_examples_common.h"

#include "jpeg_roi_decoder.h"
#include "lcd_init.h"

#include "http_stream.h"
#include "lcd_output.h"
#include "pan_control.h"
#include "jpeg_fetch.h"

static const char *TAG = "MAIN";

#define LCD_W  320
#define LCD_H  480

#define PAN_X_MAX   600
#define PAN_Y_MAX   400
#define PAN_STEP_X   20
#define PAN_STEP_Y   20

#define RETRY_DELAY_MS   2000
#define MAX_FAIL_STREAK     5
#define DECODE_TIMEOUT_MS  30000

#define JPEG_URL  CONFIG_JPEG_URL

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

static void lcd_init(void)
{
    ESP_ERROR_CHECK(ili9486_display_init());
    esp_lcd_panel_handle_t panel = ili9486_display_get_panel();
    if (!panel) {
        ESP_LOGE(TAG, "Failed to get panel handle");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    lcd_output_init(panel);
}

void app_main(void)
{
    wifi_init();
    lcd_init();
    jpeg_decoder_init();

    if (!jpeg_fetch_init()) {
        ESP_LOGE(TAG, "jpeg_fetch_init failed");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    http_stream_ctx_t http;
    if (!http_stream_init(&http, JPEG_URL)) {
        ESP_LOGE(TAG, "http_stream_init failed");
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    pan_state_t pan;
    pan_init(&pan, PAN_X_MAX, PAN_Y_MAX, PAN_STEP_X, PAN_STEP_Y);

    ESP_LOGI(TAG, "Starting pan loop — %s", JPEG_URL);

    int fail_streak = 0;

    while (1) {
        jpeg_fetch_params_t params = {
            .url   = JPEG_URL,
            .lcd_w = LCD_W,
            .lcd_h = LCD_H,
            .pan_x = pan.x,
            .pan_y = pan.y,
        };

        /* ── Fire ── */
        bool started = jpeg_fetch_start(&http, &params);

        /* ── Wait ── (explicitly visible — main does not proceed until done) */
        bool ok = started && jpeg_fetch_wait(&http, DECODE_TIMEOUT_MS);

        /* ── Advance ── */
        if (ok) {
            fail_streak = 0;
            pan_next(&pan);
        } else {
            fail_streak++;
            ESP_LOGW(TAG, "Frame failed (streak %d/%d)", fail_streak, MAX_FAIL_STREAK);

            if (fail_streak >= MAX_FAIL_STREAK) {
                ESP_LOGE(TAG, "Reinitialising HTTP client");
                http_stream_deinit(&http);
                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
                http_stream_init(&http, JPEG_URL);
                pan_init(&pan, PAN_X_MAX, PAN_Y_MAX, PAN_STEP_X, PAN_STEP_Y);
                fail_streak = 0;
            } else {
                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            }
        }
    }
}