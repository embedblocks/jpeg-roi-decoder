#include "lcd_output.h"
#include "esp_log.h"

static const char *TAG = "LCD_OUTPUT";

static esp_lcd_panel_handle_t s_panel       = NULL;
static int                    s_current_row = 0;

/* ── Init ─────────────────────────────────────────────────────────────────── */

void lcd_output_init(esp_lcd_panel_handle_t panel)
{
    s_panel       = panel;
    s_current_row = 0;
}

/* ── Frame begin ─────────────────────────────────────────────────────────── */

void lcd_output_frame_begin(void)
{
    s_current_row = 0;
}

/* ── Chunk callback ───────────────────────────────────────────────────────── */

bool lcd_on_chunk(const jpeg_chunk_event_t *evt)
{
    if (!s_panel) return false;

    /*
     * The decoder gives us one full row of RGB565 pixels.
     * draw_bitmap(x0, y0, x1_excl, y1_excl, data):
     *   - x range: 0 … evt->width  (exclusive end)
     *   - y range: s_current_row … s_current_row + 1
     *
     * This pushes exactly one scanline to the display with no
     * intermediate copy — the decoder's chunk_buffer IS the
     * source buffer for the SPI transfer.
     */
    esp_err_t err = esp_lcd_panel_draw_bitmap(
        s_panel,
        0,              s_current_row,
        evt->width,     s_current_row + 1,
        evt->pixels
    );

    if (err != ESP_OK) {
        /* Non-fatal: log and continue rather than aborting the frame */
        ESP_LOGW(TAG, "draw_bitmap row %d: %s", s_current_row, esp_err_to_name(err));
    }

    s_current_row++;
    return true;
}

/* ── Done callback ────────────────────────────────────────────────────────── */

void lcd_on_done(const jpeg_done_event_t *evt)
{
    ESP_LOGD(TAG, "Frame complete — %d rows drawn", s_current_row);
    (void)evt;
}
