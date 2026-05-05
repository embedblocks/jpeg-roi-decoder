#ifndef JPEG_DECODER_INTERNAL_H
#define JPEG_DECODER_INTERNAL_H

#include "jpeg_roi_decoder.h"
#include <stdbool.h>

/* ============================================================
 *  Internal decode context — lives on the stack inside core_run
 * ============================================================ */
#define JPEG_MAX_ROI_HEIGHT  512u

typedef struct {
    jpeg_reader_t        reader;      /* cb + ctx, no other source state */

    jpeg_roi_t           roi;         /* in scaled (output) coords after prepare */
    jpeg_decode_scale_t  scale;
    jpeg_output_format_t out_format;

    uint16_t *chunk_buffer;
    size_t    chunk_buffer_pixels;

    jpeg_chunk_cb_t chunk_cb;
    jpeg_done_cb_t  done_cb;
    void           *user_data;

    uint16_t image_width;             /* scaled image dimensions */
    uint16_t image_height;
    uint16_t roi_width;
    uint16_t roi_height;

    uint16_t row_fill_count[JPEG_MAX_ROI_HEIGHT];
    bool     row_flushed[JPEG_MAX_ROI_HEIGHT];
    bool     abort;
} decode_context_t;

/* ============================================================
 *  Unified job payload for the RTOS decode queue
 *
 *  use_intent == true  → worker calls jpeg_decoder_core_run_view
 *  use_intent == false → worker calls jpeg_decoder_core_run_request
 *
 *  reader at the top level is the authoritative source reference.
 *  For the intent path it duplicates intent.reader (harmless).
 *  The worker never resets reader.ctx between jobs — see header docs.
 * ============================================================ */
typedef struct {
    jpeg_reader_t   reader;      /* authoritative (cb + ctx)       */
    bool            use_intent;  /* selects which core function runs */

    union {
        jpeg_view_intent_t intent;   /* high-level path */
        struct {
            jpeg_roi_t           roi;
            jpeg_decode_scale_t  scale;
            jpeg_output_format_t out_format;
            uint16_t            *chunk_buffer;
            size_t               chunk_buffer_pixels;
        } raw;                       /* low-level path  */
    };

    void            *work_buffer;
    size_t           work_buffer_size;
    jpeg_chunk_cb_t  chunk_callback;
    jpeg_done_cb_t   done_callback;
    void            *user_data;
} decode_job_t;

/* ============================================================
 *  Core internal API  (called from RTOS worker or directly)
 *
 *  Both functions run a single forward pass:
 *    tjpgd_sys_prepare → scale resolution / ROI computation → tjpgd_sys_decomp
 *  No rewind is performed; no seek is required from the source.
 *
 *  done_callback is ALWAYS fired — even on early parameter errors.
 *  Return value mirrors done_callback.result.
 * ============================================================ */

/**
 * High-level runner.
 * Scale and ROI are resolved from intent after prepare.
 * JPEG_SCALE_AUTO is valid in intent->scale.
 */
jpeg_decode_result_t jpeg_decoder_core_run_view(
    const jpeg_view_intent_t *intent,
    void                     *workbuf,
    size_t                    workbuf_size,
    jpeg_chunk_cb_t           chunk_cb,
    jpeg_done_cb_t            done_cb,
    void                     *user_data
);

/**
 * Low-level runner.
 * ROI is pre-supplied in unscaled JPEG coords; req->scale must not be AUTO.
 */
jpeg_decode_result_t jpeg_decoder_core_run_request(
    const jpeg_decode_request_t *req
);

#endif /* JPEG_DECODER_INTERNAL_H */