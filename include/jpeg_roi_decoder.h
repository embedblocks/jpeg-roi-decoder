#ifndef JPEG_ROI_DECODER_H
#define JPEG_ROI_DECODER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  Platform control
 * ============================================================ */

/*
 * Define JPEG_DECODER_PLATFORM_FREERTOS in ESP-IDF builds.
 * Leave undefined for host / Windows builds.
 */
#if defined(JPEG_DECODER_PLATFORM_FREERTOS)
#define JPEG_DECODER_ASYNC 1
#else
#define JPEG_DECODER_ASYNC 0
#endif

/* ============================================================
 *  ROI
 * ============================================================ */

typedef struct {
    uint16_t left;
    uint16_t top;
    uint16_t right;
    uint16_t bottom;
} jpeg_roi_t;

/* ============================================================
 *  Decode scale (maps directly to TJpgDec)
 * ============================================================ */

typedef enum {
    JPEG_SCALE_1_1 = 0,   /* full resolution */
    JPEG_SCALE_1_2,       /* 1/2 */
    JPEG_SCALE_1_4,       /* 1/4 */
    JPEG_SCALE_1_8        /* 1/8 */
} jpeg_decode_scale_t;

/* ============================================================
 *  Decode result
 * ============================================================ */

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

/* ============================================================
 *  Image info (probe result)
 * ============================================================ */

typedef struct {
    uint16_t width;     /* original JPEG width */
    uint16_t height;    /* original JPEG height */
} jpeg_image_info_t;

/* ============================================================
 *  Chunk info
 * ============================================================ */

typedef struct {
    uint16_t x;       /* X offset inside ROI (scaled space) */
    uint16_t y;       /* Y offset inside ROI (scaled space) */
    uint16_t width;   /* chunk width */
    uint16_t height;  /* chunk height */
} jpeg_chunk_info_t;

/* ============================================================
 *  Chunk event
 * ============================================================ */

typedef struct {
    //FILE *fp;                       /* input JPEG file */
    const uint16_t *pixels;         /* RGB565 pixels */
    const jpeg_chunk_info_t *chunk; /* chunk position */
    size_t pixel_count;             /* width * height */
    size_t byte_count;              /* pixel_count * 2 */
    void *user_data;
} jpeg_chunk_event_t;

/* ============================================================
 *  Done event
 * ============================================================ */

typedef struct {
    FILE *fp;
    jpeg_decode_result_t result;
    jpeg_image_info_t image;   /* full image info */
    jpeg_roi_t roi;            /* decoded ROI (scaled space) */
    jpeg_decode_scale_t scale;
    void *user_data;
} jpeg_done_event_t;

/* ============================================================
 *  Callbacks
 * ============================================================ */

/*
 * Called when a chunk is ready.
 * Return false to abort decoding.
 */
typedef bool (*jpeg_chunk_cb_t)(const jpeg_chunk_event_t *event);

/*
 * Called once when decode finishes or aborts.
 */
typedef void (*jpeg_done_cb_t)(const jpeg_done_event_t *event);

/* ============================================================
 *  Decode request
 * ============================================================ */

typedef struct {
    jpeg_roi_t roi;                 /* ROI in scaled space */
    jpeg_decode_scale_t scale;      /* downscaling factor */

    void* work_buffer;
    size_t work_buffer_size;
    uint16_t *chunk_buffer;         /* user-provided buffer */
    size_t chunk_buffer_pixels;     /* buffer size in pixels */

    jpeg_chunk_cb_t chunk_callback;
    jpeg_done_cb_t  done_callback;

    void *user_data;
} jpeg_decode_request_t;

/* ============================================================
 *  API
 * ============================================================ */

/*
 * Initialize decoder runtime.
 * Required only on async platforms (FreeRTOS).
 */
bool jpeg_decoder_init(void);

/*
 * Decode JPEG according to request.
 *
 * Async platforms:
 *   - returns immediately
 *   - done_callback signals completion
 *
 * Sync platforms:
 *   - blocks until decode completes
 *   - done_callback called before return
 */
jpeg_decode_result_t
jpeg_decoder_decode(const jpeg_decode_request_t *request);

/*
 * Deinitialize decoder runtime.
 */
void jpeg_decoder_deinit(void);

/*
 * Probe JPEG image dimensions without decoding.
 * Does NOT depend on async runtime.
 */
jpeg_decode_result_t
jpeg_decoder_probe(FILE *fp, jpeg_image_info_t *info,void *workbuf,
    size_t workbuf_size);



#ifdef __cplusplus
}
#endif

#endif /* JPEG_DECODER_H */

