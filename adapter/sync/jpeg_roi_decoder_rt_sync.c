/* jpeg_decoder_sync.c
 *
 * Synchronous platform adapter — desktop / host testing.
 *
 * Calls core runners directly and blocks until complete.
 * done_callback fires inside the core runner before returning.
 *
 * No queue, no task, no heap — a direct call stack:
 *   jpeg_decoder_decode_view() → jpeg_decoder_core_run_view()
 *   jpeg_decoder_decode()      → jpeg_decoder_core_run_request()
 */

#include "jpeg_decoder_internal.h"

/* ---------------------------------------------------------- */

bool jpeg_decoder_init(void)   { return true; }
void jpeg_decoder_deinit(void) {}

/* ---------------------------------------------------------- */

jpeg_decode_result_t
jpeg_decoder_decode_view(
    const jpeg_view_intent_t *intent,
    void                     *work_buffer,
    size_t                    work_buffer_size,
    jpeg_chunk_cb_t           chunk_callback,
    jpeg_done_cb_t            done_callback,
    void                     *user_data
){
    return jpeg_decoder_core_run_view(
        intent,
        work_buffer, work_buffer_size,
        chunk_callback, done_callback,
        user_data
    );
}

/* ---------------------------------------------------------- */

jpeg_decode_result_t
jpeg_decoder_decode(const jpeg_decode_request_t *req)
{
    return jpeg_decoder_core_run_request(req);
}