# jpeg_roi_decoder

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x-blue)
![Espressif Component Registry](https://img.shields.io/badge/Espressif-Component%20Registry-orange)
![License](https://img.shields.io/badge/license-MIT-green)

Region-of-interest JPEG decoder for ESP32. Stream JPEG data in from any source,
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
* **Zero heap allocation** — work buffer and chunk buffer are caller-supplied
* **FreeRTOS async** — enqueue a job and return; decode runs in a dedicated worker task
* **Synchronous path** — for bare-metal builds and host-side testing

---

## Chip Support

| Chip | Status |
|---|---|
| ESP32 | ✅ Tested |
| ESP32-S3 | ⚠️ Expected to work |
| ESP32-S2 | ⚠️ Expected to work |
| ESP32-C3 | ⚠️ Expected to work |

---

## Installation

```bash
idf.py add-dependency "jpeg_roi_decoder^0.4.0"
```

Or in `idf_component.yml`:

```yaml
dependencies:
  jpeg_roi_decoder: "^0.4.0"
```

---

## How It Works

The decoder runs a single forward pass through the JPEG byte stream:

```
your callback feeds bytes
        ↓
   header parse          ← image dimensions become known here
        ↓
   scale resolution      ← JPEG_SCALE_AUTO picks best fit
        ↓
   ROI computation       ← centered, panned, clamped
        ↓
   pixel decode          ← your on_chunk() fires once per row
        ↓
   on_done()
```

The source never rewinds. This means sources that cannot seek — HTTP, UART, sockets,
queues — work just as well as files and buffers.

---

## Source Abstraction — `jpeg_reader_t`

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

#### `FILE*` — SD card, SPIFFS, LittleFS

```c
size_t file_read_cb(uint8_t *dst, size_t max, void *ctx)
{
    FILE *fp = ctx;
    if (dst == NULL)
        return fseek(fp, (long)max, SEEK_CUR) == 0 ? max : 0;
    return fread(dst, 1, max, fp);
}

jpeg_reader_t reader = { .cb = file_read_cb, .ctx = fp };
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

## Usage

### High-level API

Give the decoder your LCD dimensions. It parses the header, picks the best scale,
centers the image, applies your pan offset, and calls `on_chunk` once per completed row.

```c
#include "jpeg_roi_decoder.h"

static uint8_t  workbuf[JPEG_DECODER_WORK_BUF_DEFAULT];
static uint16_t chunk_buf[JPEG_CHUNK_BUF_PIXELS(320)];

static bool on_chunk(const jpeg_chunk_event_t *evt)
{
    /* evt->pixels is valid only during this call */
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
    /* view.pan_x = 40; view.pan_y = -20; */   /* optional */

    jpeg_decoder_decode_view(
        &view,
        workbuf, sizeof(workbuf),
        on_chunk, on_done, NULL
    );
}
```

### Using a flash-embedded image (no filesystem, no `FILE*`)

```c
extern const uint8_t img_start[] asm("_binary_splash_jpg_start");
extern const uint8_t img_end[]   asm("_binary_splash_jpg_end");

static buf_ctx_t img_ctx;   /* static — must outlive the decode job */

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
    .chunk_callback      = on_chunk,
    .done_callback       = on_done,
};
jpeg_decoder_decode(&req);
```

---

## Examples

* `examples/uart` — Flash-embedded JPEG decoded and streamed over UART; `image_rcv.py` displays it on the PC
* `examples/sdcard` — JPEG read from SD card, decoded RGB565 written back to SD card

---

## Buffer Lifetime

`jpeg_decoder_decode_view()` and `jpeg_decoder_decode()` return immediately on the RTOS path —
the worker task decodes in the background. Three things must remain valid until `done_callback` fires:

| | Must stay valid until |
|---|---|
| `work_buffer` | `done_callback` |
| `chunk_buffer` | `done_callback` |
| `reader.ctx` and everything it points to | `done_callback` |

Declaring all three as `static` is the simplest correct choice on embedded targets.

**If your source is a `FILE*`:** do not call `fclose()` after `decode_view` returns.
The worker task is still calling `fread()` on it. Close the file inside `on_done` — that is the only safe point.

```c
/* ✅ correct */
static void on_done(const jpeg_done_event_t *evt) {
    fclose(((my_ctx_t *)evt->user_data)->fin);
}

/* ❌ crash — worker still reading */
jpeg_decoder_decode_view(...);
fclose(fin);
```

---

## Return Values

On the **RTOS async path**, the return value of `decode_view` / `decode` reflects queuing only:

| Value | Meaning |
|---|---|
| `JPEG_DECODE_OK` | Request accepted — worker will decode |
| `JPEG_DECODE_ERR_PARAM` | Bad argument — not queued |
| `JPEG_DECODE_ERR_INTR` | Queue full — not queued |

`JPEG_DECODE_OK` does **not** mean the decode succeeded. All decode results — including header
parse failures and mid-stream errors — arrive via `done_callback.result`.

On the **synchronous path**, the return value is the decode result directly.

---

## Notes

**Scale is discrete.** TJpgDec supports `1/1`, `1/2`, `1/4`, `1/8` only. `JPEG_SCALE_AUTO` picks
the smallest factor where the scaled image is at least as large as the LCD. Use a fixed value
if you need predictable output dimensions.

**Pan is in LCD pixels.** `pan_x` / `pan_y` shift the viewport in output pixel units, independent
of scale. `(0, 0)` centers the image. Values that push outside the image are clamped automatically.

**`chunk_callback` must return quickly.** It runs inside the worker task. Blocking on UART TX,
SPI, or a display driver stalls the entire decode pipeline. For slow peripherals, enqueue the
pixel row into a FreeRTOS queue and return immediately; a separate task drains it.

**Work buffer must be in accessible RAM.** DRAM or SPIRAM — not flash. Minimum size is
`JPEG_DECODER_WORK_BUF_MIN` (3096 bytes); `JPEG_DECODER_WORK_BUF_DEFAULT` (4096 bytes)
is safe for all standard JPEG images.

**Filesystem must be mounted by caller.** The component does not initialize SDMMC, SPI,
SPIFFS, FATFS, or LittleFS.

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
* RGB888 conversion uses a stack-allocated buffer — avoid 1:1 decodes of very wide images
* TJpgDec is not reentrant — do not call from multiple tasks simultaneously
* Progressive JPEGs are not supported (TJpgDec limitation)

---

## License

MIT License — see LICENSE file.