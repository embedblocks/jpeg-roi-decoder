#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_mount.h"
#include "esp_log.h"
#include "jpeg_roi_decoder.h"

static const char *TAG = "test-components";

#define WORK_BUF_SIZE 5000  // 64KB

#define LCD_WIDTH       320
#define LCD_HEIGHT      240

#define RGB565      //For host testing

#define ROW_BUFFER_SIZE         (LCD_WIDTH +5)            //RGB565 format for LVGL, +
                                                        //5 because nav component has problems and creates roi slightly above required
#define COL_BUFFER_SIZE         (LCD_HEIGHT+5)             //RGB565 format for LVGL, +
                                                        //5 because nav component has problems and creates roi slightly above required

#ifdef RGB888
    #define OUTPUT_BUFFER_SIZE  ((ROW_BUFFER_SIZE)*COL_BUFFER_SIZE*3)
#elif defined(RGB565)           //For lvgl
    #define OUTPUT_BUFFER_SIZE   (ROW_BUFFER_SIZE)*(COL_BUFFER_SIZE)*2  // Note: RGB565 is 2 bytes per pixel
#endif



static uint16_t g_out_w  = 0;
static uint16_t g_out_h  = 0;


static uint8_t work_buff[WORK_BUF_SIZE] = { 0 };

static uint8_t g_output[OUTPUT_BUFFER_SIZE] = { 0 };

static uint16_t row_buffer[ROW_BUFFER_SIZE] = { 0 };





/* Example user data structure */

/*
    typedef struct {
    int request_id;
    const char *filename;
    FILE *fp;    
    size_t data_size;
    void* buffer;

} decode_user_data_t;
*/
static bool test_chunk_cb(const jpeg_chunk_event_t *evt)
{
  //  ESP_LOGI("TEST", "chunk_cb called: %u bytes", evt->byte_count);
    // Write decoded pixels directly to file, user_date is the output FILE*
    fwrite(evt->pixels, evt->byte_count, 1, evt->user_data);
    return true;  // continue decoding
}


static void test_done_cb(const jpeg_done_event_t *evt)
{
    ESP_LOGI("TEST", "Decode finished, result=%d", evt->result);

    if (evt->fp) {
        fclose(evt->fp);
        fclose(evt->user_data);  // user_data is the output FILE*
        ESP_LOGI("TEST", "Output file closed");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "JPEG ROI decode test starting");

    char str1[]="hello how are you";
    char str2[]="my name is esp32";

    sd_mount_init();

    
    jpeg_decoder_init();

    FILE *in = fopen("/sdcard/image2.jpg", "r+b");
    if (!in) {
        ESP_LOGE("APP", "Failed to open input image");
        return;
    }

    FILE *out = fopen("/sdcard/output2.raw", "wb");
    if (!out) {
        fclose(in);
        ESP_LOGE("APP", "Failed to open output file");
        return;
    }

    // ROI example: full image (you can crop)

    uint16_t left,top,right,bottom;
    left = 262; 
    top = 592;
    right = 2821;  
    bottom = 2511;
    
    jpeg_roi_t roi = {
        .left = left,
        .top = top,
        .right = right,
        .bottom = bottom
    };

    uint8_t scale = 3;  // 1/8 scale

jpeg_image_info_t info;



    if (jpeg_decoder_probe(in, &info, work_buff, WORK_BUF_SIZE) != JPEG_DECODE_OK) {
        fprintf(stderr, "ERROR: Probe failed\n");
        fclose(in);
        return;
    }

    fprintf(stderr, "STEP 2: after probe\n");
    rewind(in);


    uint16_t scaled_w = info.width  >> scale;
    uint16_t scaled_h = info.height >> scale;

    fprintf(stderr,
        "Image: %dx%d | Scale: 1/%d | Scaled: %dx%d\n",
        info.width,
        info.height,
        (1 << scale),
        scaled_w,
        scaled_h
    );

    /* --------------------------------------------------
       Validate ROI in scaled space
    -------------------------------------------------- */

/*
    if (right >= info.width || bottom >= info.height ||
    left > right || top > bottom) {

        fprintf(stderr,
            "ERROR: ROI invalid.\n"
            "ROI: [%u,%u,%u,%u]\n"
            "Image bounds: [0..%u, 0..%u]\n",
            left, top, right, bottom,
            info.width - 1,
            info.height - 1
        );

        fclose(in);
        return;
    }

*/

    uint16_t scale_div = 1 << scale;

    uint16_t scaled_left  = left   / scale_div;
    uint16_t scaled_right = right  / scale_div;
    uint16_t scaled_top   = top    / scale_div;
    uint16_t scaled_bot   = bottom / scale_div;

    uint16_t roi_w = scaled_right - scaled_left + 1;
    uint16_t roi_h = scaled_bot   - scaled_top  + 1;


    g_out_w = roi_w;
    g_out_h = roi_h;

    /*
    fprintf(stderr,
    "MAIN DEBUG:\n"
    "  ROI original: [%u,%u,%u,%u]\n"
    "  Allocated output size: %ux%u\n",
    left, top, right, bottom,
    g_out_w, g_out_h
    );*/

    /* --------------------------------------------------
       Build decode request
    -------------------------------------------------- */

    fprintf(stderr,"leaving");
    jpeg_decode_request_t req = {
        .fp = in,
        .roi = roi,
        .scale = (jpeg_decode_scale_t)scale,
        .work_buffer=work_buff,
        .work_buffer_size=WORK_BUF_SIZE,
        .chunk_buffer = row_buffer,
        .chunk_buffer_pixels = roi_w,
        .chunk_callback = test_chunk_cb,
         .done_callback  = test_done_cb,
        .user_data = out
    };

    /* --------------------------------------------------
       Decode
    -------------------------------------------------- */

    jpeg_decode_result_t res = jpeg_decoder_decode(&req);
    // In main, after decode attempt:
    // Then check after decode:


    if (res != JPEG_DECODE_OK) {
        //fprintf(stderr, "ERROR: Decode failed (%d)\n", res);
        fclose(in);
        fclose(out);
        return; //EXIT_DECODE_FAIL;
    }



    /* --------------------------------------------------
       Output PPM
    -------------------------------------------------- */
/*  
     printf("P6\n%d %d\n255\n", roi_w, roi_h);
    fwrite(g_output, 1, (size_t)(roi_w*roi_h*3), stdout);       //(roi_w*roi_h*3)
    size_t written = fwrite(g_output, 1, (size_t)(roi_w*roi_h*3), out);
    printf("fwrite returned: %zu (expected: %zu)\n", written, (size_t)(roi_w*roi_h*3));

    if (written == 0) {
        perror("fwrite failed");  // This will tell you WHY (disk full, permission, etc.)
    }

    // Check if file stream is OK
    printf("File stream error: %d\n", ferror(out));
    printf("File stream EOF: %d\n", feof(out));
    clearerr(out);  // Clear any error state
    printf("g_output address: %p\n", (void*)g_output);

// Try to read first byte - this will crash if pointer is invalid
// If it crashes here, that's your problem
    printf("First byte value: %d\n", g_output[0]);

    fprintf(stderr,"exiting");*/
    //fclose(in);
    //fclose(out);




    
    while (1) {
        //ESP_LOGI(TAG, "Queue depth: %d", jpeg_decoder_get_queue_depth());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    
    /* Cleanup (unreachable in this example) */
    //free(output_buffer);
    //free(output_buffer2);
    jpeg_decoder_deinit();
}

