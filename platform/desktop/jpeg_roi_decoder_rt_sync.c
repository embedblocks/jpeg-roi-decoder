/* jpeg_decoder_sync.c
 *
 * Platform adapter for synchronous builds (host / Windows testing).
 * On FreeRTOS this file is replaced by jpeg_decoder_rtos.c which
 * dispatches jpeg_decoder_core_run() through a task/queue.
 */

#include "jpeg_roi_decoder.h"

/* Declared in jpeg_decoder_core.c */
jpeg_decode_result_t
jpeg_decoder_core_run(
    const jpeg_decode_request_t *req,
    jpeg_done_event_t           *done_evt,
    void                        *workbuf,
    size_t                       workbuf_size
);

/* ---------------------------------------------------------- */

bool jpeg_decoder_init(void)
{
    return true;   /* nothing to init on sync platform */
}

void jpeg_decoder_deinit(void)
{
}

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

    /* Fire done callback even on failure so caller can always clean up */
    if (req->done_callback)
        req->done_callback(&evt);

    return res;
}