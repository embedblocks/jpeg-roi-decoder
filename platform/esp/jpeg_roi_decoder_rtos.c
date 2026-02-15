
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "jpeg_roi_decoder.h"

//#define WORK_BUF_SIZE     4096
#define DECODE_QUEUE_LEN  4
#define TASK_STACK_SIZE   8192
#define TASK_PRIORITY     5


static QueueHandle_t s_decode_queue = NULL;
static TaskHandle_t  s_worker_task  = NULL;
static const char *TAG = "jpeg_decoder";


jpeg_decode_result_t
jpeg_decoder_core_run(
    const jpeg_decode_request_t *req,
    jpeg_done_event_t *done_evt,
    void *workbuf,
    size_t workbuf_size
);


static void jpeg_worker_task(void *arg)
{
    jpeg_decode_request_t req;

    while (1) {
        if (xQueueReceive(s_decode_queue, &req, portMAX_DELAY) == pdTRUE) {

            jpeg_done_event_t evt;

            
            jpeg_decode_result_t res =
                jpeg_decoder_core_run(&req,
                                      &evt,
                                      req.work_buffer,
                                      req.work_buffer_size);

            if (req.done_callback) {
                req.done_callback(&evt);
            }
        }
    }
}





bool jpeg_decoder_init(void)
{
    s_decode_queue = xQueueCreate(DECODE_QUEUE_LEN,
                                  sizeof(jpeg_decode_request_t));

    if (!s_decode_queue) {
        ESP_LOGE(TAG, "Queue create failed");
        return false;
    }

    BaseType_t ret = xTaskCreate(
        jpeg_worker_task,
        "jpeg_worker",
        TASK_STACK_SIZE,
        NULL,
        TASK_PRIORITY,
        &s_worker_task
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Task create failed");
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


jpeg_decode_result_t
jpeg_decoder_decode(const jpeg_decode_request_t *req)
{
    if (!s_decode_queue)
        return ESP_FAIL;

    if (xQueueSend(s_decode_queue, req, 0) != pdTRUE) {
        return ESP_FAIL;
    }

    return JPEG_DECODE_OK;
}
