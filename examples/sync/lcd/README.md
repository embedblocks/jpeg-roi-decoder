# examples/http — Live HTTPS JPEG → ILI9486 LCD

Fetches a JPEG over HTTPS and renders it to an ILI9486 320×480 LCD in a single
streaming pass. No full frame buffer. No intermediate file. The JPEG byte stream
flows from the TLS socket directly into the decoder and out to the display,
row by row.

---

## What It Demonstrates

- **HTTP streaming source** — `esp_http_client` opened in streaming mode;
  body bytes are not fetched until the decoder asks for them
- **Input prefetch buffer** — `input_buf[JPEG_INPUT_BUF_SIZE]` batches the
  decoder's many small internal reads into chunk-sized calls, cutting TLS/syscall
  overhead during header parsing
- **Automatic scale selection** — `JPEG_SCALE_AUTO` picks 1/1, 1/2, 1/4, or 1/8
  so the image fits the 320×480 viewport
- **Pan control** — each decode cycle advances `pan_x` / `pan_y` to show a
  different region of the image without re-downloading it
- **Correct resource lifetime** — the HTTP client is closed inside `on_done`,
  never before, because the worker task is still calling `http_read_cb` until
  that point
- **FreeRTOS async decode** — `jpeg_decoder_decode_view` returns immediately;
  `ulTaskNotifyTake` in `app_main` waits for `on_done` before starting the next
  cycle

---

## Hardware

| Signal | ILI9486 Pin | Notes |
|--------|-------------|-------|
| LCD_W  | —           | 320 px, set in `app_main.c` |
| LCD_H  | —           | 480 px, set in `app_main.c` |

Wiring and GPIO assignments live in `lcd_init.h` / `lcd_init.c`. Adapt these to
your board.

---


## Configuration

All user-facing settings are exposed via `idf.py menuconfig` under
**Example Configuration**:

| Key | Default | Description |
|-----|---------|-------------|
| `CONFIG_JPEG_URL` | *(required)* | Full HTTPS URL of the JPEG to fetch |

Set the URL before building:

```
idf.py menuconfig
# → Example Configuration → JPEG URL
```



### WiFi credentials

WiFi SSID and password are set through the standard ESP-IDF Wi-Fi example
config (`Example Connection Configuration` in menuconfig). This example calls
`example_connect()` from `protocol_examples_common`.

---

## Build and Flash

```bash
idf.py set-target esp32        # or esp32s3 / esp32s2 / esp32c3
idf.py menuconfig              # set JPEG URL and WiFi credentials
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

---

## How the Code Works

### Startup sequence

```
wifi_init()
    └─ nvs_flash_init + esp_netif_init + example_connect
lcd_output_init()
jpeg_decoder_init()
```

### Decode loop

```
pan_next(&p)               ← advance viewport
http_open(JPEG_URL)        ← TCP connect + TLS handshake + read headers only
jpeg_decoder_decode_view() ← enqueue job; returns immediately
ulTaskNotifyTake()         ← block until on_done fires

    [worker task]
        http_read_cb() ← reads from esp_http_client incrementally
        on_chunk()     ← writes one MCU row band to LCD via lcd_on_chunk()
        on_done()      ← http_close() + lcd_output_frame_begin() + notify main
```

### `http_read_cb`

The read callback is the only bridge between `esp_http_client` and the decoder.
It handles two cases:

- **Normal read** (`dst != NULL`) — calls `esp_http_client_read` and returns
  the byte count
- **Skip** (`dst == NULL`) — drains and discards bytes using a local 256-byte
  stack buffer; required for non-seekable sources

When `input_buffer` is set, the component's prefetch layer calls `http_read_cb`
in `JPEG_INPUT_BUF_SIZE`-byte chunks and serves TJpgDec's small internal reads
from that buffer. The callback never sees the tiny 1- or 4-byte reads that
TJpgDec issues internally.

### Resource lifetime

```c
/* CORRECT */
static void on_done(const jpeg_done_event_t *evt) {
    http_close();              // safe — worker has finished
    lcd_output_frame_begin();
    xTaskNotifyGive(main_task_handle);
}

/* WRONG — crashes */
jpeg_decoder_decode_view(...); // returns immediately; worker still running
http_close();                  // races with worker task
```

---

## Memory Layout

All buffers are `static` so they persist for the lifetime of the application.
The decoder worker task holds pointers to them; stack-allocated buffers would be
dangling by the time `on_done` fires.

| Buffer | Size | Location |
|--------|------|----------|
| `workbuf` | 4096 B (`JPEG_DECODER_WORK_BUF_DEFAULT`) | DRAM |
| `chunk_buf` | `JPEG_CHUNK_BUF_PIXELS(320)` × 2 B ≈ 10 240 B | DRAM, 4-byte aligned |
| `input_buf` | 2048 B (`JPEG_INPUT_BUF_SIZE`) | DRAM, 4-byte aligned |

`chunk_buf` and `input_buf` carry `__attribute__((aligned(4)))` for DMA
compatibility with the LCD SPI peripheral.

---

## Pan Control

`pan_control` wraps a simple state machine. Each call to `pan_next` advances the
viewport by `PAN_STEP_X` / `PAN_STEP_Y` pixels (20 px each by default), wrapping
at the configured image bounds (30×30 steps in this example). The resulting
`p.x` / `p.y` values are passed directly to `view.pan_x` / `view.pan_y`.

Pan is in **LCD pixels** at the chosen scale, not in original JPEG coordinates.

---

## Retry Behaviour

If `http_open` fails (network error, non-200 status, TLS timeout), the loop logs
an error and waits `RETRY_DELAY_MS` (2 000 ms) before retrying. The HTTP read
timeout is set to 10 000 ms via `esp_http_client_set_timeout_ms`, which
propagates through to the mbedTLS layer.

---




## License

MIT — see the top-level LICENSE file.