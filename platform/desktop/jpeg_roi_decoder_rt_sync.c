#include "jpeg_roi_decoder.h"

/* core entry */
jpeg_decode_result_t
jpeg_decoder_core_run(
    const jpeg_decode_request_t *req,
    jpeg_done_event_t *done_evt,
    void *workbuf,
    size_t workbuf_size
);


bool jpeg_decoder_init(void)
{
    return true;
}

void jpeg_decoder_deinit(void)
{
}



#define WORK_BUF_SIZE 4096

jpeg_decode_result_t
jpeg_decoder_decode(const jpeg_decode_request_t *req)
{
    void *work = malloc(WORK_BUF_SIZE);
    if (!work)
        return JPEG_DECODE_ERR_MEM1;

    jpeg_done_event_t evt;

    jpeg_decode_result_t res =
        jpeg_decoder_core_run(req, &evt,
                              work,
                              WORK_BUF_SIZE);

    if (req->done_callback)
        req->done_callback(&evt);

    free(work);

    return res;
}

