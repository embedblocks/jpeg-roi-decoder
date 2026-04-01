/* jpeg_decoder_sync.c
 *
 * Synchronous platform adapter for desktop / host testing.
 * Defines all public API functions declared in jpeg_roi_decoder.h.
 *
 * jpeg_decoder_decode()      — calls core_run directly, blocks
 * jpeg_decoder_decode_view() — prepares request (probe + ROI math),
 *                              then calls core_run directly, blocks
 *
 * done_callback always fires before the function returns.
 */

#include "jpeg_decoder_internal.h"

/* ---------------------------------------------------------- */

bool jpeg_decoder_init(void)  { return true; }
void jpeg_decoder_deinit(void) {}

/* ---------------------------------------------------------- */

jpeg_decode_result_t
jpeg_decoder_decode(const jpeg_decode_request_t *req)
{
    if (!req || !req->work_buffer)
        return JPEG_DECODE_ERR_PARAM;

    jpeg_done_event_t evt = {0};

    jpeg_decode_result_t res = jpeg_decoder_core_run(
        req, &evt,
        req->work_buffer,
        req->work_buffer_size
    );

    /*
     * Always fire done_callback — view_done_wrapper relies on this
     * to free the heap-allocated chunk buffer and wrapper context.
     */
    if (req->done_callback)
        req->done_callback(&evt);

    return res;
}

jpeg_decode_result_t
jpeg_decoder_decode_view(
    jpeg_source_t        source,
    const jpeg_view_t   *view,
    void                *work_buffer,
    size_t               work_buffer_size,
    jpeg_chunk_cb_t      chunk_callback,
    jpeg_done_cb_t       done_callback,
    void                *user_data
){
    jpeg_decode_request_t req;

    jpeg_decode_result_t res = jpeg_decoder_prepare_view_request(
        source, view,
        work_buffer, work_buffer_size,
        chunk_callback, done_callback, user_data,
        &req
    );
    if (res != JPEG_DECODE_OK)
        return res;

    /* Reuse jpeg_decoder_decode — it calls core_run and fires done_callback */
    return jpeg_decoder_decode(&req);
}