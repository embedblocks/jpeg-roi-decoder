#ifndef JPEG_ROI_DECODER_H
#define JPEG_ROI_DECODER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

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
    JPEG_DECODE_ABORTED,        /* user callback returned false   */
    JPEG_DECODE_ERR_PARAM,      /* bad argument / invalid ROI     */
    JPEG_DECODE_ERR_INPUT,      /* source read / prepare failed   */
    JPEG_DECODE_ERR_MEM,        /* work buffer too small          */
    JPEG_DECODE_ERR_FMT,        /* unsupported JPEG format        */
    JPEG_DECODE_ERR_INTR,       /* tjpgd internal error           */
} jpeg_decode_result_t;

/** Returns a short human-readable string for any result code. */
const char *jpeg_decoder_err_to_str(jpeg_decode_result_t result);

/* ============================================================
 *  Source abstraction — read callback + context
 * ============================================================ */

typedef size_t (*jpeg_read_cb_t)(uint8_t *dst, size_t max, void *ctx);

typedef struct {
    jpeg_read_cb_t  cb;
    void           *ctx;
} jpeg_reader_t;

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
 * ============================================================ */

#define JPEG_MCU_MAX_HEIGHT        16u
#define JPEG_CHUNK_BUF_PIXELS(w)   ((w) * JPEG_MCU_MAX_HEIGHT)
#define JPEG_CHUNK_BUF_BYTES(w)    (JPEG_CHUNK_BUF_PIXELS(w) * sizeof(uint16_t))

/* ============================================================
 *  Input prefetch buffer sizing
 *
 *  When input_buffer is non-NULL in jpeg_view_intent_t or
 *  jpeg_decode_request_t, the decoder reads from the source in
 *  JPEG_INPUT_BUF_SIZE-byte chunks, serving TJpgDec's small internal
 *  requests from the internal buffer instead of calling reader.cb
 *  directly. This batches slow sources (HTTP, UART) into large aligned
 *  reads, eliminating the rapid-fire small-request pattern that
 *  firewalls and servers treat as malicious.
 *
 *  2048 bytes covers most JFIF/Exif headers in a single refill.
 *  Override by defining JPEG_INPUT_BUF_SIZE before including this header.
 *
 *  NULL (default) = original direct pass-through, no extra memory.
 * ============================================================ */

#ifndef JPEG_INPUT_BUF_SIZE
#define JPEG_INPUT_BUF_SIZE  2048u
#endif

/* ============================================================
 *  Chunk event
 * ============================================================ */

typedef struct {
    uint16_t    x;
    uint16_t    y;
    uint16_t    width;
    const void *pixels;
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
 *  HIGH-LEVEL API — jpeg_view_intent_t
 * ============================================================ */

typedef struct {
    uint16_t lcd_width;
    uint16_t lcd_height;
    int32_t  pan_x;
    int32_t  pan_y;
    jpeg_decode_scale_t  scale;
    jpeg_output_format_t out_format;

    jpeg_reader_t  reader;
    uint16_t      *chunk_buffer;      /* caller-allocated — JPEG_CHUNK_BUF_BYTES(lcd_width) */
    uint8_t       *input_buffer;      /* caller-allocated — JPEG_INPUT_BUF_SIZE bytes.
                                         NULL = direct pass-through (no prefetch). */
} jpeg_view_intent_t;

jpeg_view_intent_t jpeg_view_default(uint16_t lcd_width, uint16_t lcd_height);

/* ============================================================
 *  LOW-LEVEL API — jpeg_decode_request_t
 * ============================================================ */

typedef struct {
    jpeg_reader_t        reader;

    jpeg_roi_t           roi;
    jpeg_decode_scale_t  scale;
    jpeg_output_format_t out_format;

    void    *work_buffer;
    size_t   work_buffer_size;

    uint16_t *chunk_buffer;
    size_t    chunk_buffer_pixels;
    uint8_t  *input_buffer;          /* caller-allocated — JPEG_INPUT_BUF_SIZE bytes.
                                        NULL = direct pass-through (no prefetch). */

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
    jpeg_reader_t      reader,
    jpeg_image_info_t *info_out,
    void              *work_buffer,
    size_t             work_buffer_size
);

jpeg_decode_result_t jpeg_decoder_decode_view(
    const jpeg_view_intent_t *intent,
    void                     *work_buffer,
    size_t                    work_buffer_size,
    jpeg_chunk_cb_t           chunk_callback,
    jpeg_done_cb_t            done_callback,
    void                     *user_data
);

jpeg_decode_result_t jpeg_decoder_decode(const jpeg_decode_request_t *req);

#ifdef __cplusplus
}
#endif

#endif /* JPEG_ROI_DECODER_H */
