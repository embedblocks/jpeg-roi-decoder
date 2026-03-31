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
 *
 *  Decouples the decoder from any specific I/O backend.
 *
 *  For FILE* sources, use the factory:
 *    jpeg_source_t src = jpeg_decoder_source_from_file(fp);
 *
 *  For buffer sources, use the init function — NOT a factory —
 *  because the source must track read position internally via
 *  _buf, and ctx must point into the struct itself:
 *    jpeg_source_t src;
 *    jpeg_decoder_source_from_buffer(&src, data, len);
 *
 *  For custom backends, fill the struct directly:
 *    jpeg_source_t src = { .read=my_read, .seek=my_seek, .ctx=my_handle };
 *
 *  NOTE: On ESP32, any filesystem (SPIFFS, FATFS, LittleFS) must be
 *  mounted by the caller. This component does not initialize peripherals.
 * ============================================================ */

typedef struct {
    /**
     * Read up to nbyte bytes into buf.
     * If buf is NULL, skip nbyte bytes forward.
     * Returns bytes consumed. Return 0 on error or EOF.
     */
    size_t (*read)(void *ctx, uint8_t *buf, size_t nbyte);

    /** Seek to absolute byte offset from start of JPEG stream. */
    int    (*seek)(void *ctx, size_t offset);

    void *ctx;

    /**
     * Internal state for jpeg_decoder_source_from_buffer().
     * Do not use or modify directly.
     * For FILE* and custom sources this field is unused.
     */
    struct {
        const uint8_t *data;
        size_t         len;
        size_t         pos;
    } _buf;
} jpeg_source_t;

/**
 * Wrap a stdio FILE* as a source.
 * Returns by value — safe to use as a factory.
 */
jpeg_source_t jpeg_decoder_source_from_file(FILE *fp);

/**
 * Initialize a buffer source in-place.
 * Takes a pointer instead of returning by value because ctx must
 * point into the struct itself — a factory return-by-value would
 * produce a dangling pointer after the copy.
 *
 * Usage:
 *   jpeg_source_t src;
 *   jpeg_decoder_source_from_buffer(&src, data, len);
 *
 * Do NOT copy src after initializing — the internal ctx pointer
 * would point into the original, not the copy.
 */
void jpeg_decoder_source_from_buffer(jpeg_source_t *src,
                                      const uint8_t *data,
                                      size_t         len);

/* ============================================================
 *  Image info  (probe result)
 * ============================================================ */

typedef struct {
    uint16_t width;
    uint16_t height;
} jpeg_image_info_t;

/* ============================================================
 *  Decode scale
 *
 *  Maps 1:1 to TJpgDec's supported downscale factors.
 *  No intermediate values exist.
 *
 *  JPEG_SCALE_AUTO: component picks the largest divisor such
 *  that the scaled image still covers the LCD in both dimensions.
 *  Recommended for most users.
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
    JPEG_OUTPUT_RGB565 = 0,   /* native TJpgDec format, no conversion */
    JPEG_OUTPUT_RGB888,       /* converted internally, 3 bytes/pixel  */
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
 *  Chunk event
 *
 *  Delivered once per completed row.
 *  x, y     — position in output (LCD) pixel space.
 *  width    — guaranteed <= lcd_width (high-level API)
 *             or <= roi_width (low-level API).
 *  pixels   — valid only for the duration of the callback.
 *             Do not store the pointer.
 * ============================================================ */

typedef struct {
    uint16_t    x;
    uint16_t    y;
    uint16_t    width;
    const void *pixels;       /* RGB565 uint16_t* or RGB888 uint8_t* */
    size_t      byte_count;
    void       *user_data;
} jpeg_chunk_event_t;

/** Return false to abort. Result will be JPEG_DECODE_ABORTED. */
typedef bool (*jpeg_chunk_cb_t)(const jpeg_chunk_event_t *evt);

/* ============================================================
 *  Done event
 * ============================================================ */

typedef struct {
    jpeg_decode_result_t  result;
    jpeg_image_info_t     image;        /* original JPEG dimensions          */
    jpeg_roi_t            roi_scaled;   /* decoded ROI in scaled pixel space */
    jpeg_decode_scale_t   scale;        /* scale actually used               */
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
 *  pan_x / pan_y are in LCD pixels, independent of scale:
 *    (0, 0)  = viewport centered on image
 *    (+x)    = viewport shifted right
 *    (+y)    = viewport shifted down
 *  Out-of-bounds values are clamped automatically.
 * ============================================================ */

typedef struct {
    uint16_t lcd_width;
    uint16_t lcd_height;
    int32_t  pan_x;
    int32_t  pan_y;
    jpeg_decode_scale_t  scale;
    jpeg_output_format_t out_format;
} jpeg_view_t;

/**
 * Returns a jpeg_view_t with safe defaults:
 *   pan    = (0, 0) — centered
 *   scale  = JPEG_SCALE_AUTO
 *   format = JPEG_OUTPUT_RGB565
 */
jpeg_view_t jpeg_view_default(uint16_t lcd_width, uint16_t lcd_height);

/* ============================================================
 *  LOW-LEVEL API
 *
 *  ROI in original (unscaled) JPEG coordinates.
 *  chunk_buffer_pixels must be >= scaled ROI width.
 *  JPEG_SCALE_AUTO is not valid here — use an explicit scale.
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

/**
 * Probe JPEG dimensions without decoding pixel data.
 * Source position is restored to 0 after a successful probe.
 */
jpeg_decode_result_t jpeg_decoder_probe(
    jpeg_source_t        source,
    jpeg_image_info_t   *info_out,
    void                *work_buffer,
    size_t               work_buffer_size
);

/**
 * High-level decode. Probes, resolves AUTO scale, computes
 * centered and clamped ROI, allocates chunk buffer internally.
 */
jpeg_decode_result_t jpeg_decoder_decode_view(
    jpeg_source_t        source,
    const jpeg_view_t   *view,
    void                *work_buffer,
    size_t               work_buffer_size,
    jpeg_chunk_cb_t      chunk_callback,
    jpeg_done_cb_t       done_callback,
    void                *user_data
);

/**
 * Low-level decode. Full ROI and scale control.
 * JPEG_SCALE_AUTO is not valid in jpeg_decode_request_t.
 */
jpeg_decode_result_t jpeg_decoder_decode(
    const jpeg_decode_request_t *req
);

#ifdef __cplusplus
}
#endif

#endif /* JPEG_ROI_DECODER_H */