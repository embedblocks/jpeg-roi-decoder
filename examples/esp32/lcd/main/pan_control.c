#include "pan_control.h"
#include <string.h>
#include "stdbool.h"

/* ── Init ─────────────────────────────────────────────────────────────────── */

void pan_init(pan_state_t *p,
              int x_max, int y_max,
              int step_x, int step_y)
{
    memset(p, 0, sizeof(*p));

    /* Guard: if the image is smaller than the LCD in either axis,
     * clamp to 0 — the decoder will just show the whole image. */
    p->x_max  = x_max  > 0 ? x_max  : 0;
    p->y_max  = y_max  > 0 ? y_max  : 0;
    p->step_x = step_x > 0 ? step_x : 1;
    p->step_y = step_y > 0 ? step_y : 1;
    p->dx     = p->step_x;   /* start scanning right */
}

/* ── Advance ──────────────────────────────────────────────────────────────── */

void pan_next(pan_state_t *p)
{
    /* Nothing to pan — image fits within LCD on both axes */
    if (p->x_max == 0 && p->y_max == 0) return;

    /* Step X */
    p->x += p->dx;

    /*
     * Check X bounds.  When X hits a wall:
     *   1. Clamp X to the wall value.
     *   2. Flip dx (bouncing).
     *   3. Advance Y by one step.
     */
    bool x_bounced = false;

    if (p->x >= p->x_max) {
        p->x       = p->x_max;
        p->dx      = -(p->step_x);
        x_bounced  = true;
    } else if (p->x <= 0) {
        p->x       = 0;
        p->dx      = +(p->step_x);
        x_bounced  = true;
    }

    if (x_bounced) {
        p->y += p->step_y;

        /* Full image traversed — loop back to origin */
        if (p->y > p->y_max) {
            p->x  = 0;
            p->y  = 0;
            p->dx = p->step_x;   /* restart scanning right */
        }
    }
}
