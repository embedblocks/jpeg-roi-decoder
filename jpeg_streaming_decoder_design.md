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
   trivial to implement by the caller with three to ten lines of code each.
3. The prepare/decomp split is used correctly: both phases run in a single forward pass.
   Scale and ROI are resolved between them using `jd.width`/`jd.height`.
4. Both the **input buffer** (for incoming JPEG bytes) and the **output chunk buffer**
   (for decoded pixels) are **caller-allocated** and passed in via the config struct.
   The component owns no heap allocations.
5. All existing decode capability (ROI, pan, scale, chunk callbacks, RTOS async) is preserved.

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
 *   buf      — pointer to the input_buffer supplied in the decode config.
 *              The callback must write JPEG source bytes into this buffer.
 *   max_bytes — maximum bytes the callback may write (== input_buffer_size
 *               from the config struct).
 *   ctx      — caller-defined context (file pointer, buffer state struct,
 *              HTTP handle, queue handle, etc.). Passed through unchanged
 *              from the read_ctx field of the config struct.
 *
 * Return value:
 *   Number of bytes actually written into buf.
 *   Return 0 on end-of-data, timeout, or unrecoverable error.
 *   Partial returns (< max_bytes) are valid. The decoder retries internally.
 *
 * Contract:
 *   - Must not block indefinitely. Implement a timeout; return 0 on expiry.
 *   - The decoder has no watchdog. A permanently stalled callback stalls the decoder.
 *   - For seekable sources (file, buffer), the caller is responsible for
 *     resetting ctx to the beginning before a second decode of the same data.
 *     The component provides no reset mechanism.
 */
typedef size_t (*jpeg_read_cb_t)(uint8_t *buf, size_t max_bytes, void *ctx);
```

### Why the Buffer Comes From the Config

`buf` is always the `input_buffer` pointer from the decode config struct. This is deliberate:

- The caller controls buffer placement (IRAM, PSRAM, DMA-capable region, etc.).
- No hidden internal buffers exist inside the decoder.
- The callback signature is stable and identical regardless of source type.
- On constrained targets the caller sizes the input buffer to match read granularity
  (e.g. SD card sector size, network packet MTU, UART DMA block size).

---

## Input Buffer: Caller-Allocated, Passed in Config

Every decode config struct (high-level and low-level) gains two fields:

```c
uint8_t *input_buffer;       /* caller-allocated; passed to read_cb as buf      */
size_t   input_buffer_size;  /* max_bytes for read_cb; must be >= 512            */
```

### Sizing Guidance

| Source type | Recommended `input_buffer_size` |
|---|---|
| SD card (file) | 512 – 4096 (sector-aligned) |
| Embedded buffer (const array) | 512 – 1024 (header fits in one call) |
| HTTP / TCP socket | MTU or chunk size (typically 1460 or 4096) |
| UART / serial | One DMA block (device-specific) |
| FreeRTOS queue of byte arrays | One item payload size |

**Minimum:** 512 bytes. TJpgDec's header parse requests up to ~4 KB in small chunks;
smaller buffers work but increase callback invocation count.

**Proportionality with chunk_buffer:** The input buffer feeds the JPEG decoder;
the chunk buffer receives decoded pixels. They operate independently and need not
be the same size. A typical pairing on ESP32:

```
input_buffer_size  = 4096   /* ~one SD sector or one HTTP chunk */
chunk_buffer_size  = JPEG_CHUNK_BUF_BYTES(320) = 320 * 16 * 2 = 10240 bytes
```

---

## Removed: `jpeg_source_t` and All Built-In Backends

`jpeg_source_t`, `jpeg_decoder_source_from_file()`, `jpeg_decoder_source_from_buffer()`,
and `jpeg_roi_decoder_helpers.c` are **removed entirely**.

### Migration — What Callers Write Instead

The following examples are for illustration in the migration guide. They are **not** part
of the component. Each is three to seven lines.

#### File source

```c
typedef struct { FILE *fp; } file_ctx_t;

static size_t file_read_cb(uint8_t *buf, size_t max_bytes, void *ctx)
{
    file_ctx_t *fc = ctx;
    return fread(buf, 1, max_bytes, fc->fp);
}
```

#### In-memory buffer source

```c
typedef struct { const uint8_t *data; size_t len; size_t pos; } buf_ctx_t;

static size_t buf_read_cb(uint8_t *buf, size_t max_bytes, void *ctx)
{
    buf_ctx_t *bc = ctx;
    size_t avail = bc->len - bc->pos;
    size_t n     = max_bytes < avail ? max_bytes : avail;
    memcpy(buf, bc->data + bc->pos, n);
    bc->pos += n;
    return n;
}
```

#### FreeRTOS queue source (true streaming)

```c
typedef struct { QueueHandle_t q; TickType_t timeout; } queue_ctx_t;

static size_t queue_read_cb(uint8_t *buf, size_t max_bytes, void *ctx)
{
    queue_ctx_t *qc  = ctx;
    uint8_t      byte;
    size_t       n   = 0;
    while (n < max_bytes) {
        if (xQueueReceive(qc->q, &byte, qc->timeout) != pdTRUE) break;
        buf[n++] = byte;
    }
    return n;   /* 0 on timeout = decoder sees EOF → JPEG_DECODE_ERR_INPUT */
}
```

---

## Updated Config Structs

### High-Level: `jpeg_view_intent_t` (replaces `jpeg_view_t`)

```c
typedef struct {
    uint16_t lcd_width;
    uint16_t lcd_height;
    int32_t  pan_x;                 /* pixels from center, clamped automatically */
    int32_t  pan_y;
    jpeg_decode_scale_t  scale;     /* JPEG_SCALE_AUTO is valid here */
    jpeg_output_format_t out_format;

    /* Source — provided by caller, no built-in backends */
    jpeg_read_cb_t  read_cb;        /* mandatory */
    void           *read_ctx;       /* passed to read_cb unchanged */
    uint8_t        *input_buffer;   /* caller-allocated; min 512 bytes */
    size_t          input_buffer_size;

    /* Output — caller-allocated; see JPEG_CHUNK_BUF_BYTES() */
    uint16_t       *chunk_buffer;
} jpeg_view_intent_t;
```

`jpeg_view_default()` is updated to return `jpeg_view_intent_t` with `read_cb = NULL`,
`read_ctx = NULL`, `input_buffer = NULL`, `input_buffer_size = 0`, `chunk_buffer = NULL`.
Caller must fill all pointer fields before decoding.

### Low-Level: `jpeg_decode_request_t` (updated)

```c
typedef struct {
    /* Source */
    jpeg_read_cb_t       read_cb;              /* mandatory */
    void                *read_ctx;             /* passed to read_cb unchanged */
    uint8_t             *input_buffer;         /* caller-allocated; min 512 bytes */
    size_t               input_buffer_size;

    /* Decode parameters */
    jpeg_roi_t           roi;                  /* in original unscaled JPEG coords */
    jpeg_decode_scale_t  scale;                /* JPEG_SCALE_AUTO not valid here */
    jpeg_output_format_t out_format;

    /* Buffers */
    void    *work_buffer;
    size_t   work_buffer_size;
    uint16_t *chunk_buffer;
    size_t    chunk_buffer_pixels;

    /* Callbacks */
    jpeg_chunk_cb_t chunk_callback;
    jpeg_done_cb_t  done_callback;
    void           *user_data;
} jpeg_decode_request_t;
```

**Removed fields:** `source` (was `jpeg_source_t`).  
**JPEG_SCALE_AUTO** is rejected in this path (`JPEG_DECODE_ERR_PARAM`).

---

## Updated Internal Type: `decode_context_t`

The `stream` field is replaced with the three source fields:

```c
typedef struct {
    jpeg_read_cb_t       read_cb;
    void                *read_ctx;
    uint8_t             *input_buffer;
    size_t               input_buffer_size;

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

## Updated `input_func` — Reads Via Callback Into Caller's Buffer

TJpgDec calls `input_func(jd, buf, nbyte)` where `buf` is TJpgDec's internal buffer
and `nbyte` is the amount it wants. The internal `input_func` bridges this to the
user's callback using the caller-supplied `input_buffer` as the intermediate staging area.

```c
static size_t input_func(JDEC *jd, uint8_t *buf, size_t nbyte)
{
    decode_context_t *ctx = (decode_context_t *)jd->device;

    if (!buf) {
        /* Skip/drain path — discard nbyte bytes from the source */
        size_t remaining = nbyte;
        while (remaining > 0) {
            size_t want = remaining < ctx->input_buffer_size
                        ? remaining : ctx->input_buffer_size;
            size_t got  = ctx->read_cb(ctx->input_buffer, want, ctx->read_ctx);
            if (got == 0) break;
            remaining -= got;
        }
        return nbyte - remaining;
    }

    /* Normal read path — retry until nbyte satisfied or callback returns 0 */
    size_t total = 0;
    while (total < nbyte) {
        size_t want = (nbyte - total) < ctx->input_buffer_size
                    ? (nbyte - total) : ctx->input_buffer_size;

        size_t got = ctx->read_cb(ctx->input_buffer, want, ctx->read_ctx);
        if (got == 0) break;   /* source ended or timed out */

        memcpy(buf + total, ctx->input_buffer, got);
        total += got;
    }
    return total;
}
```

> **Note on the extra `memcpy`:** TJpgDec owns `buf`; the caller owns `input_buffer`.
> They are different memory regions. The `memcpy` is unavoidable without modifying TJpgDec
> to accept an external buffer pointer. On ESP32, this copy is negligible compared to
> SD card or network I/O latency.

---

## Core Decode Flow (Single Forward Pass — Unchanged from Previous Draft)

```
jpeg_decoder_core_run()
    │
    ├─ input_func registered with tjpgd (calls read_cb into input_buffer, copies to jd buf)
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
            └─ stream cursor is already past headers (no rewind)
               output_func fires chunk callbacks row by row
```

`jpeg_decoder_prepare_view_request()` is **removed**. Its logic is absorbed into `core_run`.

---

## `jpeg_decoder_probe()` — Retained, Caller Manages Reset

Probe is retained as a caller utility. Because the component ships no built-in backends
and no `reset` mechanism, the caller is responsible for returning their source to the
beginning before issuing a decode after a probe.

```c
/**
 * Parse JPEG headers and return image dimensions.
 *
 * Consumes the beginning of the JPEG byte stream up through the header markers.
 * After probe returns, the caller must reset their source (e.g. fseek to 0,
 * reset their buffer position index, etc.) before passing the same source to
 * a decode call. The component has no reset mechanism; this is the caller's
 * responsibility.
 *
 * For true non-seekable streams (HTTP, UART, queue), probe is not useful —
 * the stream cannot be rewound. Use the decode path directly; image dimensions
 * are available in the done_callback via done_evt->image.
 */
jpeg_decode_result_t jpeg_decoder_probe(
    jpeg_read_cb_t     read_cb,
    void              *read_ctx,
    uint8_t           *input_buffer,
    size_t             input_buffer_size,
    jpeg_image_info_t *info_out,
    void              *work_buffer,
    size_t             work_buffer_size
);
```

Example caller pattern for seekable sources:

```c
/* After probe, reset source manually, then decode */
jpeg_decoder_probe(file_read_cb, &fc, ibuf, sizeof(ibuf), &info, workbuf, sizeof(workbuf));
fseek(fc.fp, 0, SEEK_SET);   /* caller resets — component does not */
jpeg_decoder_decode_view(&intent, workbuf, sizeof(workbuf), on_chunk, on_done, NULL);
```

---

## Stalled Callback Behavior — Explicit Contract

The decoder has **no internal abort or watchdog mechanism**.

If `read_cb` blocks, the decoder task blocks. The blocking contract belongs entirely in
the callback implementation.

**Required behavior for any non-trivial callback:**
- Implement a read timeout internally (e.g. `xQueueReceive` with a finite `timeout`).
- Return 0 when the timeout expires.
- The decoder will then return `JPEG_DECODE_ERR_INPUT` via `done_callback`.

Timeout policy (aggressive vs. lenient) is decided by the callback implementor, not
the decoder.

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
- Caller must not interpret `JPEG_DECODE_OK` from these functions as decode success.

---

## RTOS Layer: Worker Task Owns Full Lifecycle

**No bytes are consumed in the caller task.** Prepare and decomp both run in the worker.

The caller queues a job struct containing the callback, context, buffer pointers, and intent:

```c
typedef struct {
    /* Source */
    jpeg_read_cb_t       read_cb;
    void                *read_ctx;
    uint8_t             *input_buffer;
    size_t               input_buffer_size;

    /* Decode intent or raw ROI */
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

Consequence: probe errors (corrupt JPEG headers, truncated stream) no longer
surface synchronously. They arrive via `done_callback` with `result = JPEG_DECODE_ERR_INPUT`.

---

## Buffer Lifetime Contracts

### Work buffer

- Caller allocates (minimum `JPEG_DECODER_WORK_BUF_MIN` bytes).
- Must remain valid and unmodified until `done_callback` fires.
- In the async RTOS path: do not free or reuse until the callback fires.
- Declaring as a static array is the simplest correct choice for most embedded use cases.

### Input buffer

- Caller allocates. Passed to `read_cb` as `buf` on every invocation.
- Must remain valid until `done_callback` fires.
- Must not be written to by any other task while a decode is in progress.

### Chunk buffer

- Caller allocates. Size: `JPEG_CHUNK_BUF_BYTES(lcd_width)`.
- Must remain valid until `done_callback` fires.
- Must not be read by the caller during decode (its contents are incomplete mid-row).

### Source context (`read_ctx`)

- Caller owns entirely. The component stores the pointer; it never dereferences it
  except by passing it to `read_cb`.
- For file sources: the FILE* must remain open until `done_callback` fires.
- For buffer sources: the data buffer must remain valid until `done_callback` fires.

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

/* Probe — caller must reset their source after calling this */
jpeg_decode_result_t jpeg_decoder_probe(
    jpeg_read_cb_t     read_cb,
    void              *read_ctx,
    uint8_t           *input_buffer,
    size_t             input_buffer_size,
    jpeg_image_info_t *info_out,
    void              *work_buffer,
    size_t             work_buffer_size
);

/* High-level API — scale and ROI resolved from intent after header parse */
jpeg_decode_result_t jpeg_decoder_decode_view(
    const jpeg_view_intent_t *intent,    /* contains read_cb, read_ctx, buffers */
    void                     *work_buffer,
    size_t                    work_buffer_size,
    jpeg_chunk_cb_t           chunk_callback,
    jpeg_done_cb_t            done_callback,
    void                     *user_data
);

/* Low-level API — caller pre-computes ROI, JPEG_SCALE_AUTO rejected */
jpeg_decode_result_t jpeg_decoder_decode(const jpeg_decode_request_t *req);

/* Utility */
jpeg_view_intent_t   jpeg_view_default(uint16_t lcd_width, uint16_t lcd_height);
const char          *jpeg_decoder_err_to_str(jpeg_decode_result_t result);
```

---

## What Is Removed

| Removed | Reason |
|---|---|
| `jpeg_source_t` | Replaced by `jpeg_read_cb_t` + `read_ctx` fields |
| `jpeg_view_t` | Replaced by `jpeg_view_intent_t` |
| `jpeg_decoder_source_from_file()` | Caller writes 4-line file callback |
| `jpeg_decoder_source_from_buffer()` | Caller writes 6-line buffer callback |
| `jpeg_roi_decoder_helpers.c` | File removed entirely |
| `seek()` / `reset()` in source struct | Component has no reset mechanism; caller resets their own context |
| `jpeg_decoder_prepare_view_request()` | Logic absorbed into `core_run` |
| `_buf` embedded state in source struct | No longer needed; caller owns source state |

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

- [ ] `jpeg_read_cb_t` typedef defined in public header
- [ ] `jpeg_view_intent_t` defined with `read_cb`, `read_ctx`, `input_buffer`, `input_buffer_size`, `chunk_buffer` fields; `jpeg_view_t` removed
- [ ] `jpeg_decode_request_t` updated: `source` field replaced by `read_cb`, `read_ctx`, `input_buffer`, `input_buffer_size`
- [ ] `decode_context_t` updated: `stream` field replaced by `read_cb`, `read_ctx`, `input_buffer`, `input_buffer_size`
- [ ] `jpeg_view_default()` returns `jpeg_view_intent_t` with all pointer fields NULL
- [ ] `jpeg_roi_decoder_helpers.c` deleted; no built-in backends remain in the component
- [ ] `jpeg_decoder_probe()` updated to accept flat `read_cb`, `read_ctx`, `input_buffer`, `input_buffer_size` parameters
- [ ] `jpeg_decoder_probe()` header comment documents that caller must reset their own source after calling it
- [ ] `input_func` calls `ctx->read_cb(ctx->input_buffer, want, ctx->read_ctx)` and memcpy result into TJpgDec's `buf`
- [ ] `input_func` skip/drain path calls `read_cb` in a loop, discarding into `input_buffer`
- [ ] `input_func` retry loop handles partial reads (returns < want) correctly
- [ ] `core_run` validates `read_cb != NULL`, `input_buffer != NULL`, `input_buffer_size >= 1` before proceeding
- [ ] `core_run` runs prepare → resolve AUTO scale → compute ROI → validate → decomp (single forward pass)
- [ ] `core_run` skips intent resolution when called from low-level path (ROI pre-supplied, scale != AUTO)
- [ ] RTOS `decode_job_t` carries `read_cb`, `read_ctx`, `input_buffer`, `input_buffer_size` instead of `stream`
- [ ] RTOS worker task owns full lifecycle (prepare + decomp); no bytes consumed in caller task
- [ ] `jpeg_decoder_decode_view()` passes `intent->read_cb/read_ctx/input_buffer/input_buffer_size` into the queued job
- [ ] `JPEG_SCALE_AUTO` rejected in `jpeg_decoder_decode()` (low-level path) with `JPEG_DECODE_ERR_PARAM`
- [ ] Work buffer lifetime documented at each async API entry point in header comments
- [ ] Input buffer and read_ctx lifetime documented in header
- [ ] All debug log statements from original `output_func` retained or intentionally removed
- [ ] Migration note added to README or component docs: callers must now supply `read_cb`; example file and buffer callbacks provided as comments or examples (not as shipped code)