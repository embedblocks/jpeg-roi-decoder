#include <unistd.h>
#include <string.h>
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jpeg_roi_decoder.h"
#include "sd_mount.h"

#define TAG   "JPEG_UART"
#define LCD_W 320
#define LCD_H 240

/* Work buffer */
static uint8_t  workbuf[JPEG_DECODER_WORK_BUF_DEFAULT];
/* Chunk buffer — JPEG_MCU_MAX_HEIGHT rows × LCD_W pixels */
static uint16_t chunk_buf[JPEG_CHUNK_BUF_PIXELS(LCD_W)];

/* ============================================================
 *  File source — caller-implemented, not shipped by component.
 *
 *  dst == NULL means TJpgDec wants to skip forward (metadata
 *  segment it doesn't need). Use fseek — zero RAM, zero copy.
 * ============================================================ */

typedef struct {
    FILE *fp;
} file_ctx_t;

static size_t file_read_cb(uint8_t *dst, size_t max, void *vctx)
{
    file_ctx_t *fc = vctx;
    if (dst == NULL) {
        /* Skip fast-path — seek forward, no read into RAM */
        long before = ftell(fc->fp);
        fseek(fc->fp, (long)max, SEEK_CUR);
        long after  = ftell(fc->fp);
        return (size_t)(after - before);
    }
    return fread(dst, 1, max, fc->fp);
}

/* ============================================================
 *  user_data carries both file handles so on_done can close
 *  them both. Never close either file in app_main — the worker
 *  task is still reading fin when decode_view returns.
 * ============================================================ */

typedef struct {
    FILE *fin;
    FILE *fout;
} decode_files_t;

/* -------------------------------------------------------
 * on_chunk — called once per decoded row (worker task context)
 * ------------------------------------------------------- */
static bool on_chunk(const jpeg_chunk_event_t *evt)
{
    if (evt->width != LCD_W) {
        /* Do NOT ESP_LOGE here if stdout is the binary pipe */
        return false;
    }

    ESP_LOGI(TAG, "Decoded byte count %d", (int)evt->byte_count);

    decode_files_t *files = (decode_files_t *)evt->user_data;
    int ret = fwrite(evt->pixels, evt->byte_count, 1, files->fout);
    return ret == 1;
}

/* -------------------------------------------------------
 * on_done — called once decode is fully complete (or failed).
 * Close BOTH files here. This is the only safe place because
 * it runs after the last fread/fwrite in the worker task.
 * ------------------------------------------------------- */
static void on_done(const jpeg_done_event_t *evt)
{
    decode_files_t *files = (decode_files_t *)evt->user_data;

    fclose(files->fout);
    fclose(files->fin);   /* safe — worker will never touch fin again */

    if (evt->result != JPEG_DECODE_OK)
        ESP_LOGE(TAG, "Decode failed: %s",
                 jpeg_decoder_err_to_str(evt->result));
}

/* ============================================================
 *  Entry point
 * ============================================================ */

/*
 * Static so the worker task can safely reference it after
 * app_main returns from jpeg_decoder_decode_view().
 */
static file_ctx_t    s_file_ctx;
static decode_files_t s_files;

void app_main(void)
{
    sd_mount_init();
    jpeg_decoder_init();

    FILE *fin = fopen("/sdcard/testimg.jpg", "rb");
    if (!fin) {
        ESP_LOGE(TAG, "Failed to open input image");
        return;
    }

    FILE *fout = fopen("/sdcard/rgb565.raw", "wb");
    if (!fout) {
        fclose(fin);
        ESP_LOGE(TAG, "Failed to open output file");
        return;
    }

    /* Initialise source context — must outlive the decode job */
    s_file_ctx = (file_ctx_t){ .fp = fin };

    /* Initialise file pair — on_done will close both */
    s_files = (decode_files_t){ .fin = fin, .fout = fout };

    /* Build the view intent */
    jpeg_view_intent_t view = jpeg_view_default(LCD_W, LCD_H);
    view.out_format   = JPEG_OUTPUT_RGB565;
    view.chunk_buffer = chunk_buf;
    view.scale        = JPEG_SCALE_AUTO;
    view.pan_x        = -100;
    view.pan_y        = +150;
    view.reader       = (jpeg_reader_t){ .cb = file_read_cb, .ctx = &s_file_ctx };

    /*
     * Returns immediately — request is queued for the worker task.
     * JPEG_DECODE_OK here means "accepted", NOT "decode succeeded".
     * Do NOT touch fin or fout after this point — the worker owns them
     * until on_done fires.
     */
    jpeg_decode_result_t res = jpeg_decoder_decode_view(
        &view,
        workbuf, sizeof(workbuf),
        on_chunk,
        on_done,
        &s_files   /* both file handles, closed in on_done */
    );

    if (res != JPEG_DECODE_OK) {
        /* Rejected before queuing — safe to clean up here */
        ESP_LOGE(TAG, "decode_view rejected: %s",
                 jpeg_decoder_err_to_str(res));
        fclose(fout);
        fclose(fin);
        return;
    }

    /* !! Do NOT fclose(fin) or fclose(fout) here !!
     * The worker task is still decoding. on_done handles cleanup. */

    ESP_LOGI(TAG, "Decode queued — waiting for completion");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}