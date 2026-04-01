
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "jpeg_roi_decoder.h"
#include "driver/uart.h"
#include "esp_log.h"

#define TAG "JPEG_UART"

#define LCD_W 320
#define LCD_H 240

#define UART_PORT UART_NUM_0
#define UART_TX   17
#define UART_RX   16
#define BAUD_RATE 921600

// Embedded JPEG 

    extern const uint8_t test_jpg_start[] asm("_binary_test_jpg_start");
    extern const uint8_t test_jpg_end[]   asm("_binary_test_jpg_end");


//Work buffer 
static uint8_t workbuf[JPEG_DECODER_WORK_BUF_DEFAULT];

// Row buffer (ONLY ONE ROW!) 
static uint16_t rowbuf[LCD_W];

// Stream header 
typedef struct {
    uint32_t magic;
    uint16_t width;
    uint16_t height;
    uint8_t  format;   // 0 = RGB565
} img_header_t;

// Context 
typedef struct {
    int uart;
} stream_ctx_t;

/// Chunk callback
static bool on_chunk(const jpeg_chunk_event_t *evt)
{
    stream_ctx_t *ctx = (stream_ctx_t*)evt->user_data;

    if (evt->width != LCD_W){

        ESP_LOGE(TAG, "Invalid chunk size (%u)",
                 evt->width);
             //    evt->chunk->height);
        return false;
    }

    //uart_write_bytes(ctx->uart,
      //               (const char*)evt->pixels,
        //             evt->byte_count);

    

    write(1, evt->pixels, evt->byte_count);

    return true;
}
// Done callback 
static void on_done(const jpeg_done_event_t *evt)
{
    if (evt->result != JPEG_DECODE_OK) {
        ESP_LOGE(TAG, "Decode failed: %s",
                 jpeg_decoder_err_to_str(evt->result));
    } else {
        ESP_LOGI(TAG, "Decode complete");
    }
}

static void uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate = 921600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 4096, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT,
                                UART_TX,
                                UART_RX,
                                UART_PIN_NO_CHANGE,
                                UART_PIN_NO_CHANGE));
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting JPEG UART stream example");

    uart_init();
    jpeg_decoder_init();


    size_t jpg_size = test_jpg_end - test_jpg_start;

    jpeg_source_t src;
    jpeg_decoder_source_from_buffer(
        &src,
        test_jpg_start,
        jpg_size
    );

    jpeg_view_t view = jpeg_view_default(LCD_W, LCD_H);
    view.out_format = JPEG_OUTPUT_RGB565;
    


  //Send header 


    img_header_t hdr = {
        .magic  = 0xDEADBEEF,
        .width  = LCD_W,
        .height = LCD_H,
        .format = 0
    };


    uint8_t start;

    ESP_LOGI(TAG, "Waiting for PC trigger...");

    uart_read_bytes(UART_PORT, &start, 1, portMAX_DELAY);

    ESP_LOGI(TAG, "Trigger received, starting stream");
    //uart_write_bytes(UART_PORT,
      //               (const char*)&hdr,
        //             sizeof(hdr));

    write(1, &hdr, sizeof(hdr));
    stream_ctx_t ctx = {
        .uart = UART_PORT
    };

    jpeg_decoder_decode_view(
        src,
        &view,
        workbuf, sizeof(workbuf),
        on_chunk,
        on_done,
        &ctx
    );

    ESP_LOGI(TAG, "Streaming finished");
}



/*
#include <stdio.h>
#include <unistd.h>
#include "esp_log.h"

#define TAG "TEST"

// Send raw bytes 
static void send_bytes(const uint8_t *data, size_t len)
{
    write(1, data, len);
}

// Print hex for verification 
static void print_hex(const uint8_t *data, size_t len)
{
    printf("\nHEX: ");
    for (int i = 0; i < len; i++) {
        printf("%02X ", data[i]);
    }
    printf("\n");
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting test");

    //vTaskDelay(pdMS_TO_TICKS(2000));

    // Magic number 
    uint32_t magic = 0xDEADBEEF;

    // Send raw 
    send_bytes((uint8_t*)&magic, sizeof(magic));

    // Print readable verification 
    print_hex((uint8_t*)&magic, sizeof(magic));

    // Check correctness 
    uint8_t *m = (uint8_t*)&magic;

    if (m[0] == 0xEF &&
        m[1] == 0xBE &&
        m[2] == 0xAD &&
        m[3] == 0xDE) {

        ESP_LOGI(TAG, "MAGIC VERIFIED (little endian)");
    } else {
        ESP_LOGE(TAG, "MAGIC WRONG");
    }

    ESP_LOGI(TAG, "Done");
}*/