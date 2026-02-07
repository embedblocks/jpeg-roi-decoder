/**
 * @file jpeg_decoder.c
 * @brief JPEG ROI decoder using TJpgDec with scalable output buffer
 */

#include "jpeg_roi_decoder.h"
#include "tjpgd.h"
#include "tjpgd_sys.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <string.h>

static const char *TAG = "jpeg_decoder";

/* ================= CONFIG ================= */

#ifndef CONFIG_JPEG_QUEUE_DEPTH
#define CONFIG_JPEG_QUEUE_DEPTH 5
#endif

#ifndef CONFIG_JPEG_TASK_STACK_SIZE
#define CONFIG_JPEG_TASK_STACK_SIZE 8192
#endif

#ifndef CONFIG_JPEG_TASK_PRIORITY
#define CONFIG_JPEG_TASK_PRIORITY 5
#endif

#ifndef CONFIG_JPEG_POOL_SIZE
#define CONFIG_JPEG_POOL_SIZE 3100

#endif

#define JPEG_MAX_ROI_HEIGHT 480
/* ================= STATE ================= */

static QueueHandle_t decode_queue;
static TaskHandle_t  decode_task_handle;
static bool is_initialized;

/* TJpgDec work buffer */
static uint8_t work_pool[CONFIG_JPEG_POOL_SIZE];

/* ================= CONTEXT ================= */

typedef struct {
    FILE *fp;
    jpeg_roi_t roi;

    /* User-provided buffer (used as row / multi-row buffer) */
    uint16_t *chunk_buffer;
    size_t chunk_buffer_pixels;   // must be >= roi_width

    jpeg_chunk_cb_t chunk_cb;
    jpeg_done_cb_t  done_cb;
    void *user_data;

    /* Image info */
    uint16_t image_width;
    uint16_t image_height;

    /* ROI info */
    uint16_t roi_width;
    uint16_t roi_height;
    size_t   roi_pixels_total;

    /* --- NEW / REPLACEMENT STATE --- */

    uint16_t current_row;   // current Y inside ROI being filled
    uint16_t row_filled;    // pixels filled in current row


      /* --- row tracking (NO malloc) --- */
    uint16_t row_fill_count[JPEG_MAX_ROI_HEIGHT];
    bool     row_flushed[JPEG_MAX_ROI_HEIGHT];

    uint16_t flushed_rows_count;


    bool abort;
} decode_context_t;

/* ================= TJPG INPUT ================= */

static size_t input_func(JDEC *jd, uint8_t *buff, size_t nbyte)
{
    decode_context_t *ctx = jd->device;

    if (buff) {
        return fread(buff, 1, nbyte, ctx->fp);
    } else {
        fseek(ctx->fp, nbyte, SEEK_CUR);
        return nbyte;
    }
}

/* ================= FLUSH CHUNK ================= */
/*
static bool flush_chunk(decode_context_t *ctx)
{
    if (ctx->chunk_used == 0)
        return true;

    jpeg_chunk_info_t chunk = {
        .x = ctx->pixels_emitted % ctx->roi_width,
        .y = ctx->pixels_emitted / ctx->roi_width,
        .width  = ctx->roi_width,
        .height = ctx->chunk_used / ctx->roi_width
    };

    jpeg_chunk_event_t evt = {
        .fp = ctx->fp,
        .pixels = ctx->chunk_buffer,
        .chunk = &chunk,
        .pixel_count = ctx->chunk_used,
        .byte_count  = ctx->chunk_used * sizeof(uint16_t),
        .user_data   = ctx->user_data
    };

    bool cont = true;
    if (ctx->chunk_cb) {
        cont = ctx->chunk_cb(&evt);
    }

    ctx->pixels_emitted += ctx->chunk_used;
    ctx->chunk_used = 0;

    return cont;
}
*/
/* ================= TJPG OUTPUT ================= */


static int output_func(JDEC *jd, void *bitmap, JRECT *rect)
{
    decode_context_t *ctx = jd->device;
    uint16_t *src = bitmap;
    
    // Early rejection: MCU completely outside ROI
    if (rect->right  < ctx->roi.left  ||
        rect->left   > ctx->roi.right ||
        rect->bottom < ctx->roi.top   ||
        rect->top    > ctx->roi.bottom) {
        return 1;
    }
    
    uint16_t mcu_w = rect->right - rect->left + 1;
    
    // Calculate overlap region
    uint16_t y_start = rect->top    < ctx->roi.top    ? ctx->roi.top    : rect->top;
    uint16_t y_end   = rect->bottom > ctx->roi.bottom ? ctx->roi.bottom : rect->bottom;
    uint16_t x_start = rect->left   < ctx->roi.left   ? ctx->roi.left   : rect->left;
    uint16_t x_end   = rect->right  > ctx->roi.right  ? ctx->roi.right  : rect->right;
    
    uint16_t copy_width = x_end - x_start + 1;  // Pixels to copy per row
    
    for (uint16_t y = y_start; y <= y_end; y++) {
        uint16_t roi_y = y - ctx->roi.top;
        uint16_t roi_x = x_start - ctx->roi.left;
        uint16_t src_off = (y - rect->top) * mcu_w + (x_start - rect->left);
        
        // OPTIMIZATION 1: Use memcpy instead of pixel-by-pixel loop
        memcpy(&ctx->chunk_buffer[roi_x], 
               &src[src_off], 
               copy_width * sizeof(uint16_t));
        
        // OPTIMIZATION 2: Batch update fill count
        uint16_t new_fill_count = ctx->row_fill_count[roi_y] + copy_width;
        ctx->row_fill_count[roi_y] = new_fill_count;
        
        // OPTIMIZATION 3: Only check for row completion once per row
        if (new_fill_count == ctx->roi_width && !ctx->row_flushed[roi_y]) {
            jpeg_chunk_info_t info = {
                .x = 0,
                .y = roi_y,
                .width = ctx->roi_width,
                .height = 1
            };
            jpeg_chunk_event_t evt = {
                .pixels = ctx->chunk_buffer,
                .chunk = &info,
                .pixel_count = ctx->roi_width,
                .byte_count  = ctx->roi_width * 2,
                .user_data   = ctx->user_data
            };
            
            if (ctx->chunk_cb && !ctx->chunk_cb(&evt)) {
                ctx->abort = true;
                return 0;
            }
            ctx->row_flushed[roi_y] = true;
            ctx->flushed_rows_count++;
            
            if (ctx->flushed_rows_count == ctx->roi_height) {
                return 0;   // STOP decoding immediately
            }

        }
    }
    
    return 1;
}
/* ================= RESULT MAP ================= */

static jpeg_decode_result_t convert_result(JRESULT r)
{
    switch (r) {
        case JDR_OK:   return JPEG_DECODE_OK;
        case JDR_INTR: return JPEG_DECODE_ERR_INTR;
        case JDR_INP:  return JPEG_DECODE_ERR_INPUT;
        case JDR_MEM1: return JPEG_DECODE_ERR_MEM1;
        case JDR_MEM2: return JPEG_DECODE_ERR_MEM2;
        case JDR_PAR:  return JPEG_DECODE_ERR_PARAM;
        case JDR_FMT1: return JPEG_DECODE_ERR_FMT1;
        case JDR_FMT2: return JPEG_DECODE_ERR_FMT2;
        case JDR_FMT3: return JPEG_DECODE_ERR_FMT3;
        default:       return JPEG_DECODE_ERR_FMT1;
    }
}

/* ================= WORKER TASK ================= */

static void decode_task(void *arg)
{
    jpeg_decode_request_t req;

    while (1) {
        if (xQueueReceive(decode_queue, &req, portMAX_DELAY) != pdTRUE)
            continue;

        decode_context_t ctx = {
            .fp = req.fp,
            .roi = req.roi,
            .chunk_buffer = req.chunk_buffer,
            .chunk_buffer_pixels = req.chunk_buffer_pixels,
            .chunk_cb = req.chunk_callback,
            .done_cb  = req.done_callback,
            .user_data = req.user_data
        };

        JDEC jd;
        JRESULT jr = tjpgd_sys_prepare(&jd, input_func,
                                      work_pool, CONFIG_JPEG_POOL_SIZE,
                                      &ctx);

        jpeg_decode_result_t result;

        if (jr == JDR_OK) {
            ctx.image_width  = jd.width;
            ctx.image_height = jd.height;

            ctx.roi_width  = ctx.roi.right  - ctx.roi.left + 1;
            ctx.roi_height = ctx.roi.bottom - ctx.roi.top  + 1;
            ctx.roi_pixels_total = ctx.roi_width * ctx.roi_height;

            if (ctx.roi.right >= jd.width || ctx.roi.bottom >= jd.height) {
                result = JPEG_DECODE_ERR_PARAM;
            } else {
                jr = tjpgd_sys_decomp(&jd, output_func, 0);
                //flush_chunk(&ctx);
                //Tell whether the call was successful or not and translate to the appropriate result code.
                result = (jr == JDR_OK || ctx.abort) ? JPEG_DECODE_OK : convert_result(jr);
            }

            for (uint16_t i = 0; i < ctx.roi_height; i++) {
                ctx.row_fill_count[i] = 0;
                ctx.row_flushed[i] = false;
            }

        } else {
            result = convert_result(jr);
        }

        if (ctx.done_cb) {
            jpeg_done_event_t evt = {
                .fp = req.fp,
                .result = result,
                .image_width = ctx.image_width,
                .image_height = ctx.image_height,
                .roi = ctx.roi,
                .user_data = ctx.user_data
            };
            ctx.done_cb(&evt);
        }
    }
}

/* ================= PUBLIC API ================= */

esp_err_t jpeg_decoder_init(void)
{
    if (is_initialized)
        return ESP_OK;

    decode_queue = xQueueCreate(CONFIG_JPEG_QUEUE_DEPTH,
                                sizeof(jpeg_decode_request_t));
    if (!decode_queue)
        return ESP_ERR_NO_MEM;

    if (xTaskCreate(decode_task, "jpeg_decode",
                    CONFIG_JPEG_TASK_STACK_SIZE,
                    NULL, CONFIG_JPEG_TASK_PRIORITY,
                    &decode_task_handle) != pdPASS) {
        vQueueDelete(decode_queue);
        return ESP_ERR_NO_MEM;
    }

    is_initialized = true;
    return ESP_OK;
}

esp_err_t jpeg_decoder_decode(const jpeg_decode_request_t *req)
{
    if (!is_initialized || !req || !req->fp || !req->chunk_buffer)
        return ESP_ERR_INVALID_ARG;

    return xQueueSend(decode_queue, req, 0) == pdTRUE
           ? ESP_OK
           : ESP_ERR_TIMEOUT;
}

esp_err_t jpeg_decoder_deinit(void)
{
    if (!is_initialized)
        return ESP_ERR_INVALID_STATE;

    vTaskDelete(decode_task_handle);
    vQueueDelete(decode_queue);

    is_initialized = false;
    return ESP_OK;
}

int jpeg_decoder_get_queue_depth(void)
{
    return is_initialized ? uxQueueMessagesWaiting(decode_queue) : 0;
}
