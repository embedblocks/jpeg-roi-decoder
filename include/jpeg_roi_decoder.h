#ifndef JPEG_ROI_DECODER_H
#define JPEG_ROI_DECODER_H

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================= ROI ================= */

typedef struct {
    uint16_t left;
    uint16_t top;
    uint16_t right;
    uint16_t bottom;
} jpeg_roi_t;

/* ================= RESULT ================= */

typedef enum {
    JPEG_DECODE_OK = 0,
    JPEG_DECODE_ERR_INTR,
    JPEG_DECODE_ERR_INPUT,
    JPEG_DECODE_ERR_MEM1,
    JPEG_DECODE_ERR_MEM2,
    JPEG_DECODE_ERR_PARAM,
    JPEG_DECODE_ERR_FMT1,
    JPEG_DECODE_ERR_FMT2,
    JPEG_DECODE_ERR_FMT3,
} jpeg_decode_result_t;

/* ================= CHUNK INFO ================= */

typedef struct {
    uint16_t x;       // X offset inside ROI
    uint16_t y;       // Y offset inside ROI
    uint16_t width;   // Width of this chunk
    uint16_t height;  // Height of this chunk
} jpeg_chunk_info_t;

/* ================= CHUNK EVENT ================= */

typedef struct {
    FILE *fp;                       // input JPEG file (read-only)
    const uint16_t *pixels;         // RGB565 pixel data
    const jpeg_chunk_info_t *chunk; // chunk position + size
    size_t pixel_count;             // width * height
    size_t byte_count;              // pixel_count * 2
    void *user_data;                // user supplied pointer
} jpeg_chunk_event_t;

/* ================= DONE EVENT ================= */

typedef struct {
    FILE *fp;                        // input JPEG file
    jpeg_decode_result_t result;     // decode result
    uint16_t image_width;            // full JPEG width
    uint16_t image_height;           // full JPEG height
    jpeg_roi_t roi;                  // decoded ROI
    void *user_data;                 // user supplied pointer
} jpeg_done_event_t;

/* ================= CALLBACKS ================= */

/**
 * Called every time output buffer is flushed.
 * Return false to abort decoding.
 */
typedef bool (*jpeg_chunk_cb_t)(const jpeg_chunk_event_t *event);

/**
 * Called once when decode finishes or aborts.
 */
typedef void (*jpeg_done_cb_t)(const jpeg_done_event_t *event);

/* ================= REQUEST ================= */

typedef struct {
    FILE *fp;                         // opened JPEG file
    jpeg_roi_t roi;                   // region to decode
    uint16_t *chunk_buffer;           // user allocated buffer
    size_t chunk_buffer_pixels;       // buffer size in pixels
    jpeg_chunk_cb_t chunk_callback;   // per-chunk callback
    jpeg_done_cb_t done_callback;     // completion callback
    void *user_data;                  // passed to callbacks
} jpeg_decode_request_t;

/* ================= API ================= */

esp_err_t jpeg_decoder_init(void);

esp_err_t jpeg_decoder_decode(const jpeg_decode_request_t *request);

esp_err_t jpeg_decoder_deinit(void);

int jpeg_decoder_get_queue_depth(void);

#ifdef __cplusplus
}
#endif

#endif /* JPEG_DECODER_H */
