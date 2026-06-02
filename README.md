# jpeg_roi_decoder

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x-blue)
![Espressif Component Registry](https://img.shields.io/badge/Espressif-Component%20Registry-orange)
![License](https://img.shields.io/badge/license-MIT-green)

Streaming region-of-interest JPEG decoder for ESP32 based on tjpgd. Stream JPEG data in from any source,
stream decoded pixel rows out to your callback — without loading the full image into RAM,
without a seekable source, without a full-frame output buffer.

Stream in from a file, a flash blob, an HTTP response body, a UART byte stream, a FreeRTOS
queue, a TCP socket, or a DMA ring buffer. If you can hand bytes to a callback, you can decode a JPEG.

---

## Features

* **Stream in from anywhere** — one read callback, any backend. File, buffer, HTTP, UART, queue, socket, DMA — all work identically
* **Stream out row-by-row** — no full-frame buffer needed, decoded pixel rows stream directly to your callback
* **Any rectangular region** — decode a tile, a thumbnail strip, or the full image
* **LCD-aware high-level API** — give it your display size, it handles scale, centering, and pan
* **Automatic scale selection** — `JPEG_SCALE_AUTO` picks the best 1/1, 1/2, 1/4, or 1/8 fit
* **RGB565 and RGB888 output**
* **Zero heap allocation** — work buffer, chunk buffer, and input buffer are all caller-supplied
* **Optional input prefetch buffer** — batches the decoder's many small internal reads into fewer large chunk-sized calls, reducing per-call overhead for sources where each callback invocation is expensive (HTTP, socket, UART, queue)
* **Two threading models** — synchronous blocking call (caller owns the thread) or FreeRTOS async (component owns a worker task); selected at build time, same API either way

---

## Chip Support

| Chip | Status |
|---|---|
| ESP32 | Tested |
| ESP32-S3 | Expected to work |
| ESP32-S2 | Expected to work |
| ESP32-C3 | Expected to work |

---

## Installation

```bash
idf.py add-dependency "jpeg_roi_decoder^0.5.2"
```

Or in `idf_component.yml`:

```yaml
dependencies:
  jpeg_roi_decoder: "^0.5.2"
```

---

## Threading Model

The component ships two platform adapters. Select one at build time via CMake — the public API is identical either way.

### Synchronous (default)

`jpeg_decoder_decode_view()` and `jpeg_decoder_decode()` block until the full image is decoded. The caller's task drives the decode loop. `on_done` fires before the call returns.

```
while (1) {
    http_open(url);
    jpeg_decoder_decode_view(...);   // blocks — on_chunk fires per row, on_done fires at end
    http_close();                    // safe — decode is fully complete
}
```

This is the right choice when the calling task has one job: fetch and display. No extra stack, no queue, no synchronization primitives needed.

### Asynchronous (FreeRTOS)

`jpeg_decoder_decode_view()` and `jpeg_decoder_decode()` enqueue the job and return immediately. Decode runs in a dedicated worker task. `on_done` fires from the worker task when complete.

```
jpeg_decoder_decode_view(...);   // returns immediately — job is queued
ulTaskNotifyTake(...);           // caller waits for on_done to signal completion
```

Use this when the calling task must stay responsive while decode runs — for example, handling touch input or driving a UI alongside decoding.

### Selecting the adapter

In your project `CMakeLists.txt` before adding the component:

```cmake
# default — no entry needed
set(JPEG_DECODER_THREADING "sync")

# or, to use the internal FreeRTOS worker task
set(JPEG_DECODER_THREADING "async")
```

`jpeg_decoder_init()` and `jpeg_decoder_deinit()` are no-ops on the sync path. On the async path, `jpeg_decoder_init()` creates the queue and worker task and must be called before any decode.

---

## How It Works

The decoder runs a single forward pass through the JPEG byte stream:

```
your callback feeds bytes
        |
   header parse          <- image dimensions become known here
        |
   scale resolution      <- JPEG_SCALE_AUTO picks best fit
        |
   ROI computation       <- centered, panned, clamped
        |
   pixel decode          <- your on_chunk() fires once per row
        |
   on_done()
```

The source never rewinds. This means sources that cannot seek — HTTP, UART, sockets,
queues — work just as well as files and buffers.

---

## Source Abstraction — jpeg_reader_t

The complete input contract is one function signature:

```c
typedef size_t (*jpeg_read_cb_t)(uint8_t *dst, size_t max, void *ctx);
```

- Write `max` bytes into `dst` and return how many you wrote.
- If `dst` is `NULL`, the decoder wants to skip `max` bytes forward — seek if you can, discard if you can't.
- Return `0` to signal end-of-data or a timeout. The decoder will report `JPEG_DECODE_ERR_INPUT`.
- Partial returns (less than `max`) are fine — the decoder retries internally.

Pair the callback with your context:

```c
typedef struct {
    jpeg_read_cb_t  cb;
    void           *ctx;
} jpeg_reader_t;
```

The component stores the pointer and calls it. All source state lives in `ctx` — the component never touches it directly.

### Example callbacks

#### FILE* — SD card, SPIFFS, LittleFS

```c
size_t file_read_cb(uint8_t *dst, size_t max, void *ctx)
{
    FILE *fp = ctx;
    if (dst == NULL)
        return fseek(fp, (long)max, SEEK_CUR) == 0 ? max : 0;
    return fread(dst, 1, max, fp);
}
```

#### Flash blob or any buffer in RAM / PSRAM

```c
typedef struct { const uint8_t *data; size_t len; size_t pos; } buf_ctx_t;

size_t buf_read_cb(uint8_t *dst, size_t max, void *vctx)
{
    buf_ctx_t *bc = vctx;
    size_t n = max < (bc->len - bc->pos) ? max : (bc->len - bc->pos);
    if (dst) memcpy(dst, bc->data + bc->pos, n);
    bc->pos += n;
    return n;
}
```

#### FreeRTOS queue — live byte stream

```c
typedef struct { QueueHandle_t q; TickType_t timeout; } queue_ctx_t;

size_t queue_read_cb(uint8_t *dst, size_t max, void *vctx)
{
    queue_ctx_t *qc = vctx;
    size_t n = 0;
    while (n < max) {
        uint8_t byte;
        if (xQueueReceive(qc->q, &byte, qc->timeout) != pdTRUE)
            break;
        if (dst) dst[n] = byte;
        n++;
    }
    return n;
}
```

HTTP, TCP, UART, DMA ring buffer — same pattern every time. Write your callback once, plug it in.

---

## Input Prefetch Buffer (optional)

### The problem

TJpgDec parses JPEG headers with many small internal reads — 1, 4, 14, 65 bytes at a time.
For in-memory and file sources this costs nothing. For sources where each `reader.cb`
invocation has overhead, it adds up:

| Source | Per-call cost |
|---|---|
| RAM / flash buffer | `memcpy` — negligible |
| `FILE*` on SD / SPIFFS | filesystem layer — small |
| HTTP / HTTPS | `esp_http_client_read` → TLS → `recv()` syscall |
| TCP socket | `recv()` syscall + kernel buffer management |
| UART | `uart_read_bytes` + driver overhead |
| FreeRTOS queue | `xQueueReceive` + scheduler check per byte |

During header parsing alone the decoder makes 15–25 `reader.cb` calls. On an HTTP source
each of those is a round-trip through the TLS stack. Batching them into one or two
chunk-sized calls reduces that overhead significantly and keeps header parse time short.

### The solution

Set `view.input_buffer` to a caller-allocated byte array. The component then reads from
your source exclusively in `JPEG_INPUT_BUF_SIZE`-byte chunks, serving TJpgDec's small
internal requests from that buffer. Your callback sees far fewer, larger calls.

```c
static uint8_t input_buf[JPEG_INPUT_BUF_SIZE];   // 2048 bytes, lives in .bss

view.input_buffer = input_buf;
```

Setting `input_buffer = NULL` (the default from `jpeg_view_default`) restores the original
zero-copy direct path — correct and efficient for file and buffer sources.

### When to use it

| Source type | `input_buffer` | Reason |
|---|---|---|
| Flash blob / RAM buffer | `NULL` | Per-call cost is negligible |
| `FILE*` on SD / SPIFFS | `NULL` | Filesystem handles its own buffering |
| HTTP / HTTPS | **Set** | Each call traverses TLS stack + syscall |
| TCP socket | **Set** | Each call is a `recv()` syscall |
| UART | **Set** | Driver overhead per call |
| FreeRTOS queue | **Set** | Scheduler overhead per dequeue |

### Sizing

`JPEG_INPUT_BUF_SIZE` defaults to 2048 bytes, which covers most JFIF and Exif headers in a
single refill. Override per-project via CMake:

```cmake
target_compile_definitions(${COMPONENT_TARGET} PRIVATE JPEG_INPUT_BUF_SIZE=4096)
```

### Skip handling

When TJpgDec skips over unknown JPEG markers it calls `input_func` with `dst = NULL`.
In the buffered path the component drains and discards bytes from the prefetch buffer,
refilling from your callback as needed. Your callback always receives a real destination
pointer — it never needs to handle `dst = NULL` for non-seekable sources.

---

## Buffer Sizing Macros

```c
/* Output chunk buffer — one MCU row band */
#define JPEG_MCU_MAX_HEIGHT        16u
#define JPEG_CHUNK_BUF_PIXELS(w)   ((w) * JPEG_MCU_MAX_HEIGHT)
#define JPEG_CHUNK_BUF_BYTES(w)    (JPEG_CHUNK_BUF_PIXELS(w) * sizeof(uint16_t))

/* Input prefetch buffer — optional, for sources with per-call overhead */
#ifndef JPEG_INPUT_BUF_SIZE
#define JPEG_INPUT_BUF_SIZE        2048u
#endif

/* Work buffer — TJpgDec internal scratch */
#define JPEG_DECODER_WORK_BUF_MIN      3096U
#define JPEG_DECODER_WORK_BUF_DEFAULT  4096U
```

---

## Usage

### High-level API — file or buffer source

```c
#include "jpeg_roi_decoder.h"

static uint8_t  workbuf[JPEG_DECODER_WORK_BUF_DEFAULT];
static uint16_t chunk_buf[JPEG_CHUNK_BUF_PIXELS(320)];

static bool on_chunk(const jpeg_chunk_event_t *evt)
{
    my_lcd_write_row(evt->y, evt->pixels, evt->byte_count);
    return true;   /* return false to abort */
}

static void on_done(const jpeg_done_event_t *evt)
{
    if (evt->result != JPEG_DECODE_OK)
        ESP_LOGE(TAG, "%s", jpeg_decoder_err_to_str(evt->result));
}

void display_jpeg_from_sdcard(FILE *fp)
{
    jpeg_view_intent_t view = jpeg_view_default(320, 240);
    view.reader       = (jpeg_reader_t){ .cb = file_read_cb, .ctx = fp };
    view.chunk_buffer = chunk_buf;
    view.scale        = JPEG_SCALE_AUTO;
    /* input_buffer left NULL — file sources handle buffering themselves */

    jpeg_decoder_decode_view(
        &view,
        workbuf, sizeof(workbuf),
        on_chunk, on_done, NULL
    );
}
```

### High-level API — HTTP or high-overhead source

Add one buffer declaration and one assignment. Everything else is identical.

```c
static uint8_t  workbuf[JPEG_DECODER_WORK_BUF_DEFAULT];
static uint16_t chunk_buf[JPEG_CHUNK_BUF_PIXELS(320)];
static uint8_t  input_buf[JPEG_INPUT_BUF_SIZE];        /* reduces TLS stack overhead */

void display_jpeg_from_http(http_stream_ctx_t *http_ctx)
{
    jpeg_view_intent_t view = jpeg_view_default(320, 240);
    view.reader       = (jpeg_reader_t){ .cb = http_read_cb, .ctx = http_ctx };
    view.chunk_buffer = chunk_buf;
    view.input_buffer = input_buf;   /* batch small reads into chunk-sized calls */
    view.scale        = JPEG_SCALE_AUTO;

    jpeg_decoder_decode_view(
        &view,
        workbuf, sizeof(workbuf),
        on_chunk, on_done, NULL
    );
}
```

### Using a flash-embedded image

```c
extern const uint8_t img_start[] asm("_binary_splash_jpg_start");
extern const uint8_t img_end[]   asm("_binary_splash_jpg_end");

static buf_ctx_t img_ctx;

void display_splash(void)
{
    img_ctx = (buf_ctx_t){
        .data = img_start,
        .len  = img_end - img_start,
        .pos  = 0,
    };

    jpeg_view_intent_t view = jpeg_view_default(320, 240);
    view.reader       = (jpeg_reader_t){ .cb = buf_read_cb, .ctx = &img_ctx };
    view.chunk_buffer = chunk_buf;

    jpeg_decoder_decode_view(&view, workbuf, sizeof(workbuf), on_chunk, on_done, NULL);
}
```

### Low-level API

Pre-compute your own ROI in original unscaled JPEG coordinates. `JPEG_SCALE_AUTO` is not valid here.

```c
jpeg_decode_request_t req = {
    .reader              = { .cb = file_read_cb, .ctx = fp },
    .roi                 = { .left = 0, .top = 0, .right = 1919, .bottom = 1079 },
    .scale               = JPEG_SCALE_1_4,
    .out_format          = JPEG_OUTPUT_RGB565,
    .work_buffer         = workbuf,
    .work_buffer_size    = sizeof(workbuf),
    .chunk_buffer        = chunk_buf,
    .chunk_buffer_pixels = JPEG_CHUNK_BUF_PIXELS(480),
    .input_buffer        = NULL,   /* set to input_buf for HTTP/socket sources */
    .chunk_callback      = on_chunk,
    .done_callback       = on_done,
};
jpeg_decoder_decode(&req);
```

---

## Examples

| Example | Source | Output | Threading | Notes |
|---|---|---|---|---|
| `examples/async/uart` | Flash blob | UART | sync | Embedded JPEG streamed to PC receiver script |
| `examples/async/https` | HTTPS | LCD | async | Same as above with FreeRTOS worker task |
| `examples/async/sdcard` | SD card | SD card | sync | RGB565 output written back to SD card |
| `examples/async/lcd` | HTTPS | ILI9486 LCD | async | Same with FreeRTOS async notification |
| `examples/async/ipcam` | HTTP IPCAM | ILI9486 LCD | async | Same with FreeRTOS worker task |
| `examples/sync/https` | HTTPS | LCD | sync | Single-pass HTTPS decode with `input_buffer` |
| `examples/sync/lcd` | HTTPS | ILI9486 LCD | sync | Pan control, `input_buffer`, correct HTTP lifetime |
| `examples/sync/ipcam` | HTTP IPCAM | ILI9486 LCD | sync | Continuous frame loop, `swap_xy`, DMA semaphore |


---

## Buffer Lifetime

### Synchronous path

All buffers are only needed for the duration of the blocking call. Since `jpeg_decoder_decode_view()` does not return until `on_done` has fired, stack-allocated buffers are safe as long as the call is on the stack. `static` is still the simplest choice on embedded targets.

Resource cleanup belongs in the caller, after the call returns — not in `on_done`:

```c
/* correct — decode is fully complete when the call returns */
jpeg_decoder_decode_view(...);
http_close();

/* also correct — on_done fires before the call returns, cleanup there is fine too */
static void on_done(const jpeg_done_event_t *evt) {
    http_close();
}
```

### Asynchronous path

`jpeg_decoder_decode_view()` returns immediately — the worker task decodes in the background. Four things must remain valid until `done_callback` fires:

| | Must stay valid until |
|---|---|
| `work_buffer` | `done_callback` |
| `chunk_buffer` | `done_callback` |
| `input_buffer` (if set) | `done_callback` |
| `reader.ctx` and everything it points to | `done_callback` |

Declaring all as `static` is the simplest correct choice.

**If your source owns a connection** (FILE*, HTTP client, socket): do not close it after
`decode_view` returns. The worker task is still calling your callback. Close the resource
inside `on_done` — that is the correct and only safe point.

```c
/* correct — resource closed from on_done */
static void on_done(const jpeg_done_event_t *evt) {
    http_close();
}

/* crash — worker task still reading when main task closes the connection */
jpeg_decoder_decode_view(...);   /* returns immediately on async path */
http_close();                    /* races with worker task */
```

---

## Return Values

### Synchronous path

The return value is the decode result directly:

| Value | Meaning |
|---|---|
| `JPEG_DECODE_OK` | Decode succeeded |
| `JPEG_DECODE_ERR_PARAM` | Bad argument or invalid ROI |
| `JPEG_DECODE_ERR_INPUT` | Source read failed or returned 0 too early |
| `JPEG_DECODE_ERR_MEM` | Work buffer too small |
| `JPEG_DECODE_ERR_FMT` | Unsupported JPEG format |
| `JPEG_DECODE_ERR_INTR` | TJpgDec internal error |
| `JPEG_DECODE_ABORTED` | `chunk_callback` returned false |

### Asynchronous path

The return value reflects queuing only — not decode outcome:

| Value | Meaning |
|---|---|
| `JPEG_DECODE_OK` | Request accepted — worker will decode |
| `JPEG_DECODE_ERR_PARAM` | Bad argument — not queued |
| `JPEG_DECODE_ERR_INTR` | Queue full — not queued |

All decode results — including header parse failures and mid-stream errors — arrive via `done_callback.result`.

---

## Notes

**`input_buffer` is optional and source-dependent.** File, buffer, and PSRAM sources should
leave it `NULL` — the zero-copy direct path is faster for local memory. Set it for sources
where each `reader.cb` call carries non-trivial overhead: HTTP, sockets, UART, queues.

**Scale is discrete.** TJpgDec supports 1/1, 1/2, 1/4, 1/8 only. `JPEG_SCALE_AUTO` picks
the smallest factor where the scaled image is at least as large as the LCD. Use a fixed value
if you need predictable output dimensions.

**Pan is in LCD pixels.** `pan_x` / `pan_y` shift the viewport in output pixel units,
independent of scale. `(0, 0)` centers the image. Values that push outside the image are
clamped automatically.

**`chunk_callback` must return quickly.** On the sync path it runs in the caller's task; on
the async path it runs in the worker task. Either way, blocking on UART TX, SPI, or a display
driver stalls the entire decode pipeline. For slow peripherals, enqueue the pixel row into a
FreeRTOS queue and return immediately; a separate task drains it.

**Work buffer must be in accessible RAM.** DRAM or SPIRAM — not flash. Minimum size is
`JPEG_DECODER_WORK_BUF_MIN` (3096 bytes); `JPEG_DECODER_WORK_BUF_DEFAULT` (4096 bytes)
is safe for all standard JPEG images.

**Filesystem must be mounted by caller.** The component does not initialize SDMMC, SPI,
SPIFFS, FATFS, or LittleFS.

**Stack size on the sync path.** The decode runs on the caller's stack. For wide images
decoded at 1:1 scale, the RGB888 conversion path allocates a per-row line buffer on the
stack. Ensure the calling task has at least 8 KB of stack.

---

## Error Handling

```c
static void on_done(const jpeg_done_event_t *evt)
{
    if (evt->result != JPEG_DECODE_OK)
        ESP_LOGE(TAG, "decode failed: %s", jpeg_decoder_err_to_str(evt->result));
}
```

---

## Known Limitations

* Maximum ROI height: `JPEG_MAX_ROI_HEIGHT` (512 rows, configurable via Kconfig)
* RGB888 conversion uses a stack-allocated line buffer — on the sync path ensure the calling task has at least 8 KB stack; on the async path the worker task stack covers this
* TJpgDec is not reentrant — do not call from multiple tasks simultaneously
* Progressive JPEGs are not supported (TJpgDec limitation)

---

## License

MIT License — see LICENSE file.