#pragma once

/* ============================================================
 *  pan_control — stateless raster-scan panning over an image.
 *
 *  Pattern (viewed from above):
 *
 *    x=0 ──────────────► x_max
 *    ◄──────────────────
 *    ──────────────────►
 *    ...
 *    x=0 ──────────────► x_max   ← y wraps back to 0 here
 *
 *  Every call to pan_next() advances by one step.
 *  When y wraps, the full image has been traversed and the
 *  scan restarts from (0, 0) — creating a seamless loop.
 *
 *  Design intent:
 *    - Pure value type — no heap, no I/O, no side effects.
 *    - Caller decides step sizes and bounds; this module only
 *      advances state and reports current position.
 * ============================================================ */

typedef struct {
    /* Current pan offsets passed to the JPEG decoder */
    int x;
    int y;

    /* Internal state */
    int dx;        /* +step_x or -step_x */
    int x_max;
    int y_max;
    int step_x;
    int step_y;
} pan_state_t;

/**
 * Initialise pan state. Starts at (0, 0) scanning right.
 *
 * @param x_max   Maximum pan_x value (image_width  - lcd_width).
 *                Clamped to 0 if the image fits within the LCD.
 * @param y_max   Maximum pan_y value (image_height - lcd_height).
 * @param step_x  Horizontal pixels advanced per frame.
 * @param step_y  Vertical   pixels advanced per row-wrap.
 */
void pan_init(pan_state_t *p,
              int x_max, int y_max,
              int step_x, int step_y);

/**
 * Advance to the next frame's pan position.
 *
 * X sweeps left↔right (bouncing at 0 and x_max).
 * Y steps down by step_y each time X bounces.
 * When Y exceeds y_max the full image has been covered;
 * state resets to (0, 0) for a seamless loop.
 */
void pan_next(pan_state_t *p);
