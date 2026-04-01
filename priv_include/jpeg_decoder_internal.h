#ifndef JPEG_DECODER_INTERNAL_H
#define JPEG_DECODER_INTERNAL_H

#include "jpeg_roi_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Called by the worker task (FreeRTOS) and directly by jpeg_decoder_decode()
 * (sync). Not part of the public API.
 */
jpeg_decode_result_t jpeg_decoder_core_run(
    const jpeg_decode_request_t *req,
    jpeg_done_event_t           *done_evt,
    void                        *workbuf,
    size_t                       workbuf_size
);

/*
 * Pick largest scale where scaled image still covers lcd_w x lcd_h.
 */
jpeg_decode_scale_t jpeg_decoder_auto_scale(
    uint16_t img_w, uint16_t img_h,
    uint16_t lcd_w, uint16_t lcd_h
);

/*
 * Shared view-to-request preparation used by both platform adapters.
 *
 * Probes the JPEG, resolves AUTO scale, computes the centered and
 * clamped ROI, and heap-allocates a chunk buffer for one row.
 *
 * On success:
 *   - *req_out is filled and ready to pass to jpeg_decoder_core_run()
 *   - req_out->chunk_buffer is heap-allocated
 *   - req_out->done_callback is set to an internal wrapper that frees
 *     chunk_buffer and then calls the original done_callback
 *   - req_out->user_data points to the wrapper context (also heap-allocated)
 *
 * On failure:
 *   - returns error code, no heap memory is left allocated
 *
 * The platform adapter must NOT free chunk_buffer manually — the wrapper
 * done_callback always does it, including on the async FreeRTOS path.
 */
jpeg_decode_result_t jpeg_decoder_prepare_view_request(
    jpeg_source_t        source,
    const jpeg_view_t   *view,
    void                *work_buffer,
    size_t               work_buffer_size,
    jpeg_chunk_cb_t      chunk_callback,
    jpeg_done_cb_t       done_callback,
    void                *user_data,
    jpeg_decode_request_t *req_out
);

#ifdef __cplusplus
}
#endif

#endif /* JPEG_DECODER_INTERNAL_H */