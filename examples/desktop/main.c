#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "jpeg_roi_decoder.h"

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




/* -------------------------------------------------- */
/* Exit codes                                         */
/* -------------------------------------------------- */

enum {
    EXIT_OK = 0,
    EXIT_BAD_ARGS = 1,
    EXIT_OPEN_FAIL = 2,
    EXIT_PROBE_FAIL = 3,
    EXIT_ROI_INVALID = 4,
    EXIT_ALLOC_FAIL = 5,
    EXIT_DECODE_FAIL = 6
};

/* -------------------------------------------------- */
/* Global output buffer                               */
/* -------------------------------------------------- */


static uint16_t g_out_w  = 0;
static uint16_t g_out_h  = 0;


static uint8_t work_buff[WORK_BUF_SIZE] = { 0 };

static uint8_t g_output[OUTPUT_BUFFER_SIZE] = { 0 };

static uint16_t row_buffer[ROW_BUFFER_SIZE] = { 0 };


/* -------------------------------------------------- */
/* RGB565 -> RGB888 chunk callback                    */
/* -------------------------------------------------- */

static bool chunk_cb(const jpeg_chunk_event_t *evt)
{
    const uint16_t *src = evt->pixels;
    uint16_t y = evt->chunk->y;
    uint16_t x = evt->chunk->x;
    uint16_t w = evt->chunk->width;

    // Bounds checking
    if (y >= g_out_h) {
        fprintf(stderr, "ERROR: y overflow: %u >= %u\n", y, g_out_h);
        return false;
    }
    if ((x + w) > g_out_w) {
        fprintf(stderr, "ERROR: x overflow: x=%u w=%u g_out_w=%u\n", x, w, g_out_w);
        return false;
    }

#ifdef RGB888  // Windows/Host testing
    // Convert RGB565 → RGB888
    for (uint16_t i = 0; i < w; i++) {
        uint16_t p = src[i];
        uint8_t r = ((p >> 11) & 0x1F) << 3;
        uint8_t g = ((p >> 5)  & 0x3F) << 2;
        uint8_t b = ( p        & 0x1F) << 3;

        size_t idx = ((size_t)y * g_out_w + (x + i)) * 3;
        g_output[idx + 0] = r;
        g_output[idx + 1] = g;
        g_output[idx + 2] = b;
    }
#else  // RGB565 - ESP32/LVGL
    // Direct copy - no conversion needed!
    size_t idx = (size_t)y * g_out_w + x;
    uint16_t *dest = (uint16_t*)g_output;  // Cast to uint16_t*
    memcpy(&dest[idx], src, w * sizeof(uint16_t));
#endif

    return true;
}
static void done_cb(const jpeg_done_event_t *evt)
{
    if (evt->result != JPEG_DECODE_OK) {
        fprintf(stderr, "Decoder internal error: %d\n", evt->result);
    }
}

/* -------------------------------------------------- */
/* Main                                               */
/* -------------------------------------------------- */

int main(int argc, char *argv[])
{
    if (argc != 7) {
        fprintf(stderr,
            "Usage: %s <image.jpg> <left> <top> <right> <bottom> <scale>\n"
            "scale: 0=1/1, 1=1/2, 2=1/4, 3=1/8\n",
            argv[0]);
        return EXIT_BAD_ARGS;
    }

    const char *filename = argv[1];
    uint16_t left   = atoi(argv[2]);
    uint16_t top    = atoi(argv[3]);
    uint16_t right  = atoi(argv[4]);
    uint16_t bottom = atoi(argv[5]);
    uint8_t scale   = atoi(argv[6]);

    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "ERROR: Cannot open %s\n", filename);
        return EXIT_OPEN_FAIL;
    }


    FILE *fpw = fopen("output2.raw", "wb");
    if (!fpw) {
        fprintf(stderr, "ERROR: Cannot open %s\n", filename);
        return EXIT_OPEN_FAIL;
    }

    fprintf(stderr, "STEP 1: opened file\n");
    /* --------------------------------------------------
       Probe image size
    -------------------------------------------------- */



    jpeg_image_info_t info;



    if (jpeg_decoder_probe(fp, &info, work_buff, WORK_BUF_SIZE) != JPEG_DECODE_OK) {
        fprintf(stderr, "ERROR: Probe failed\n");
        fclose(fp);
        return EXIT_PROBE_FAIL;
    }

    fprintf(stderr, "STEP 2: after probe\n");
    rewind(fp);


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

        fclose(fp);
        return EXIT_ROI_INVALID;
    }



    uint16_t scale_div = 1 << scale;

    uint16_t scaled_left  = left   / scale_div;
    uint16_t scaled_right = right  / scale_div;
    uint16_t scaled_top   = top    / scale_div;
    uint16_t scaled_bot   = bottom / scale_div;

    uint16_t roi_w = scaled_right - scaled_left + 1;
    uint16_t roi_h = scaled_bot   - scaled_top  + 1;


    g_out_w = roi_w;
    g_out_h = roi_h;

    fprintf(stderr,
    "MAIN DEBUG:\n"
    "  ROI original: [%u,%u,%u,%u]\n"
    "  Allocated output size: %ux%u\n",
    left, top, right, bottom,
    g_out_w, g_out_h
    );

    /* --------------------------------------------------
       Build decode request
    -------------------------------------------------- */

    fprintf(stderr,"leaving");
    jpeg_decode_request_t req = {
        .fp = fp,
        .roi = { left, top, right, bottom },
        .scale = (jpeg_decode_scale_t)scale,
        .work_buffer=work_buff,
        .work_buffer_size=WORK_BUF_SIZE,
        .chunk_buffer = row_buffer,
        .chunk_buffer_pixels = roi_w,
        .chunk_callback = chunk_cb,
        .done_callback  = done_cb,
        .user_data = NULL
    };

    /* --------------------------------------------------
       Decode
    -------------------------------------------------- */

    jpeg_decode_result_t res = jpeg_decoder_decode(&req);
    // In main, after decode attempt:
    // Then check after decode:


    if (res != JPEG_DECODE_OK) {
        fprintf(stderr, "ERROR: Decode failed (%d)\n", res);
        fclose(fp);
        return EXIT_DECODE_FAIL;
    }



    /* --------------------------------------------------
       Output PPM
    -------------------------------------------------- */

    printf("P6\n%d %d\n255\n", roi_w, roi_h);
    //fwrite(g_output, 1, (size_t)(roi_w*roi_h*3), stdout);       //(roi_w*roi_h*3)
    size_t written = fwrite(g_output, 1, (size_t)(roi_w*roi_h*3), fpw);
    printf("fwrite returned: %zu (expected: %zu)\n", written, (size_t)(roi_w*roi_h*3));

    if (written == 0) {
        perror("fwrite failed");  // This will tell you WHY (disk full, permission, etc.)
    }

    // Check if file stream is OK
    printf("File stream error: %d\n", ferror(fpw));
    printf("File stream EOF: %d\n", feof(fpw));
    clearerr(fpw);  // Clear any error state
    printf("g_output address: %p\n", (void*)g_output);

// Try to read first byte - this will crash if pointer is invalid
// If it crashes here, that's your problem
    printf("First byte value: %d\n", g_output[0]);

    fprintf(stderr,"exiting");
    fclose(fp);
    fclose(fpw);

    return EXIT_OK;
}
