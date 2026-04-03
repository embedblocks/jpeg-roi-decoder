/*
 * test_jpeg_roi_decoder.c
 *
 * Unity hardware tests for jpeg_roi_decoder.
 * All callbacks are at file scope — no nested functions (GCC nested
 * functions use stack trampolines which crash on Xtensa/ESP32).
 *
 * Assets required in CMakeLists.txt:
 *   target_add_binary_files(${COMPONENT_TARGET}
 *       "assets/test_320x240.jpg"   -- position-encoded (generate_test.py)
 *       "assets/test_16x16.jpg"     -- any tiny valid JPEG
 *   )
 *
 * Position encoding in test_320x240.jpg:
 *   R = x * 255 / 319   (left=0, right=255)
 *   G = y * 255 / 239   (top=0,  bottom=255)
 *   B = 0
 */

#include <string.h>
#include "unity.h"
#include "esp_log.h"
#include "jpeg_roi_decoder.h"

static const char *TAG = "test_jpeg";

/* ------------------------------------------------------------------ */
/*  Binary assets                                                      */
/* ------------------------------------------------------------------ */

extern const uint8_t test_320x240_jpg_start[] asm("_binary_test_320x240_jpg_start");
extern const uint8_t test_320x240_jpg_end[]   asm("_binary_test_320x240_jpg_end");
extern const uint8_t test_16x16_jpg_start[]   asm("_binary_test_16x16_jpg_start");
extern const uint8_t test_16x16_jpg_end[]     asm("_binary_test_16x16_jpg_end");

/* ------------------------------------------------------------------ */
/*  Shared buffers                                                     */
/* ------------------------------------------------------------------ */

static uint8_t  s_workbuf[JPEG_DECODER_WORK_BUF_DEFAULT];
static uint16_t s_chunk_buf[JPEG_CHUNK_BUF_PIXELS(320)];

/* ------------------------------------------------------------------ */
/*  Unity hooks                                                        */
/* ------------------------------------------------------------------ */

void setUp(void)    { jpeg_decoder_init(); }
void tearDown(void) { jpeg_decoder_deinit(); }

/* ------------------------------------------------------------------ */
/*  Source helpers                                                     */
/* ------------------------------------------------------------------ */

static jpeg_source_t make_src_320x240(void)
{
    static jpeg_source_t src;
    jpeg_decoder_source_from_buffer(&src,
        test_320x240_jpg_start,
        test_320x240_jpg_end - test_320x240_jpg_start);
    return src;
}

static jpeg_source_t make_src_16x16(void)
{
    static jpeg_source_t src;
    jpeg_decoder_source_from_buffer(&src,
        test_16x16_jpg_start,
        test_16x16_jpg_end - test_16x16_jpg_start);
    return src;
}

/* ------------------------------------------------------------------ */
/*  Position encoding helpers                                          */
/* ------------------------------------------------------------------ */

#define POSITION_TOLERANCE  12   /* absorbs JPEG compression rounding */

static uint8_t expected_r(uint16_t x)
{
    return (uint8_t)((uint32_t)x * 255 / 319) & 0xF8;
}

static uint8_t expected_g(uint16_t y)
{
    return (uint8_t)((uint32_t)y * 255 / 239) & 0xFC;
}

static int abs_diff(int a, int b) { return a > b ? a - b : b - a; }

static void rgb565_decode(uint16_t p, uint8_t *r8, uint8_t *g8)
{
    *r8 = ((p >> 11) & 0x1F) << 3;
    *g8 = ((p >>  5) & 0x3F) << 2;
}

/* ------------------------------------------------------------------ */
/*  Generic capture context — used by most callbacks                  */
/* ------------------------------------------------------------------ */

#define CAPTURE_W  320

typedef struct {
    uint16_t first_row[CAPTURE_W];
    uint16_t last_row[CAPTURE_W];
    uint16_t rows_received;
    uint16_t last_y;
    bool     y_monotonic;
    bool     width_consistent;
    uint16_t expected_width;
    /* abort control */
    bool     do_abort;
    uint16_t abort_at_row;
} capture_ctx_t;

static capture_ctx_t s_cap;   /* one shared instance, reset before each test */

static void reset_capture(uint16_t expected_width)
{
    memset(&s_cap, 0, sizeof(s_cap));
    s_cap.width_consistent = true;
    s_cap.expected_width   = expected_width;
}

/* ------------------------------------------------------------------ */
/*  File-scope callbacks                                               */
/* ------------------------------------------------------------------ */

static bool capture_cb(const jpeg_chunk_event_t *evt)
{
    capture_ctx_t *ctx = (capture_ctx_t *)evt->user_data;

    if (ctx->do_abort && evt->y >= ctx->abort_at_row)
        return false;

    if (evt->width != ctx->expected_width)
        ctx->width_consistent = false;

    if (ctx->rows_received == 0) {
        memcpy(ctx->first_row, evt->pixels,
               evt->width * sizeof(uint16_t));
        ctx->y_monotonic = true;
    } else {
        if (evt->y != ctx->last_y + 1)
            ctx->y_monotonic = false;
    }

    memcpy(ctx->last_row, evt->pixels,
           evt->width * sizeof(uint16_t));

    ctx->last_y = evt->y;
    ctx->rows_received++;
    return true;
}

/* Captures only row 0 into a caller-provided buffer */
typedef struct {
    uint16_t *dst;
    uint16_t  dst_w;
    bool      captured;
} row0_ctx_t;

static bool row0_cb(const jpeg_chunk_event_t *evt)
{
    row0_ctx_t *ctx = (row0_ctx_t *)evt->user_data;
    if (evt->y == 0 && !ctx->captured) {
        memcpy(ctx->dst, evt->pixels, ctx->dst_w * sizeof(uint16_t));
        ctx->captured = true;
    }
    return true;
}

/* Captures only the first pixel of row 0 into a uint16_t */
typedef struct {
    uint16_t pixel;
    bool     captured;
} first_pixel_ctx_t;

static bool first_pixel_cb(const jpeg_chunk_event_t *evt)
{
    first_pixel_ctx_t *ctx = (first_pixel_ctx_t *)evt->user_data;
    if (evt->y == 0 && !ctx->captured) {
        ctx->pixel    = ((const uint16_t *)evt->pixels)[0];
        ctx->captured = true;
    }
    return true;
}

static void noop_done(const jpeg_done_event_t *evt) { (void)evt; }

/* ================================================================== */
/*  GROUP 1 — Basic decode properties                                */
/* ================================================================== */

TEST_CASE("decode_view: delivers exactly lcd_height rows in order",
          "[jpeg][basic]")
{
    reset_capture(320);

    jpeg_view_t view  = jpeg_view_default(320, 240);
    view.chunk_buffer = s_chunk_buf;

    jpeg_decoder_decode_view(make_src_320x240(), &view,
        s_workbuf, sizeof(s_workbuf),
        capture_cb, noop_done, &s_cap);

    TEST_ASSERT_EQUAL(240, s_cap.rows_received);
    TEST_ASSERT_TRUE(s_cap.y_monotonic);
    TEST_ASSERT_TRUE(s_cap.width_consistent);
}

/* ================================================================== */
/*  GROUP 2 — ROI pixel correctness (high-level API)                */
/* ================================================================== */

TEST_CASE("ROI: centered decode — top-left pixel encodes image origin (0,0)",
          "[jpeg][roi][pixels]")
{
    reset_capture(320);

    jpeg_view_t view  = jpeg_view_default(320, 240);
    view.chunk_buffer = s_chunk_buf;

    jpeg_decoder_decode_view(make_src_320x240(), &view,
        s_workbuf, sizeof(s_workbuf),
        capture_cb, noop_done, &s_cap);

    uint8_t r, g;
    rgb565_decode(s_cap.first_row[0], &r, &g);
    TEST_ASSERT_LESS_THAN(POSITION_TOLERANCE, abs_diff(r, expected_r(0)));
    TEST_ASSERT_LESS_THAN(POSITION_TOLERANCE, abs_diff(g, expected_g(0)));
}

TEST_CASE("ROI: centered decode — bottom-right pixel encodes (319,239)",
          "[jpeg][roi][pixels]")
{
    reset_capture(320);

    jpeg_view_t view  = jpeg_view_default(320, 240);
    view.chunk_buffer = s_chunk_buf;

    jpeg_decoder_decode_view(make_src_320x240(), &view,
        s_workbuf, sizeof(s_workbuf),
        capture_cb, noop_done, &s_cap);

    uint8_t r, g;
    rgb565_decode(s_cap.last_row[319], &r, &g);
    TEST_ASSERT_LESS_THAN(POSITION_TOLERANCE, abs_diff(r, expected_r(319)));
    TEST_ASSERT_LESS_THAN(POSITION_TOLERANCE, abs_diff(g, expected_g(239)));
}

TEST_CASE("ROI: pan_x shifts decoded region — left edge R value increases",
          "[jpeg][roi][pan]")
{
    /* Centered decode */
    reset_capture(320);
    jpeg_view_t view  = jpeg_view_default(320, 240);
    view.chunk_buffer = s_chunk_buf;
    jpeg_decoder_decode_view(make_src_320x240(), &view,
        s_workbuf, sizeof(s_workbuf), capture_cb, noop_done, &s_cap);
    uint8_t r_center, g_center;
    rgb565_decode(s_cap.first_row[0], &r_center, &g_center);

    /* Panned decode */
    reset_capture(320);
    view              = jpeg_view_default(320, 240);
    view.chunk_buffer = s_chunk_buf;
    view.pan_x        = 50;
    jpeg_decoder_decode_view(make_src_320x240(), &view,
        s_workbuf, sizeof(s_workbuf), capture_cb, noop_done, &s_cap);
    uint8_t r_pan, g_pan;
    rgb565_decode(s_cap.first_row[0], &r_pan, &g_pan);

    /* pan_x=50 → viewport shifted right → left edge encodes larger x → larger R */
    TEST_ASSERT_GREATER_THAN(r_center + POSITION_TOLERANCE, r_pan);
}

TEST_CASE("ROI: pan_y shifts decoded region — top edge G value increases",
          "[jpeg][roi][pan]")
{
    reset_capture(320);
    jpeg_view_t view  = jpeg_view_default(320, 240);
    view.chunk_buffer = s_chunk_buf;
    jpeg_decoder_decode_view(make_src_320x240(), &view,
        s_workbuf, sizeof(s_workbuf), capture_cb, noop_done, &s_cap);
    uint8_t r_center, g_center;
    rgb565_decode(s_cap.first_row[0], &r_center, &g_center);

    reset_capture(320);
    view              = jpeg_view_default(320, 240);
    view.chunk_buffer = s_chunk_buf;
    view.pan_y        = 50;
    jpeg_decoder_decode_view(make_src_320x240(), &view,
        s_workbuf, sizeof(s_workbuf), capture_cb, noop_done, &s_cap);
    uint8_t r_pan, g_pan;
    rgb565_decode(s_cap.first_row[0], &r_pan, &g_pan);

    /* pan_y=50 → viewport shifted down → top edge encodes larger y → larger G */
    TEST_ASSERT_GREATER_THAN(g_center + POSITION_TOLERANCE, g_pan);
}

TEST_CASE("ROI: extreme positive pan clamped — decode succeeds, full rows delivered",
          "[jpeg][roi][pan]")
{
    reset_capture(320);
    jpeg_view_t view  = jpeg_view_default(320, 240);
    view.chunk_buffer = s_chunk_buf;
    view.pan_x        = 99999;
    view.pan_y        = 99999;

    jpeg_decode_result_t res = jpeg_decoder_decode_view(
        make_src_320x240(), &view,
        s_workbuf, sizeof(s_workbuf),
        capture_cb, noop_done, &s_cap);

    TEST_ASSERT_EQUAL(JPEG_DECODE_OK, res);
    TEST_ASSERT_EQUAL(240, s_cap.rows_received);
}

TEST_CASE("ROI: extreme negative pan clamped — top-left pixel encodes origin",
          "[jpeg][roi][pan]")
{
    reset_capture(320);
    jpeg_view_t view  = jpeg_view_default(320, 240);
    view.chunk_buffer = s_chunk_buf;
    view.pan_x        = -99999;
    view.pan_y        = -99999;

    jpeg_decode_result_t res = jpeg_decoder_decode_view(
        make_src_320x240(), &view,
        s_workbuf, sizeof(s_workbuf),
        capture_cb, noop_done, &s_cap);

    TEST_ASSERT_EQUAL(JPEG_DECODE_OK, res);

    uint8_t r, g;
    rgb565_decode(s_cap.first_row[0], &r, &g);
    TEST_ASSERT_LESS_THAN(POSITION_TOLERANCE, abs_diff(r, expected_r(0)));
    TEST_ASSERT_LESS_THAN(POSITION_TOLERANCE, abs_diff(g, expected_g(0)));
}

/* ================================================================== */
/*  GROUP 3 — Output dimensions respect LCD size, not image size     */
/* ================================================================== */

TEST_CASE("ROI: 160x120 viewport from 320x240 image — exactly 120 rows of 160px",
          "[jpeg][roi][dimensions]")
{
    static uint16_t small_chunk[JPEG_CHUNK_BUF_PIXELS(160)];
    reset_capture(160);

    jpeg_view_t view  = jpeg_view_default(160, 120);
    view.chunk_buffer = small_chunk;
    view.scale        = JPEG_SCALE_1_1;

    jpeg_decode_result_t res = jpeg_decoder_decode_view(
        make_src_320x240(), &view,
        s_workbuf, sizeof(s_workbuf),
        capture_cb, noop_done, &s_cap);

    TEST_ASSERT_EQUAL(JPEG_DECODE_OK, res);
    TEST_ASSERT_EQUAL(120, s_cap.rows_received);
    TEST_ASSERT_TRUE(s_cap.width_consistent);
}

/* ================================================================== */
/*  GROUP 4 — Low-level ROI API (pixel correctness)                 */
/* ================================================================== */

TEST_CASE("ROI low-level: top-left 16x16 corner encodes position (0,0)",
          "[jpeg][roi][lowlevel]")
{
    static uint16_t roi_chunk[JPEG_CHUNK_BUF_PIXELS(16)];
    static uint16_t captured_row[16];

    row0_ctx_t ctx = { .dst = captured_row, .dst_w = 16 };

    jpeg_decode_request_t req = {
        .source              = make_src_320x240(),
        .roi                 = { .left=0, .top=0, .right=15, .bottom=15 },
        .scale               = JPEG_SCALE_1_1,
        .out_format          = JPEG_OUTPUT_RGB565,
        .work_buffer         = s_workbuf,
        .work_buffer_size    = sizeof(s_workbuf),
        .chunk_buffer        = roi_chunk,
        .chunk_buffer_pixels = JPEG_CHUNK_BUF_PIXELS(16),
        .chunk_callback      = row0_cb,
        .done_callback       = NULL,
        .user_data           = &ctx,
    };

    jpeg_decode_result_t res = jpeg_decoder_decode(&req);
    TEST_ASSERT_EQUAL(JPEG_DECODE_OK, res);
    TEST_ASSERT_TRUE(ctx.captured);

    uint8_t r, g;
    rgb565_decode(captured_row[0], &r, &g);
    TEST_ASSERT_LESS_THAN(POSITION_TOLERANCE, abs_diff(r, expected_r(0)));
    TEST_ASSERT_LESS_THAN(POSITION_TOLERANCE, abs_diff(g, expected_g(0)));
}

TEST_CASE("ROI low-level: center region encodes center coordinates",
          "[jpeg][roi][lowlevel]")
{
    /*
     * This is the key ROI correctness test.
     * If ROI math is wrong, you get (0,0) pixel values instead of (152,112).
     */
    const uint16_t cx = 152, cy = 112;

    static uint16_t roi_chunk[JPEG_CHUNK_BUF_PIXELS(16)];
    static uint16_t captured_row[16];

    row0_ctx_t ctx = { .dst = captured_row, .dst_w = 16 };

    jpeg_decode_request_t req = {
        .source              = make_src_320x240(),
        .roi                 = { .left=cx, .top=cy,
                                 .right=cx+15, .bottom=cy+15 },
        .scale               = JPEG_SCALE_1_1,
        .out_format          = JPEG_OUTPUT_RGB565,
        .work_buffer         = s_workbuf,
        .work_buffer_size    = sizeof(s_workbuf),
        .chunk_buffer        = roi_chunk,
        .chunk_buffer_pixels = JPEG_CHUNK_BUF_PIXELS(16),
        .chunk_callback      = row0_cb,
        .done_callback       = NULL,
        .user_data           = &ctx,
    };

    jpeg_decode_result_t res = jpeg_decoder_decode(&req);
    TEST_ASSERT_EQUAL(JPEG_DECODE_OK, res);
    TEST_ASSERT_TRUE(ctx.captured);

    uint8_t r, g;
    rgb565_decode(captured_row[0], &r, &g);
    TEST_ASSERT_LESS_THAN(POSITION_TOLERANCE, abs_diff(r, expected_r(cx)));
    TEST_ASSERT_LESS_THAN(POSITION_TOLERANCE, abs_diff(g, expected_g(cy)));
}

TEST_CASE("ROI low-level: different ROIs produce different first pixels",
          "[jpeg][roi][lowlevel]")
{
    static uint16_t roi_chunk[JPEG_CHUNK_BUF_PIXELS(16)];
    first_pixel_ctx_t ctx_a = {0};
    first_pixel_ctx_t ctx_b = {0};

    /* ROI A: top-left corner */
    jpeg_decode_request_t req = {
        .source              = make_src_320x240(),
        .roi                 = { 0, 0, 15, 15 },
        .scale               = JPEG_SCALE_1_1,
        .out_format          = JPEG_OUTPUT_RGB565,
        .work_buffer         = s_workbuf,
        .work_buffer_size    = sizeof(s_workbuf),
        .chunk_buffer        = roi_chunk,
        .chunk_buffer_pixels = JPEG_CHUNK_BUF_PIXELS(16),
        .chunk_callback      = first_pixel_cb,
        .done_callback       = NULL,
        .user_data           = &ctx_a,
    };
    jpeg_decoder_decode(&req);

    /* ROI B: bottom-right corner */
    req.source     = make_src_320x240();
    req.roi        = (jpeg_roi_t){ 304, 224, 319, 239 };
    req.user_data  = &ctx_b;
    jpeg_decoder_decode(&req);

    TEST_ASSERT_TRUE(ctx_a.captured);
    TEST_ASSERT_TRUE(ctx_b.captured);
    TEST_ASSERT_NOT_EQUAL(ctx_a.pixel, ctx_b.pixel);
}

/* ================================================================== */
/*  GROUP 5 — Error handling                                         */
/* ================================================================== */

TEST_CASE("error: decode_view fails when chunk_buffer is NULL",
          "[jpeg][error]")
{
    jpeg_view_t view = jpeg_view_default(320, 240);
    /* chunk_buffer intentionally left NULL */

    jpeg_decode_result_t res = jpeg_decoder_decode_view(
        make_src_320x240(), &view,
        s_workbuf, sizeof(s_workbuf), NULL, NULL, NULL);

    TEST_ASSERT_EQUAL(JPEG_DECODE_ERR_PARAM, res);
}

TEST_CASE("error: decode_view fails when view is NULL",
          "[jpeg][error]")
{
    jpeg_decode_result_t res = jpeg_decoder_decode_view(
        make_src_320x240(), NULL,
        s_workbuf, sizeof(s_workbuf), NULL, NULL, NULL);

    TEST_ASSERT_EQUAL(JPEG_DECODE_ERR_PARAM, res);
}

TEST_CASE("error: decode_view fails when work_buffer is NULL",
          "[jpeg][error]")
{
    jpeg_view_t view  = jpeg_view_default(320, 240);
    view.chunk_buffer = s_chunk_buf;

    jpeg_decode_result_t res = jpeg_decoder_decode_view(
        make_src_320x240(), &view, NULL, 0, NULL, NULL, NULL);

    TEST_ASSERT_EQUAL(JPEG_DECODE_ERR_PARAM, res);
}

TEST_CASE("error: decode rejects JPEG_SCALE_AUTO in low-level API",
          "[jpeg][error]")
{
    static uint16_t buf[JPEG_CHUNK_BUF_PIXELS(16)];

    jpeg_decode_request_t req = {
        .source              = make_src_16x16(),
        .roi                 = { 0, 0, 15, 15 },
        .scale               = JPEG_SCALE_AUTO,
        .out_format          = JPEG_OUTPUT_RGB565,
        .work_buffer         = s_workbuf,
        .work_buffer_size    = sizeof(s_workbuf),
        .chunk_buffer        = buf,
        .chunk_buffer_pixels = JPEG_CHUNK_BUF_PIXELS(16),
    };

    TEST_ASSERT_EQUAL(JPEG_DECODE_ERR_PARAM, jpeg_decoder_decode(&req));
}

TEST_CASE("error: decode rejects ROI outside image bounds",
          "[jpeg][error]")
{
    static uint16_t buf[JPEG_CHUNK_BUF_PIXELS(16)];

    jpeg_decode_request_t req = {
        .source              = make_src_16x16(),
        .roi                 = { 0, 0, 999, 15 },
        .scale               = JPEG_SCALE_1_1,
        .out_format          = JPEG_OUTPUT_RGB565,
        .work_buffer         = s_workbuf,
        .work_buffer_size    = sizeof(s_workbuf),
        .chunk_buffer        = buf,
        .chunk_buffer_pixels = JPEG_CHUNK_BUF_PIXELS(16),
    };

    TEST_ASSERT_EQUAL(JPEG_DECODE_ERR_PARAM, jpeg_decoder_decode(&req));
}

TEST_CASE("error: returning false from callback gives JPEG_DECODE_ABORTED",
          "[jpeg][error]")
{
    reset_capture(320);
    s_cap.do_abort    = true;
    s_cap.abort_at_row = 10;

    jpeg_view_t view  = jpeg_view_default(320, 240);
    view.chunk_buffer = s_chunk_buf;

    jpeg_decode_result_t res = jpeg_decoder_decode_view(
        make_src_320x240(), &view,
        s_workbuf, sizeof(s_workbuf),
        capture_cb, NULL, &s_cap);

    TEST_ASSERT_EQUAL(JPEG_DECODE_ABORTED, res);
    TEST_ASSERT_LESS_THAN(240, s_cap.rows_received);
}

/* ================================================================== */
/*  GROUP 6 — Probe                                                  */
/* ================================================================== */

TEST_CASE("probe: returns correct 320x240 dimensions",
          "[jpeg][probe]")
{
    jpeg_image_info_t info = {0};
    jpeg_decode_result_t res = jpeg_decoder_probe(
        make_src_320x240(), &info, s_workbuf, sizeof(s_workbuf));

    TEST_ASSERT_EQUAL(JPEG_DECODE_OK, res);
    TEST_ASSERT_EQUAL(320, info.width);
    TEST_ASSERT_EQUAL(240, info.height);
}

TEST_CASE("probe: restores source position — probe twice gives same result",
          "[jpeg][probe]")
{
    static jpeg_source_t src;
    jpeg_decoder_source_from_buffer(&src,
        test_320x240_jpg_start,
        test_320x240_jpg_end - test_320x240_jpg_start);

    jpeg_image_info_t info = {0};
    jpeg_decoder_probe(src, &info, s_workbuf, sizeof(s_workbuf));

    jpeg_decode_result_t res = jpeg_decoder_probe(
        src, &info, s_workbuf, sizeof(s_workbuf));

    TEST_ASSERT_EQUAL(JPEG_DECODE_OK, res);
    TEST_ASSERT_EQUAL(320, info.width);
    TEST_ASSERT_EQUAL(240, info.height);
}

TEST_CASE("probe: fails when info_out is NULL",
          "[jpeg][probe]")
{
    TEST_ASSERT_EQUAL(JPEG_DECODE_ERR_PARAM,
        jpeg_decoder_probe(make_src_16x16(), NULL,
                           s_workbuf, sizeof(s_workbuf)));
}

TEST_CASE("probe: fails when work_buffer is NULL",
          "[jpeg][probe]")
{
    jpeg_image_info_t info = {0};
    TEST_ASSERT_EQUAL(JPEG_DECODE_ERR_PARAM,
        jpeg_decoder_probe(make_src_16x16(), &info, NULL, 0));
}

/* ================================================================== */
/*  GROUP 7 — err_to_str                                             */
/* ================================================================== */

TEST_CASE("err_to_str: all result codes return non-NULL string",
          "[jpeg][util]")
{
    TEST_ASSERT_NOT_NULL(jpeg_decoder_err_to_str(JPEG_DECODE_OK));
    TEST_ASSERT_NOT_NULL(jpeg_decoder_err_to_str(JPEG_DECODE_ABORTED));
    TEST_ASSERT_NOT_NULL(jpeg_decoder_err_to_str(JPEG_DECODE_ERR_PARAM));
    TEST_ASSERT_NOT_NULL(jpeg_decoder_err_to_str(JPEG_DECODE_ERR_INPUT));
    TEST_ASSERT_NOT_NULL(jpeg_decoder_err_to_str(JPEG_DECODE_ERR_MEM));
    TEST_ASSERT_NOT_NULL(jpeg_decoder_err_to_str(JPEG_DECODE_ERR_FMT));
    TEST_ASSERT_NOT_NULL(jpeg_decoder_err_to_str(JPEG_DECODE_ERR_INTR));
    TEST_ASSERT_NOT_NULL(jpeg_decoder_err_to_str((jpeg_decode_result_t)99));
}

TEST_CASE("err_to_str: OK code returns exact string 'OK'",
          "[jpeg][util]")
{
    TEST_ASSERT_EQUAL_STRING("OK", jpeg_decoder_err_to_str(JPEG_DECODE_OK));
}