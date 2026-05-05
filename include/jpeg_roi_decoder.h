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
 *
 *  The component ships no source backends. File, buffer, HTTP,
 *  UART, DMA — all are trivial to implement by the caller.
 *
 *  Example — in-memory buffer (caller implements, not shipped):
 *
 *    typedef struct { const uint8_t *data; size_t len; size_t pos; } buf_ctx_t;
 *
 *    size_t buf_read_cb(uint8_t *dst, size_t max, void *vctx) {
 *        buf_ctx_t *bc = vctx;
 *        size_t avail = bc->len - bc->pos;
 *        size_t n     = max < avail ? max : avail;
 *        if (dst) memcpy(dst, bc->data + bc->pos, n);
 *        bc->pos += n;   // advance even on skip (dst == NULL)
 *        return n;
 *    }
 *
 *  Example — FILE* with seek fast-path (caller implements, not shipped):
 *
 *    size_t file_read_cb(uint8_t *dst, size_t max, void *ctx) {
 *        FILE *fp = ctx;
 *        if (dst == NULL)
 *            return fseek(fp, (long)max, SEEK_CUR) == 0 ? max : 0;
 *        return fread(dst, 1, max, fp);
 *    }
 *
 *  Example — FreeRTOS queue source with timeout (caller implements):
 *
 *    typedef struct { QueueHandle_t q; TickType_t timeout; } queue_ctx_t;
 *
 *    size_t queue_read_cb(uint8_t *dst, size_t max, void *vctx) {
 *        queue_ctx_t *qc = vctx;
 *        size_t n = 0;
 *        while (n < max) {
 *            uint8_t byte;
 *            if (xQueueReceive(qc->q, &byte, qc->timeout) != pdTRUE)
 *                break;   // timeout → decoder sees ERR_INPUT
 *            if (dst) dst[n] = byte;
 *            n++;
 *        }
 *        return n;
 *    }
 * ============================================================ */

/**
 * Read callback — the only source abstraction the decoder requires.
 *
 * @param dst   Destination buffer owned by TJpgDec. Write JPEG source bytes
 *              directly here. If NULL, the decoder is requesting a skip/drain:
 *              advance the source by @p max bytes without writing to RAM.
 *              For seekable sources implement this as fseek (zero RAM, zero copy).
 *              For non-seekable sources read and discard.
 * @param max   Maximum bytes to write, or bytes to skip when dst is NULL.
 * @param ctx   Caller-defined context (file pointer, buffer struct, queue handle…).
 *
 * @return Number of bytes read or skipped.
 *         Return 0 on end-of-data, timeout, or unrecoverable error.
 *         Partial returns (< max) are valid; the decoder retries internally.
 *
 * @note Must not block indefinitely. Implement a timeout; return 0 on expiry.
 *       A permanently stalled callback stalls the decoder with no watchdog.
 */
typedef size_t (*jpeg_read_cb_t)(uint8_t *dst, size_t max, void *ctx);

/**
 * (callback, context) pair — the complete source descriptor.
 * Contains no internal state; all source state lives in ctx.
 * Embed this by value in view/request structs; it is safe to copy.
 */
typedef struct {
    jpeg_read_cb_t  cb;   /* mandatory, must not be NULL */
    void           *ctx;  /* passed to cb unchanged       */
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
    JPEG_SCALE_AUTO = -1,   /* valid in jpeg_view_intent_t only */
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
 *  rows so each row can accumulate in its own dedicated slot
 *  without overwriting rows still in progress.
 *
 *  Required size:
 *    pixels : roi_width * JPEG_MCU_MAX_HEIGHT
 *    bytes  : roi_width * JPEG_MCU_MAX_HEIGHT * sizeof(uint16_t)
 *
 *  Use JPEG_CHUNK_BUF_PIXELS(w) / JPEG_CHUNK_BUF_BYTES(w) to compute.
 * ============================================================ */

#define JPEG_MCU_MAX_HEIGHT        16u
#define JPEG_CHUNK_BUF_PIXELS(w)   ((w) * JPEG_MCU_MAX_HEIGHT)
#define JPEG_CHUNK_BUF_BYTES(w)    (JPEG_CHUNK_BUF_PIXELS(w) * sizeof(uint16_t))

/* ============================================================
 *  Chunk event  — fired once per completed output row
 * ============================================================ */

typedef struct {
    uint16_t    x;
    uint16_t    y;
    uint16_t    width;
    const void *pixels;       /* valid only during callback */
    size_t      byte_count;
    void       *user_data;
} jpeg_chunk_event_t;

/**
 * Row-complete callback.
 * Return true to continue decoding, false to abort.
 * Must return within a bounded, predictable time — the decode
 * pipeline stalls for exactly as long as this callback takes.
 * For slow outputs (UART, SPI display) use a queue adapter.
 */
typedef bool (*jpeg_chunk_cb_t)(const jpeg_chunk_event_t *evt);

/* ============================================================
 *  Done event  — fired once when decode completes or errors
 * ============================================================ */

typedef struct {
    jpeg_decode_result_t  result;
    jpeg_image_info_t     image;      /* full JPEG dimensions (unscaled) */
    jpeg_roi_t            roi_scaled; /* ROI in scaled (output) coords   */
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
 *
 *  pan_x / pan_y in LCD pixels from center. Clamped automatically.
 *
 *  reader       — caller-implemented read callback + context.
 *                 All source state lives in reader.ctx.
 *                 Must remain valid until done_callback fires.
 *
 *  chunk_buffer — caller-allocated; must hold JPEG_MCU_MAX_HEIGHT rows of
 *                 lcd_width pixels. Size: JPEG_CHUNK_BUF_BYTES(lcd_width).
 *                 Must remain valid and unread until done_callback fires.
 *
 *  work_buffer is passed separately to jpeg_decoder_decode_view().
 *  Execution resources are intentionally kept out of this struct.
 * ============================================================ */

typedef struct {
    uint16_t lcd_width;
    uint16_t lcd_height;
    int32_t  pan_x;                   /* pixels from center, clamped  */
    int32_t  pan_y;
    jpeg_decode_scale_t  scale;       /* JPEG_SCALE_AUTO is valid here */
    jpeg_output_format_t out_format;

    jpeg_reader_t  reader;            /* cb + ctx — caller implements  */
    uint16_t      *chunk_buffer;      /* caller-allocated — see JPEG_CHUNK_BUF_BYTES() */
} jpeg_view_intent_t;

/**
 * Returns a jpeg_view_intent_t with safe defaults.
 * reader = {NULL, NULL}, chunk_buffer = NULL — caller must fill all pointer fields.
 */
jpeg_view_intent_t jpeg_view_default(uint16_t lcd_width, uint16_t lcd_height);

/* ============================================================
 *  LOW-LEVEL API — jpeg_decode_request_t
 *
 *  Caller pre-computes scale and ROI (in original unscaled JPEG coords).
 *  JPEG_SCALE_AUTO is rejected with JPEG_DECODE_ERR_PARAM.
 *  chunk_buffer_pixels must be >= roi_width * JPEG_MCU_MAX_HEIGHT.
 *  Use JPEG_CHUNK_BUF_PIXELS(roi_width) to compute.
 * ============================================================ */

typedef struct {
    jpeg_reader_t        reader;      /* cb + ctx, mandatory              */

    jpeg_roi_t           roi;         /* in original unscaled JPEG coords */
    jpeg_decode_scale_t  scale;       /* JPEG_SCALE_AUTO not valid here   */
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
 * Initialise the decoder (RTOS path only — creates queue and worker task).
 * Must be called before jpeg_decoder_decode_view() or jpeg_decoder_decode().
 * Safe to call on bare-metal builds (no-op).
 */
bool jpeg_decoder_init(void);
void jpeg_decoder_deinit(void);

/**
 * Parse JPEG headers and return image dimensions.
 *
 * Advances the source through the header markers only. The caller must reset
 * their own source context (e.g. fseek(fp, 0, SEEK_SET), reset buf_ctx_t.pos)
 * before passing the same source to a decode call. The component provides no
 * reset mechanism.
 *
 * For non-seekable streams (HTTP, UART, queue), probe is not useful — the
 * stream cannot be rewound. Use the decode path directly; image dimensions
 * are available in done_callback via done_evt->image.
 *
 * @param reader           (cb + ctx) pair. cb must not be NULL.
 * @param info_out         Receives width and height on success.
 * @param work_buffer      Caller-allocated; JPEG_DECODER_WORK_BUF_MIN bytes minimum.
 * @param work_buffer_size Size of work_buffer in bytes.
 */
jpeg_decode_result_t jpeg_decoder_probe(
    jpeg_reader_t      reader,
    jpeg_image_info_t *info_out,
    void              *work_buffer,
    size_t             work_buffer_size
);

/**
 * High-level decode — scale and ROI are resolved from the intent after the
 * JPEG header is parsed (single forward pass, no rewind required).
 *
 * RTOS path: returns JPEG_DECODE_OK if the request was accepted into the queue.
 *   All decode errors (including header parse failures) arrive via done_callback.
 *   Caller must not treat JPEG_DECODE_OK as decode success.
 * Synchronous path: returns the actual decode result; done_callback also fires.
 *
 * Buffer lifetimes (work_buffer, intent->chunk_buffer, intent->reader.ctx):
 *   Must remain valid and unmodified until done_callback fires.
 *   Declaring buffers as static arrays is the simplest correct choice.
 *
 * @param intent           Viewing parameters incl. reader and chunk_buffer.
 * @param work_buffer      Caller-allocated scratch; JPEG_DECODER_WORK_BUF_DEFAULT recommended.
 * @param work_buffer_size Size of work_buffer in bytes.
 */
jpeg_decode_result_t jpeg_decoder_decode_view(
    const jpeg_view_intent_t *intent,
    void                     *work_buffer,
    size_t                    work_buffer_size,
    jpeg_chunk_cb_t           chunk_callback,
    jpeg_done_cb_t            done_callback,
    void                     *user_data
);

/**
 * Low-level decode — caller pre-computes scale and ROI.
 * JPEG_SCALE_AUTO in req->scale is rejected with JPEG_DECODE_ERR_PARAM.
 *
 * RTOS path: returns JPEG_DECODE_OK if enqueued; errors arrive via done_callback.
 * Synchronous path: returns the actual decode result; done_callback also fires.
 */
jpeg_decode_result_t jpeg_decoder_decode(const jpeg_decode_request_t *req);

#ifdef __cplusplus
}
#endif

#endif /* JPEG_ROI_DECODER_H */