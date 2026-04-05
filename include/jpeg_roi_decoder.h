#ifndef JPEG_ROI_DECODER_H
#define JPEG_ROI_DECODER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  Platform control
 * ============================================================ */

#if defined(JPEG_DECODER_PLATFORM_FREERTOS)
#  define JPEG_DECODER_ASYNC 1
#else
#  define JPEG_DECODER_ASYNC 0
#endif

/* ============================================================
 *  Result codes
 * ============================================================ */

typedef enum {
    JPEG_DECODE_OK = 0,
    JPEG_DECODE_ABORTED,        /* user callback returned false  */
    JPEG_DECODE_ERR_PARAM,      /* bad argument / invalid ROI    */
    JPEG_DECODE_ERR_INPUT,      /* source read / prepare failed  */
    JPEG_DECODE_ERR_MEM,        /* work buffer too small         */
    JPEG_DECODE_ERR_FMT,        /* unsupported JPEG format       */
    JPEG_DECODE_ERR_INTR,       /* tjpgd internal error          */
} jpeg_decode_result_t;

/** Returns a short human-readable string for any result code. */
const char *jpeg_decoder_err_to_str(jpeg_decode_result_t result);

/* ============================================================
 *  Source abstraction
 * ============================================================ */

typedef struct {
    size_t (*read)(void *ctx, uint8_t *buf, size_t nbyte);
    int    (*seek)(void *ctx, size_t offset);
    void  *ctx;

    /* Internal state for jpeg_decoder_source_from_buffer().
     * Do not use directly. */
    struct {
        const uint8_t *data;
        size_t         len;
        size_t         pos;
    } _buf;
} jpeg_source_t;

jpeg_source_t jpeg_decoder_source_from_file  (FILE *fp);
void          jpeg_decoder_source_from_buffer(jpeg_source_t *src,
                                              const uint8_t *data,
                                              size_t         len);

/* ============================================================
 *  Image info
 * ============================================================ */

typedef struct {
    uint16_t width;
    uint16_t height;
} jpeg_image_info_t;

/* ============================================================
 *  Decode scale
 * ============================================================ */

typedef enum {
    JPEG_SCALE_AUTO = -1,
    JPEG_SCALE_1_1  =  0,
    JPEG_SCALE_1_2,
    JPEG_SCALE_1_4,
    JPEG_SCALE_1_8,
} jpeg_decode_scale_t;

/* ============================================================
 *  Output pixel format
 * ============================================================ */

typedef enum {
    JPEG_OUTPUT_RGB565 = 0,
    JPEG_OUTPUT_RGB888,
} jpeg_output_format_t;

/* ============================================================
 *  ROI  (in original, unscaled JPEG coordinates)
 * ============================================================ */

typedef struct {
    uint16_t left;
    uint16_t top;
    uint16_t right;
    uint16_t bottom;
} jpeg_roi_t;

/* ============================================================
 *  Chunk buffer sizing
 *
 *  TJpgDec delivers MCU blocks left-to-right across the full
 *  image width before moving to the next row band. Each MCU
 *  block is up to JPEG_MCU_MAX_HEIGHT rows tall.
 *
 *  The chunk buffer must hold one full band of JPEG_MCU_MAX_HEIGHT
 *  rows so each row can accumulate independently without
 *  overwriting other rows in the same band.
 *
 *  Required size:
 *    pixels : roi_width * JPEG_MCU_MAX_HEIGHT
 *    bytes  : roi_width * JPEG_MCU_MAX_HEIGHT * sizeof(uint16_t)
 *
 *  Use JPEG_CHUNK_BUF_PIXELS(w) to compute the pixel count.
 * ============================================================ */

#define JPEG_MCU_MAX_HEIGHT        16u
#define JPEG_CHUNK_BUF_PIXELS(w)   ((w) * JPEG_MCU_MAX_HEIGHT)
#define JPEG_CHUNK_BUF_BYTES(w)    (JPEG_CHUNK_BUF_PIXELS(w) * sizeof(uint16_t))

/* ============================================================
 *  Chunk event
 * ============================================================ */

typedef struct {
    uint16_t    x;
    uint16_t    y;
    uint16_t    width;
    const void *pixels;       /* valid only during callback */
    size_t      byte_count;
    void       *user_data;
} jpeg_chunk_event_t;

typedef bool (*jpeg_chunk_cb_t)(const jpeg_chunk_event_t *evt);

/* ============================================================
 *  Done event
 * ============================================================ */

typedef struct {
    jpeg_decode_result_t  result;
    jpeg_image_info_t     image;
    jpeg_roi_t            roi_scaled;
    jpeg_decode_scale_t   scale;
    jpeg_output_format_t  out_format;
    void                 *user_data;
} jpeg_done_event_t;

typedef void (*jpeg_done_cb_t)(const jpeg_done_event_t *evt);

/* ============================================================
 *  Work buffer
 * ============================================================ */

#define JPEG_DECODER_WORK_BUF_MIN      3096U
#define JPEG_DECODER_WORK_BUF_DEFAULT  4096U

/* ============================================================
 *  HIGH-LEVEL API
 *
 *  pan_x / pan_y in LCD pixels from center. Clamped automatically.
 *
 *  chunk_buffer  — caller-allocated, must hold JPEG_MCU_MAX_HEIGHT
 *                  rows of lcd_width pixels each.
 *                  Size in pixels : JPEG_CHUNK_BUF_PIXELS(lcd_width)
 *                  Size in bytes  : JPEG_CHUNK_BUF_BYTES(lcd_width)
 * ============================================================ */

typedef struct {
    uint16_t lcd_width;
    uint16_t lcd_height;
    int32_t  pan_x;
    int32_t  pan_y;
    jpeg_decode_scale_t  scale;
    jpeg_output_format_t out_format;

    uint16_t *chunk_buffer;   /* caller-allocated — see JPEG_CHUNK_BUF_BYTES() */
} jpeg_view_t;

/**
 * Returns a jpeg_view_t with safe defaults.
 * chunk_buffer is set to NULL — caller must set it before decoding.
 */
jpeg_view_t jpeg_view_default(uint16_t lcd_width, uint16_t lcd_height);

/* ============================================================
 *  LOW-LEVEL API
 *
 *  chunk_buffer_pixels must be >= roi_width * JPEG_MCU_MAX_HEIGHT.
 *  Use JPEG_CHUNK_BUF_PIXELS(roi_width) to compute.
 *  JPEG_SCALE_AUTO is not valid here.
 * ============================================================ */

typedef struct {
    jpeg_source_t        source;
    jpeg_roi_t           roi;
    jpeg_decode_scale_t  scale;
    jpeg_output_format_t out_format;

    void    *work_buffer;
    size_t   work_buffer_size;

    uint16_t *chunk_buffer;
    size_t    chunk_buffer_pixels;

    jpeg_chunk_cb_t chunk_callback;
    jpeg_done_cb_t  done_callback;
    void           *user_data;
} jpeg_decode_request_t;

/* ============================================================
 *  API
 * ============================================================ */

bool jpeg_decoder_init(void);
void jpeg_decoder_deinit(void);

jpeg_decode_result_t jpeg_decoder_probe(
    jpeg_source_t        source,
    jpeg_image_info_t   *info_out,
    void                *work_buffer,
    size_t               work_buffer_size
);

jpeg_decode_result_t jpeg_decoder_decode_view(
    jpeg_source_t        source,
    const jpeg_view_t   *view,
    void                *work_buffer,
    size_t               work_buffer_size,
    jpeg_chunk_cb_t      chunk_callback,
    jpeg_done_cb_t       done_callback,
    void                *user_data
);

jpeg_decode_result_t jpeg_decoder_decode(
    const jpeg_decode_request_t *req
);

#ifdef __cplusplus
}
#endif

#endif /* JPEG_ROI_DECODER_H */