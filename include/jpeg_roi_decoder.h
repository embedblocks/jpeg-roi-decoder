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

/** Returns a short human-readable string for any result code.
 *  Useful during development and for error logging.
 *  Always returns a valid non-NULL string.
 */
const char *jpeg_decoder_err_to_str(jpeg_decode_result_t result);

/* ============================================================
 *  Source abstraction
 *
 *  Decouples the decoder from any specific I/O backend.
 *  Use jpeg_decoder_source_from_file() or
 *  jpeg_decoder_source_from_buffer() for the common cases,
 *  or fill the struct directly for a custom backend
 *  (network stream, encrypted FS, custom SPIFFS handle, etc).
 *
 *  NOTE: On ESP32, any filesystem (SPIFFS, FATFS, LittleFS)
 *  must be mounted by the caller before passing a source.
 *  This component does not perform driver or FS initialization.
 * ============================================================ */

typedef struct {
    /**
     * Read up to nbyte bytes into buf.
     * If buf is NULL, skip nbyte bytes forward (seek-by-reading).
     * Returns number of bytes consumed. Return 0 on error or EOF.
     */
    size_t (*read)(void *ctx, uint8_t *buf, size_t nbyte);

    /** Seek to absolute byte offset from start of JPEG stream. */
    int    (*seek)(void *ctx, size_t offset);

    void *ctx;
} jpeg_source_t;

/** Wrap a stdio FILE* as a source. */
jpeg_source_t jpeg_decoder_source_from_file(FILE *fp);

/**
 * Wrap an in-memory buffer as a source.
 * Suitable for images in rodata (flash), PSRAM, or DRAM.
 */
jpeg_source_t jpeg_decoder_source_from_buffer(const uint8_t *data, size_t len);

/* ============================================================
 *  Image info  (probe result)
 * ============================================================ */

typedef struct {
    uint16_t width;    /* original JPEG width  in pixels */
    uint16_t height;   /* original JPEG height in pixels */
} jpeg_image_info_t;

/* ============================================================
 *  Decode scale
 *
 *  Maps 1:1 to TJpgDec's supported downscale factors.
 *  These are the only valid hardware-accelerated values —
 *  no intermediate scales exist in TJpgDec.
 *
 *  JPEG_SCALE_AUTO: the component picks the largest divisor
 *  such that the scaled image still covers the LCD in both
 *  dimensions. Recommended for most users.
 *
 *  Output dimensions = ceil(jpeg_dim / scale_divisor).
 * ============================================================ */

typedef enum {
    JPEG_SCALE_AUTO = -1, /* component chooses best fit for LCD  */
    JPEG_SCALE_1_1  =  0, /* full resolution                     */
    JPEG_SCALE_1_2,       /* 1/2 linear (1/4 area)               */
    JPEG_SCALE_1_4,       /* 1/4 linear (1/16 area)              */
    JPEG_SCALE_1_8,       /* 1/8 linear (1/64 area)              */
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
 *  Delivered to the chunk callback once per completed row.
 *
 *  x, y     — position in output (LCD) pixel space.
 *  width    — guaranteed <= lcd_width (high-level API)
 *             or <= roi_width (low-level API).
 *  pixels   — RGB565 (uint16_t*) or RGB888 (uint8_t*),
 *             depending on out_format. Valid only during callback.
 * ============================================================ */

typedef struct {
    uint16_t    x;
    uint16_t    y;
    uint16_t    width;
    const void *pixels;
    size_t      byte_count;
    void       *user_data;
} jpeg_chunk_event_t;

/** Return false to abort decoding. Result will be JPEG_DECODE_ABORTED. */
typedef bool (*jpeg_chunk_cb_t)(const jpeg_chunk_event_t *evt);

/* ============================================================
 *  Done event
 * ============================================================ */

typedef struct {
    jpeg_decode_result_t  result;
    jpeg_image_info_t     image;        /* original JPEG dimensions          */
    jpeg_roi_t            roi_scaled;   /* decoded ROI in scaled pixel space */
    jpeg_decode_scale_t   scale;        /* scale that was actually used      */
    jpeg_output_format_t  out_format;
    void                 *user_data;
} jpeg_done_event_t;

typedef void (*jpeg_done_cb_t)(const jpeg_done_event_t *evt);

/* ============================================================
 *  Work buffer
 *
 *  TJpgDec requires ~3KB minimum. 4KB is a safe default.
 *  Larger buffers may improve performance on complex images.
 *  On ESP32 this buffer may live in DRAM or SPIRAM.
 * ============================================================ */

#define JPEG_DECODER_WORK_BUF_MIN      3096U
#define JPEG_DECODER_WORK_BUF_DEFAULT  4096U

/* ============================================================
 *  ── HIGH-LEVEL API ──
 *
 *  Describe an LCD viewport and how to pan across the image.
 *  The component handles probe, scale selection, ROI math,
 *  clamping, chunk buffer allocation, and pixel format.
 *
 *  pan_x / pan_y are in LCD pixels, independent of scale:
 *    (0, 0)  = viewport centered on image  [default]
 *    (+x)    = viewport shifted right
 *    (+y)    = viewport shifted down
 *  Values that would push the viewport outside the image are
 *  clamped automatically — no out-of-bounds is possible.
 * ============================================================ */

typedef struct {
    uint16_t lcd_width;
    uint16_t lcd_height;
    int32_t  pan_x;          /* LCD pixels from center, clamped */
    int32_t  pan_y;          /* LCD pixels from center, clamped */
    jpeg_decode_scale_t  scale;      /* use JPEG_SCALE_AUTO for most cases */
    jpeg_output_format_t out_format;
} jpeg_view_t;

/**
 * Returns a jpeg_view_t populated with safe defaults:
 *   pan  = (0,0)  — centered
 *   scale = JPEG_SCALE_AUTO
 *   format = JPEG_OUTPUT_RGB565
 *
 * Typical usage:
 *   jpeg_view_t view = jpeg_view_default(480, 320);
 *   view.pan_x = my_pan_x;   // override only what you need
 */
jpeg_view_t jpeg_view_default(uint16_t lcd_width, uint16_t lcd_height);

/* ============================================================
 *  ── LOW-LEVEL API ──
 *
 *  Direct ROI and scale control for advanced users.
 *  Use for map tiling, thumbnail pipelines, or any case where
 *  you need precise control over the decoded region.
 *
 *  ROI coordinates are in ORIGINAL (unscaled) JPEG space.
 *  The component converts them to scaled space internally.
 *
 *  chunk_buffer must be caller-allocated and large enough to
 *  hold one full row of the ROI:
 *    chunk_buffer_pixels >= (roi.right - roi.left + 1) / scale_div
 *  The component returns JPEG_DECODE_ERR_PARAM if this is violated.
 * ============================================================ */

typedef struct {
    jpeg_source_t        source;
    jpeg_roi_t           roi;               /* original JPEG pixel space */
    jpeg_decode_scale_t  scale;             /* JPEG_SCALE_AUTO not valid here */
    jpeg_output_format_t out_format;

    void    *work_buffer;
    size_t   work_buffer_size;

    uint16_t *chunk_buffer;                 /* caller-allocated, one row   */
    size_t    chunk_buffer_pixels;          /* must be >= scaled ROI width */

    jpeg_chunk_cb_t chunk_callback;
    jpeg_done_cb_t  done_callback;
    void           *user_data;
} jpeg_decode_request_t;

/* ============================================================
 *  API
 * ============================================================ */

/**
 * Initialize decoder runtime.
 * Required only on FreeRTOS (creates internal task + queue).
 * No-op on synchronous / host builds.
 */
bool jpeg_decoder_init(void);

/**
 * Deinitialize decoder runtime.
 */
void jpeg_decoder_deinit(void);

/**
 * Probe JPEG dimensions without decoding any pixel data.
 * Does not require jpeg_decoder_init().
 * Source position is restored to 0 after a successful probe,
 * so the same source can be passed directly to decode.
 *
 * Use this when you need image dimensions before deciding
 * how to display — e.g. building a navigation UI, choosing
 * a manual scale, or showing image info to the user.
 */
jpeg_decode_result_t jpeg_decoder_probe(
    jpeg_source_t        source,
    jpeg_image_info_t   *info_out,
    void                *work_buffer,
    size_t               work_buffer_size
);

/**
 * High-level decode. Internally probes, selects scale (if AUTO),
 * computes and clamps ROI, allocates chunk buffer, and decodes.
 *
 * Async  (FreeRTOS): returns immediately; done_callback fires on completion.
 * Sync   (host):     blocks until complete; done_callback fires before return.
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
 * Caller is responsible for coordinate math and buffer sizing.
 * JPEG_SCALE_AUTO is not valid in jpeg_decode_request_t.
 *
 * Async  (FreeRTOS): returns immediately; done_callback fires on completion.
 * Sync   (host):     blocks until complete; done_callback fires before return.
 */
jpeg_decode_result_t jpeg_decoder_decode(
    const jpeg_decode_request_t *req
);

#ifdef __cplusplus
}
#endif

#endif /* JPEG_ROI_DECODER_H */