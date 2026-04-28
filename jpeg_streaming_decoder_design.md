# JPEG ROI Decoder — Streaming Redesign Design Document

## Purpose

This document is a complete action specification for redesigning the `jpeg_roi_decoder` component.
It is self-contained: an implementor with the original source files and this document has
everything needed to produce the new code without further clarification.

---

## Context: What Exists Today

Four source files form the component:

| File | Role |
|---|---|
| `jpeg_roi_decoder.h` | Public API and all types |
| `jpeg_roi_decoder_core.c` | Core decode logic, output callbacks, ROI math |
| `jpeg_roi_decoder_rtos.c` | FreeRTOS adapter: queue, worker task, async dispatch |
| `jpeg_roi_decoder_helpers.c` | Source backends: file and buffer |

The decoder wraps **TJpgDec** (`tjpgd`), a pull-based JPEG decoder.
TJpgDec operates in two sequential phases on the same byte stream:

- `tjpgd_sys_prepare()` — reads JPEG header markers (SOI, DHT, DQT, SOF). Typically 1–4 KB.
  After this call, `jd.width` and `jd.height` are valid.
- `tjpgd_sys_decomp()` — reads scan data from wherever prepare left the stream cursor.

These two phases share a single forward stream cursor. **No rewind is needed between them.**

### Problems With The Current Design

1. **Built-in backends are unnecessary complexity.** `jpeg_roi_decoder_helpers.c` ships
   file and buffer backends that belong in application code. Every new source type
   (HTTP, UART, DMA, queue, socket) requires the component to grow. This does not scale.

2. **`jpeg_source_t` has a `seek()` operation.** The design calls `probe()` (which calls prepare),
   then `seek(0)` to rewind, then calls prepare again inside `core_run`. This double-read of
   headers exists solely to support the separate probe/decode API split. It requires seekable
   sources and makes streaming impossible.

3. **The source abstraction leaks source knowledge into the component.** An abstraction that
   ships concrete implementations is not an abstraction — it is a library. The correct boundary:
   the component defines the callback contract; the caller implements the source.

---

## Design Goals

1. The decoder sees **one abstraction: a read callback + a caller-owned context**.
   The component defines the callback signature. The caller implements it.
2. The component ships **no source backends**. File, buffer, HTTP, UART, DMA — all are
   trivial to implement by the caller with a handful of lines each.
3. The callback receives **TJpgDec's own destination buffer directly**. Zero copies in the
   hot path. No intermediate staging buffer is introduced by the component.
4. When TJpgDec wants to skip bytes it passes `dst = NULL`. The callback can implement a
   true zero-RAM skip (e.g. `fseek`) instead of reading and discarding.
5. The prepare/decomp split is used correctly: both phases run in a single forward pass.
   Scale and ROI are resolved between them using `jd.width`/`jd.height`.
6. The **output chunk buffer** is caller-allocated and passed in via the config struct.
   The component owns no heap allocations.
7. All existing decode capability (ROI, pan, scale, chunk callbacks, RTOS async) is preserved.

---

## New Type: `jpeg_read_cb_t`

The sole source abstraction. Replaces `jpeg_source_t` entirely.

```c
/**
 * Read callback — the only source abstraction the decoder requires.
 *
 * Called by the decoder whenever it needs more JPEG byte data.
 *
 * Parameters:
 *   dst  — Destination buffer owned by TJpgDec. Write JPEG source bytes
 *           directly here. If NULL, the decoder is requesting a skip/drain:
 *           advance the source by max bytes without writing to RAM.
 *   max  — Maximum bytes to write (or bytes to skip if dst is NULL).
 *   ctx  — Caller-defined context (file pointer, buffer state struct,
 *           HTTP handle, queue handle, etc.).
 *
 * Return value:
 *   Number of bytes read or skipped.
 *   Return 0 on end-of-data, timeout, or unrecoverable error.
 *   Partial returns (< max) are valid; the decoder retries internally.
 *
 * Contract:
 *   - Must not block indefinitely. Implement a timeout and return 0 on expiry.
 *   - The decoder has no watchdog. A permanently stalled callback stalls the decoder.
 *   - For seekable sources, implement the dst == NULL skip fast-path (see below).
 *   - For non-seekable streams, dst == NULL still arrives; advance and discard max
 *     bytes as best as possible, or return 0 if the source has ended.
 */
typedef size_t (*jpeg_read_cb_t)(uint8_t *dst, size_t max, void *ctx);
```

### Grouping: `jpeg_reader_t`

`read_cb` and context always travel together. A minimal struct prevents parameter list
growth and keeps call sites stable if the read contract ever needs a third field:

```c
typedef struct {
    jpeg_read_cb_t  cb;   /* mandatory, must not be NULL */
    void           *ctx;  /* passed to cb unchanged      */
} jpeg_reader_t;
```

This struct is used in every config struct and in the probe signature. It embeds no
buffer, no internal state — it is purely a (callback, context) pair.

---

## Zero-Copy Hot Path

TJpgDec calls `input_func(jd, buf, nbyte)` where `buf` is TJpgDec's own internal buffer.
The internal `input_func` passes `buf` directly to the user's callback. No intermediate
staging buffer exists inside the component. No `memcpy` is introduced.

```c
static size_t input_func(JDEC *jd, uint8_t *buf, size_t nbyte)
{
    decode_context_t *ctx = (decode_context_t *)jd->device;
    size_t total = 0;
    while (total < nbyte) {
        /* Pass buf directly — zero copy.
         * buf is NULL when TJpgDec wants a skip; pass NULL through to the callback. */
        size_t got = ctx->reader.cb(buf ? buf + total : NULL,
                                    nbyte - total,
                                    ctx->reader.ctx);
        if (got == 0) break;   /* source ended or timed out */
        total += got;
    }
    return total;
}
```

The retry loop handles partial reads without any internal buffer.

---

## NULL Skip Fast-Path

When TJpgDec wants to skip over a JPEG metadata segment it does not need, it calls
`input_func` with `buf = NULL`. The `input_func` above passes `NULL` straight through
to the callback. A callback for a seekable source implements this as a true zero-RAM seek:

```c
size_t file_read_cb(uint8_t *dst, size_t max, void *ctx)
{
    FILE *fp = ctx;
    if (dst == NULL)
        return fseek(fp, (long)max, SEEK_CUR) == 0 ? max : 0;  /* seek, no RAM */
    return fread(dst, 1, max, fp);
}
```

For non-seekable sources, the callback reads and discards — acceptable because no better
option exists for those sources. JPEG decoding occasionally needs to skip large metadata
blocks; `fseek` is strictly superior to reading into RAM and throwing the data away.

---

## No `input_buffer` in Config Structs

A previous draft added `input_buffer` and `input_buffer_size` to every config struct,
citing DMA-region control as the motivation. This was incorrect.

**DMA staging is the caller's concern, not the component's.** A caller who needs
DMA-aligned reads places the buffer inside their own `ctx` and manages it entirely
within their callback:

```c
typedef struct {
    uint8_t dma_buf[512];   /* DMA-capable, caller-placed, caller-sized */
    /* ... SD card handle, etc. */
} my_sd_ctx_t;

size_t my_sd_read_cb(uint8_t *dst, size_t max, void *vctx)
{
    my_sd_ctx_t *c = vctx;
    size_t n = max < sizeof(c->dma_buf) ? max : sizeof(c->dma_buf);

    if (dst == NULL) {
        return sd_seek_forward(n) ? n : 0;   /* skip, no DMA needed */
    }
    sd_dma_read(c->dma_buf, n);    /* DMA fills aligned buffer   */
    memcpy(dst, c->dma_buf, n);    /* one copy, caller-controlled */
    return n;
}
```

The 99% of callers who do not use DMA pay zero extra copies. The 1% who do manage their
own staging inside `ctx` — exactly where it belongs.

---

## Removed: `jpeg_source_t` and All Built-In Backends

`jpeg_source_t`, `jpeg_decoder_source_from_file()`, `jpeg_decoder_source_from_buffer()`,
and `jpeg_roi_decoder_helpers.c` are **removed entirely**.

### Migration — What Callers Write Instead

These are illustrative only. They are **not** part of the component.

#### File source (seekable, with skip fast-path)

```c
size_t file_read_cb(uint8_t *dst, size_t max, void *ctx)
{
    FILE *fp = ctx;
    if (dst == NULL)
        return fseek(fp, (long)max, SEEK_CUR) == 0 ? max : 0;
    return fread(dst, 1, max, fp);
}

/* Usage */
jpeg_reader_t reader = { .cb = file_read_cb, .ctx = fp };
```

#### In-memory buffer source

```c
typedef struct { const uint8_t *data; size_t len; size_t pos; } buf_ctx_t;

size_t buf_read_cb(uint8_t *dst, size_t max, void *vctx)
{
    buf_ctx_t *bc = vctx;
    size_t avail = bc->len - bc->pos;
    size_t n     = max < avail ? max : avail;
    if (dst)
        memcpy(dst, bc->data + bc->pos, n);
    bc->pos += n;   /* advance even on skip */
    return n;
}
```

#### FreeRTOS queue source (true streaming, with timeout)

```c
typedef struct { QueueHandle_t q; TickType_t timeout; } queue_ctx_t;

size_t queue_read_cb(uint8_t *dst, size_t max, void *vctx)
{
    queue_ctx_t *qc = vctx;
    size_t n = 0;
    while (n < max) {
        uint8_t byte;
        if (xQueueReceive(qc->q, &byte, qc->timeout) != pdTRUE)
            break;   /* timeout → return short count → decoder sees ERR_INPUT */
        if (dst) dst[n] = byte;
        n++;
    }
    return n;
}
```

---

## Updated Config Structs

### High-Level: `jpeg_view_intent_t` (replaces `jpeg_view_t`)

```c
typedef struct {
    uint16_t lcd_width;
    uint16_t lcd_height;
    int32_t  pan_x;                  /* pixels from center, clamped automatically */
    int32_t  pan_y;
    jpeg_decode_scale_t  scale;      /* JPEG_SCALE_AUTO is valid here */
    jpeg_output_format_t out_format;

    jpeg_reader_t reader;            /* cb + ctx — caller implements, no backends shipped */

    uint16_t *chunk_buffer;          /* caller-allocated; see JPEG_CHUNK_BUF_BYTES() */
} jpeg_view_intent_t;
```

`jpeg_view_default()` returns `jpeg_view_intent_t` with `reader = {NULL, NULL}` and
`chunk_buffer = NULL`. Caller must fill all pointer fields before decoding.

### Low-Level: `jpeg_decode_request_t` (updated)

```c
typedef struct {
    jpeg_reader_t        reader;               /* cb + ctx, mandatory */

    jpeg_roi_t           roi;                  /* in original unscaled JPEG coords */
    jpeg_decode_scale_t  scale;                /* JPEG_SCALE_AUTO not valid here */
    jpeg_output_format_t out_format;

    void    *work_buffer;
    size_t   work_buffer_size;

    uint16_t *chunk_buffer;
    size_t    chunk_buffer_pixels;

    jpeg_chunk_cb_t chunk_callback;
    jpeg_done_cb_t  done_callback;
    void           *user_data;
} jpeg_decode_request_t;
```

**`JPEG_SCALE_AUTO`** is rejected in this path (`JPEG_DECODE_ERR_PARAM`).

---

## Updated Internal Type: `decode_context_t`

```c
typedef struct {
    jpeg_reader_t        reader;

    jpeg_roi_t           roi;
    jpeg_decode_scale_t  scale;
    jpeg_output_format_t out_format;

    uint16_t *chunk_buffer;
    size_t    chunk_buffer_pixels;

    jpeg_chunk_cb_t chunk_cb;
    jpeg_done_cb_t  done_cb;
    void           *user_data;

    uint16_t image_width;
    uint16_t image_height;
    uint16_t roi_width;
    uint16_t roi_height;

    uint16_t row_fill_count[JPEG_MAX_ROI_HEIGHT];
    bool     row_flushed[JPEG_MAX_ROI_HEIGHT];
    bool     abort;
} decode_context_t;
```

---

## Core Decode Flow (Single Forward Pass)

```
jpeg_decoder_core_run()
    │
    ├─ input_func registered with tjpgd
    │       └─ passes TJpgDec's buf directly to reader.cb (zero copy)
    │          passes NULL through on skip requests (enables fseek fast-path)
    │
    ├─ tjpgd_sys_prepare()
    │       └─ reads ~1–4 KB of JPEG headers via input_func
    │          jd.width and jd.height are now valid
    │
    ├─ if intent->scale == JPEG_SCALE_AUTO:
    │       scale = jpeg_decoder_auto_scale(jd.width, jd.height,
    │                                       intent->lcd_width, intent->lcd_height)
    │
    ├─ compute ROI:
    │       div = 1 << scale
    │       scaled_w = jd.width / div,  scaled_h = jd.height / div
    │       cx = (scaled_w - lcd_w) / 2 + pan_x   → clamp [0, scaled_w - lcd_w]
    │       cy = (scaled_h - lcd_h) / 2 + pan_y   → clamp [0, scaled_h - lcd_h]
    │       roi.left   = cx * div,   roi.top    = cy * div
    │       roi.right  = (cx + lcd_w - 1) * div
    │       roi.bottom = (cy + lcd_h - 1) * div
    │
    ├─ validate ROI fits within image bounds → JPEG_DECODE_ERR_PARAM if not
    │
    └─ tjpgd_sys_decomp()
            └─ stream cursor already past headers (no rewind)
               output_func fires chunk callbacks row by row
```

`jpeg_decoder_prepare_view_request()` is **removed**. Its logic is absorbed into `core_run`.

---

## `jpeg_decoder_probe()` — Retained, Caller Manages Reset

Probe is retained as a caller utility. The component has no reset mechanism. The caller
is responsible for returning their source to the beginning before issuing a decode after
a probe (e.g. `fseek(fp, 0, SEEK_SET)`, resetting a buffer `pos` to 0, etc.).

```c
/**
 * Parse JPEG headers and return image dimensions.
 *
 * Consumes the source up through the header markers. The caller must reset
 * their own source context before passing it to a decode call. The component
 * provides no reset mechanism.
 *
 * For true non-seekable streams (HTTP, UART, queue), probe is not useful —
 * the stream cannot be rewound. Use the decode path directly; image dimensions
 * are available in done_callback via done_evt->image.
 */
jpeg_decode_result_t jpeg_decoder_probe(
    jpeg_reader_t      reader,
    jpeg_image_info_t *info_out,
    void              *work_buffer,
    size_t             work_buffer_size
);
```

Example caller pattern:

```c
jpeg_reader_t reader = { .cb = file_read_cb, .ctx = fp };

jpeg_decoder_probe(reader, &info, workbuf, sizeof(workbuf));
fseek(fp, 0, SEEK_SET);   /* caller resets — component does not */

jpeg_decoder_decode_view(&intent, workbuf, sizeof(workbuf), on_chunk, on_done, NULL);
```

---

## Stalled Callback Behavior — Explicit Contract

The decoder has **no internal abort or watchdog mechanism**.

If `reader.cb` blocks, the decoder task blocks. The blocking contract belongs entirely in
the callback implementation.

**Required behavior for any non-trivial callback:**
- Implement a read timeout internally.
- Return 0 when the timeout expires.
- The decoder will return `JPEG_DECODE_ERR_INPUT` via `done_callback`.

Timeout policy is decided by the callback implementor, not the decoder.

---

## Return Value Semantics — Explicit API Contract

### Synchronous path (`jpeg_decoder_core_run` called directly)

- Return value reflects decode result.
- `done_callback` also fires (always, even on error).
- Caller can use either or both.

### Async RTOS path (`jpeg_decoder_decode_view`, `jpeg_decoder_decode`)

- Return value reflects **queuing only**:
  - `JPEG_DECODE_OK` — request accepted into queue.
  - `JPEG_DECODE_ERR_PARAM` — bad argument, not queued.
  - `JPEG_DECODE_ERR_INTR` — queue full, not queued.
- **All decode errors arrive exclusively via `done_callback`.**
- Caller must not interpret `JPEG_DECODE_OK` as decode success.

---

## RTOS Layer: Worker Task Owns Full Lifecycle

**No bytes are consumed in the caller task.** Prepare and decomp both run in the worker.

```c
typedef struct {
    jpeg_reader_t   reader;

    bool use_intent;
    union {
        jpeg_view_intent_t intent;
        struct {
            jpeg_roi_t           roi;
            jpeg_decode_scale_t  scale;
            jpeg_output_format_t out_format;
            uint16_t            *chunk_buffer;
            size_t               chunk_buffer_pixels;
        } raw;
    };

    void            *work_buffer;
    size_t           work_buffer_size;
    jpeg_chunk_cb_t  chunk_callback;
    jpeg_done_cb_t   done_callback;
    void            *user_data;
} decode_job_t;
```

Probe errors (corrupt headers, truncated stream) no longer surface synchronously.
They arrive via `done_callback` with `result = JPEG_DECODE_ERR_INPUT`.

---

## Buffer Lifetime Contracts

### Work buffer
- Caller allocates (minimum `JPEG_DECODER_WORK_BUF_MIN` bytes).
- Must remain valid and unmodified until `done_callback` fires.
- Declaring as a static array is the simplest correct choice for most embedded use cases.

### Chunk buffer
- Caller allocates. Size: `JPEG_CHUNK_BUF_BYTES(lcd_width)`.
- Must remain valid until `done_callback` fires.
- Must not be read by the caller during decode (its contents are incomplete mid-row).

### Source context (`reader.ctx`)
- Caller owns entirely. The component stores the pointer and passes it to `reader.cb`.
  It never dereferences it directly.
- The underlying source (file, buffer, socket) must remain valid and readable until
  `done_callback` fires.

---

## `chunk_buffer` and Output Logic — Unchanged

Chunk buffer sizing logic, `JPEG_MCU_MAX_HEIGHT`, `JPEG_CHUNK_BUF_PIXELS()`,
`JPEG_CHUNK_BUF_BYTES()`, slot-based row accumulation in `output_func`, and
`row_fill_count`/`row_flushed` arrays are all unchanged.

---

## Public API Summary

```c
/* Init/deinit (RTOS path only) */
bool jpeg_decoder_init(void);
void jpeg_decoder_deinit(void);

/* Probe — caller must reset their own source after this call */
jpeg_decode_result_t jpeg_decoder_probe(
    jpeg_reader_t      reader,
    jpeg_image_info_t *info_out,
    void              *work_buffer,
    size_t             work_buffer_size
);

/* High-level API — scale and ROI resolved from intent after header parse */
jpeg_decode_result_t jpeg_decoder_decode_view(
    const jpeg_view_intent_t *intent,    /* contains reader and chunk_buffer */
    void                     *work_buffer,
    size_t                    work_buffer_size,
    jpeg_chunk_cb_t           chunk_callback,
    jpeg_done_cb_t            done_callback,
    void                     *user_data
);

/* Low-level API — caller pre-computes ROI; JPEG_SCALE_AUTO rejected */
jpeg_decode_result_t jpeg_decoder_decode(const jpeg_decode_request_t *req);

/* Utility */
jpeg_view_intent_t   jpeg_view_default(uint16_t lcd_width, uint16_t lcd_height);
const char          *jpeg_decoder_err_to_str(jpeg_decode_result_t result);
```

---

## What Is Removed

| Removed | Reason |
|---|---|
| `jpeg_source_t` | Replaced by `jpeg_reader_t` (cb + ctx, nothing else) |
| `jpeg_view_t` | Replaced by `jpeg_view_intent_t` |
| `jpeg_decoder_source_from_file()` | Caller writes a 5-line file callback |
| `jpeg_decoder_source_from_buffer()` | Caller writes a 7-line buffer callback |
| `jpeg_roi_decoder_helpers.c` | File deleted entirely |
| `seek()` / `reset()` in source | Caller resets their own ctx; component has no reset |
| `jpeg_decoder_prepare_view_request()` | Logic absorbed into `core_run` |
| `_buf` embedded state in source struct | No longer needed; caller owns all source state |
| `input_buffer` / `input_buffer_size` in config | Component introduces no intermediate buffer; TJpgDec's own buf is passed directly to callback |

---

## File Structure

```
jpeg_roi_decoder.h              ← all public types and API declarations
jpeg_roi_decoder_core.c         ← core_run, output_func, input_func, ROI math
jpeg_roi_decoder_rtos.c         ← FreeRTOS queue/task adapter
jpeg_decoder_internal.h         ← internal types shared across .c files

jpeg_roi_decoder_helpers.c      ← DELETED
```

---

## Implementation Checklist

- [ ] `jpeg_read_cb_t` typedef: `size_t (*)(uint8_t *dst, size_t max, void *ctx)`
- [ ] `jpeg_reader_t` struct: `{ jpeg_read_cb_t cb; void *ctx; }` — no buffer, no state
- [ ] `jpeg_view_intent_t` defined with `reader` and `chunk_buffer` fields; `jpeg_view_t` removed
- [ ] `jpeg_decode_request_t` updated: old `source` field replaced by `reader`
- [ ] `decode_context_t` updated: all former stream fields replaced by single `reader` field
- [ ] `jpeg_view_default()` returns `jpeg_view_intent_t` with `reader = {NULL, NULL}`, `chunk_buffer = NULL`
- [ ] `jpeg_roi_decoder_helpers.c` deleted; no built-in backends remain in the component
- [ ] `input_func` passes TJpgDec's `buf` pointer directly to `reader.cb` — zero copy
- [ ] `input_func` passes `NULL` through to `reader.cb` on skip requests — enables fseek fast-path in callbacks
- [ ] `input_func` retry loop handles partial returns correctly; no internal staging buffer
- [ ] `jpeg_decoder_probe()` signature: `(jpeg_reader_t, jpeg_image_info_t*, void*, size_t)` — 4 parameters
- [ ] `jpeg_decoder_probe()` header comment documents caller responsibility to reset their own source
- [ ] `core_run` validates `reader.cb != NULL` and `chunk_buffer != NULL` before proceeding
- [ ] `core_run` runs prepare → resolve AUTO scale → compute ROI → validate → decomp (single forward pass)
- [ ] `core_run` skips intent resolution in the low-level path (ROI pre-supplied, scale != AUTO)
- [ ] RTOS `decode_job_t` carries `jpeg_reader_t reader`; no stream fields
- [ ] RTOS worker task owns full lifecycle (prepare + decomp); no bytes consumed in caller task
- [ ] `jpeg_decoder_decode_view()` copies `intent->reader` into the queued job
- [ ] `JPEG_SCALE_AUTO` rejected in `jpeg_decoder_decode()` with `JPEG_DECODE_ERR_PARAM`
- [ ] Work buffer, chunk buffer, and `reader.ctx` lifetime documented in header comments at each public entry point
- [ ] All debug log statements from original `output_func` retained or intentionally removed
- [ ] Migration note in README: callers supply `jpeg_reader_t`; file/buffer/queue example callbacks provided as comments, not shipped code