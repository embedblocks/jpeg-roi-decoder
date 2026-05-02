/* jpeg_decoder_rtos.c
 *
 * FreeRTOS platform adapter.
 *
 * jpeg_decoder_decode_view() — validates and enqueues the intent; returns immediately.
 * jpeg_decoder_decode()      — validates and enqueues a raw request; returns immediately.
 *
 * The worker task owns the full decode lifecycle:
 *   tjpgd_sys_prepare + scale/ROI resolution + tjpgd_sys_decomp
 * No JPEG bytes are consumed in the calling task.
 *
 * Return value semantics (RTOS path):
 *   JPEG_DECODE_OK      — request accepted into queue (NOT decode success).
 *   JPEG_DECODE_ERR_*   — bad argument or queue full; done_callback fired with error.
 * All decode errors (including header failures) arrive via done_callback.
 *
 * Caution — shared mutable reader.ctx:
 *   Do not queue multiple jobs that share the same mutable context (e.g. a buf_ctx_t
 *   with a pos index) unless done_callback of the first job resets the context state.
 *   The worker processes jobs serially but never touches reader.ctx between them.
 */

#include "jpeg_decoder_internal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define DECODE_QUEUE_LEN  4
#define TASK_STACK_SIZE   8192
#define TASK_PRIORITY     5

static const char    *TAG            = "jpeg_decoder";
static QueueHandle_t  s_decode_queue = NULL;
static TaskHandle_t   s_worker_task  = NULL;

/* ---------------------------------------------------------- */

static void jpeg_worker_task(void *arg)
{
    decode_job_t job;

    while (1) {
        if (xQueueReceive(s_decode_queue, &job, portMAX_DELAY) != pdTRUE)
            continue;

        /*
         * Full lifecycle runs here: prepare → ROI math → decomp.
         * core_run_* always fires done_callback before returning.
         */
        if (job.use_intent) {
            /* intent.reader was copied from the caller; it is the source of truth. */
            jpeg_decoder_core_run_view(
                &job.intent,
                job.work_buffer, job.work_buffer_size,
                job.chunk_callback, job.done_callback,
                job.user_data
            );
        } else {
            jpeg_decode_request_t req = {
                .reader              = job.reader,
                .roi                 = job.raw.roi,
                .scale               = job.raw.scale,
                .out_format          = job.raw.out_format,
                .work_buffer         = job.work_buffer,
                .work_buffer_size    = job.work_buffer_size,
                .chunk_buffer        = job.raw.chunk_buffer,
                .chunk_buffer_pixels = job.raw.chunk_buffer_pixels,
                .chunk_callback      = job.chunk_callback,
                .done_callback       = job.done_callback,
                .user_data           = job.user_data,
            };
            jpeg_decoder_core_run_request(&req);
        }
    }
}

/* ---------------------------------------------------------- */

bool jpeg_decoder_init(void)
{
    s_decode_queue = xQueueCreate(DECODE_QUEUE_LEN, sizeof(decode_job_t));
    if (!s_decode_queue) {
        ESP_LOGE(TAG, "queue create failed");
        return false;
    }

    BaseType_t ret = xTaskCreate(
        jpeg_worker_task, "jpeg_worker",
        TASK_STACK_SIZE, NULL,
        TASK_PRIORITY, &s_worker_task
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        vQueueDelete(s_decode_queue);
        s_decode_queue = NULL;
        return false;
    }

    return true;
}

void jpeg_decoder_deinit(void)
{
    if (s_worker_task) {
        vTaskDelete(s_worker_task);
        s_worker_task = NULL;
    }
    if (s_decode_queue) {
        vQueueDelete(s_decode_queue);
        s_decode_queue = NULL;
    }
}

/* ---------------------------------------------------------- */

/**
 * High-level async decode.
 * Enqueues the intent as-is; all ROI math and header parsing run in the worker.
 * Returns immediately — decode result arrives via done_callback.
 */
jpeg_decode_result_t
jpeg_decoder_decode_view(
    const jpeg_view_intent_t *intent,
    void                     *work_buffer,
    size_t                    work_buffer_size,
    jpeg_chunk_cb_t           chunk_callback,
    jpeg_done_cb_t            done_callback,
    void                     *user_data
){
    if (!s_decode_queue) {
        ESP_LOGE(TAG, "decoder not initialised — call jpeg_decoder_init() first");
        return JPEG_DECODE_ERR_PARAM;
    }

    if (!intent || !intent->reader.cb || !intent->chunk_buffer || !work_buffer) {
        if (done_callback) {
            jpeg_done_event_t evt = { .result    = JPEG_DECODE_ERR_PARAM,
                                      .user_data = user_data };
            done_callback(&evt);
        }
        return JPEG_DECODE_ERR_PARAM;
    }

    decode_job_t job = {
        .use_intent       = true,
        .reader           = intent->reader,   /* top-level reference   */
        .intent           = *intent,           /* full copy incl reader */
        .work_buffer      = work_buffer,
        .work_buffer_size = work_buffer_size,
        .chunk_callback   = chunk_callback,
        .done_callback    = done_callback,
        .user_data        = user_data,
    };

    if (xQueueSend(s_decode_queue, &job, 0) != pdTRUE) {
        ESP_LOGE(TAG, "decode queue full");
        if (done_callback) {
            jpeg_done_event_t evt = { .result    = JPEG_DECODE_ERR_INTR,
                                      .user_data = user_data };
            done_callback(&evt);
        }
        return JPEG_DECODE_ERR_INTR;
    }

    return JPEG_DECODE_OK;
}

/* ---------------------------------------------------------- */

/**
 * Low-level async decode.
 * JPEG_SCALE_AUTO in req->scale is rejected immediately (before queuing).
 * Returns immediately — decode result arrives via done_callback.
 */
jpeg_decode_result_t
jpeg_decoder_decode(const jpeg_decode_request_t *req)
{
    if (!s_decode_queue) {
        ESP_LOGE(TAG, "decoder not initialised — call jpeg_decoder_init() first");
        return JPEG_DECODE_ERR_PARAM;
    }

    if (!req || !req->reader.cb || !req->chunk_buffer) {
        if (req && req->done_callback) {
            jpeg_done_event_t evt = { .result    = JPEG_DECODE_ERR_PARAM,
                                      .user_data = req->user_data };
            req->done_callback(&evt);
        }
        return JPEG_DECODE_ERR_PARAM;
    }

    if (req->scale == JPEG_SCALE_AUTO) {
        ESP_LOGE(TAG, "JPEG_SCALE_AUTO is not valid in the low-level API");
        if (req->done_callback) {
            jpeg_done_event_t evt = { .result    = JPEG_DECODE_ERR_PARAM,
                                      .user_data = req->user_data };
            req->done_callback(&evt);
        }
        return JPEG_DECODE_ERR_PARAM;
    }

    decode_job_t job = {
        .use_intent       = false,
        .reader           = req->reader,
        .raw = {
            .roi                 = req->roi,
            .scale               = req->scale,
            .out_format          = req->out_format,
            .chunk_buffer        = req->chunk_buffer,
            .chunk_buffer_pixels = req->chunk_buffer_pixels,
        },
        .work_buffer      = req->work_buffer,
        .work_buffer_size = req->work_buffer_size,
        .chunk_callback   = req->chunk_callback,
        .done_callback    = req->done_callback,
        .user_data        = req->user_data,
    };

    if (xQueueSend(s_decode_queue, &job, 0) != pdTRUE) {
        ESP_LOGE(TAG, "decode queue full");
        if (req->done_callback) {
            jpeg_done_event_t evt = { .result    = JPEG_DECODE_ERR_INTR,
                                      .user_data = req->user_data };
            req->done_callback(&evt);
        }
        return JPEG_DECODE_ERR_INTR;
    }

    return JPEG_DECODE_OK;
}