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

/* ============================================================
 *  Source abstraction
 *
 *  Decouples the decoder from any specific I/O backend.
 *  Use jpeg_decoder_source_from_file() or
 *  jpeg_decoder_source_from_buffer() for the common cases,
 *  or fill the struct directly for custom backends
 *  (network stream, encrypted FS, etc).
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
jpeg_source_t jpeg_decoder_source_from_file  (FILE *fp);

/** Wrap an in-memory buffer as a source (e.g. rodata, PSRAM). */
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
 *  These are the only valid values — no intermediate scales exist.
 *  Output dimensions = ceil(jpeg_dim / scale_divisor).
 * ============================================================ */

typedef enum {
    JPEG_SCALE_1_1 = 0,   /* full resolution             */
    JPEG_SCALE_1_2,       /* 1/2 linear (1/4 pixels)     */
    JPEG_SCALE_1_4,       /* 1/4 linear (1/16 pixels)    */
    JPEG_SCALE_1_8,       /* 1/8 linear (1/64 pixels)    */
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
 *  x, y are in output (scaled ROI) space.
 *  width is guaranteed <= lcd_width (high-level) or roi_width (low-level).
 * ============================================================ */

typedef struct {
    uint16_t    x;
    uint16_t    y;
    uint16_t    width;
    const void *pixels;       /* RGB565 uint16_t* or RGB888 uint8_t*   */
    size_t      byte_count;   /* width * bytes_per_pixel               */
    void       *user_data;
} jpeg_chunk_event_t;

/** Return false to abort decoding. Result will be JPEG_DECODE_ABORTED. */
typedef bool (*jpeg_chunk_cb_t)(const jpeg_chunk_event_t *evt);

/* ============================================================
 *  Done event
 * ============================================================ */

typedef struct {
    jpeg_decode_result_t  result;
    jpeg_image_info_t     image;       /* original JPEG dimensions         */
    jpeg_roi_t            roi_scaled;  /* decoded ROI in scaled pixel space */
    jpeg_decode_scale_t   scale;
    jpeg_output_format_t  out_format;
    void                 *user_data;
} jpeg_done_event_t;

typedef void (*jpeg_done_cb_t)(const jpeg_done_event_t *evt);

/* ============================================================
 *  Work buffer
 *
 *  TJpgDec requires ~3KB minimum. 4KB is a safe default.
 *  Larger buffers improve performance on complex images.
 *  On ESP32, this buffer may be placed in DRAM or SPIRAM.
 * ============================================================ */

#define JPEG_DECODER_WORK_BUF_MIN      3096U
#define JPEG_DECODER_WORK_BUF_DEFAULT  4096U

/* ============================================================
 *  ── HIGH-LEVEL API ──
 *
 *  User describes an LCD viewport + pan offset.
 *  Component handles probe, ROI math, clamping, and format.
 *
 *  pan_x / pan_y are offsets from the image center, in output
 *  (scaled) pixel units:
 *    (0, 0)  = viewport centered on image  [default]
 *    (+x)    = viewport shifted right
 *    (+y)    = viewport shifted down
 *  Values that would push the viewport outside the image are
 *  clamped automatically.
 * ============================================================ */

typedef struct {
    uint16_t lcd_width;
    uint16_t lcd_height;
    int32_t  pan_x;
    int32_t  pan_y;
    jpeg_decode_scale_t  scale;
    jpeg_output_format_t out_format;
} jpeg_view_t;

/* ============================================================
 *  ── LOW-LEVEL API ──
 *
 *  Direct ROI and scale control for advanced users.
 *  ROI coordinates are in ORIGINAL (unscaled) JPEG space.
 *  The component converts them to scaled space internally.
 *
 *  chunk_buffer must be caller-allocated and hold at least
 *  (roi_width) pixels (one full row of the ROI).
 * ============================================================ */

typedef struct {
    jpeg_source_t        source;
    jpeg_roi_t           roi;               /* original JPEG pixel space */
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

/**
 * Initialize decoder runtime.
 * Required only on FreeRTOS (creates internal task + queue).
 * No-op on synchronous/host builds.
 */
bool jpeg_decoder_init(void);

/**
 * Deinitialize decoder runtime.
 */
void jpeg_decoder_deinit(void);

/**
 * Probe JPEG dimensions without decoding pixel data.
 * Does not require jpeg_decoder_init().
 * Source position is restored after a successful probe.
 */
jpeg_decode_result_t jpeg_decoder_probe(
    jpeg_source_t        source,
    jpeg_image_info_t   *info_out,
    void                *work_buffer,
    size_t               work_buffer_size
);

/**
 * High-level decode. Internally probes, computes ROI, clamps,
 * allocates chunk buffer, and calls the core decoder.
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