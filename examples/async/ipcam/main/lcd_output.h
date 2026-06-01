#pragma once

#include <stdbool.h>
#include "esp_lcd_panel_ops.h"
#include "jpeg_roi_decoder.h"

/* ============================================================
 *  lcd_output — receives decoded rows from the JPEG decoder
 *  and writes them directly to the LCD panel, one row at a time.
 *
 *  Design intent:
 *    - Zero intermediate framebuffer — each decoded row is sent
 *      to the display the moment the decoder produces it.
 *    - lcd_output_frame_begin() resets the row counter;
 *      call it once before starting each new decode.
 *    - lcd_on_chunk / lcd_on_done are passed directly as
 *      callbacks to jpeg_decoder_decode_view().
 * ============================================================ */

/**
 * Store the panel handle for use by the chunk callback.
 * Must be called once before the first frame.
 */
esp_err_t lcd_output_init();

/**
 * Reset internal row counter to 0.
 * Call this immediately before each jpeg_decoder_decode_view().
 */
void lcd_output_frame_begin(void);

/**
 * Chunk callback — compatible with jpeg_chunk_cb_t.
 *
 * Draws one decoded row to the LCD via esp_lcd_panel_draw_bitmap.
 * Returning false aborts the decode.
 */
bool lcd_on_chunk(const jpeg_chunk_event_t *evt);

/**
 * Done callback — compatible with jpeg_done_cb_t.
 * Logs completion; extend here for e.g. vsync signalling.
 */
void lcd_on_done(const jpeg_done_event_t *evt);
