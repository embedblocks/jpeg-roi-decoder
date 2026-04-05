/* jpeg_decoder_freertos.c
 *
 * FreeRTOS platform adapter.
 * Defines all public API functions declared in jpeg_roi_decoder.h.
 *
 * jpeg_decoder_decode()      — enqueues request, returns immediately
 * jpeg_decoder_decode_view() — prepares request (probe + ROI math),
 *                              then enqueues it, returns immediately
 *
 * The worker task calls jpeg_decoder_core_run() and fires done_callback.
 * The done_callback set by decode_view is view_done_wrapper (in core.c)
 * which frees the chunk buffer before calling the user's callback.
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
    jpeg_decode_request_t req;

    while (1) {
        if (xQueueReceive(s_decode_queue, &req, portMAX_DELAY) != pdTRUE)
            continue;

        jpeg_done_event_t evt = {0};

        jpeg_decode_result_t res =jpeg_decoder_core_run(
                                    &req, &evt,
                                    req.work_buffer,
                                    req.work_buffer_size
                                );

        /*
         * Always fire done_callback — view_done_wrapper relies on this
         * to free the heap-allocated chunk buffer and wrapper context.
         */
        evt.result = res;   // ← overwrite whatever core_run may or may not have set
        if (req.done_callback)
            req.done_callback(&evt);
    }
}

/* ---------------------------------------------------------- */

bool jpeg_decoder_init(void)
{
    s_decode_queue = xQueueCreate(DECODE_QUEUE_LEN,
                                  sizeof(jpeg_decode_request_t));
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

jpeg_decode_result_t
jpeg_decoder_decode(const jpeg_decode_request_t *req)
{
    if (!req)
        return JPEG_DECODE_ERR_PARAM;

    if (!s_decode_queue) {
        ESP_LOGE(TAG, "decoder not initialized — call jpeg_decoder_init() first");
        return JPEG_DECODE_ERR_PARAM;
    }

    if (xQueueSend(s_decode_queue, req, 0) != pdTRUE) {
        ESP_LOGE(TAG, "decode queue full");
        return JPEG_DECODE_ERR_INTR;
    }

    return JPEG_DECODE_OK;
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
    if (!s_decode_queue) {
        ESP_LOGE(TAG, "decoder not initialized — call jpeg_decoder_init() first");
        return JPEG_DECODE_ERR_PARAM;
    }

    /*
     * Probe and ROI computation happen here in the calling task —
     * probe is a blocking read regardless of platform.
     * The resulting request is then queued for the worker task.
     */
    jpeg_decode_request_t req;
    jpeg_decode_result_t res = jpeg_decoder_prepare_view_request(
        source, view,
        work_buffer, work_buffer_size,
        chunk_callback, done_callback, user_data,
        &req
    );
    if (res != JPEG_DECODE_OK)
        return res;

    if (xQueueSend(s_decode_queue, &req, 0) != pdTRUE) {
        /*
         * Queue full — req.done_callback is view_done_wrapper which
         * owns the chunk_buffer. Call it directly to free memory.
         */
        jpeg_done_event_t fail_evt = { .result = JPEG_DECODE_ERR_INTR,
                                       .user_data = req.user_data };
        req.done_callback(&fail_evt);
        ESP_LOGE(TAG, "decode queue full");
        return JPEG_DECODE_ERR_INTR;
    }

    return JPEG_DECODE_OK;
}