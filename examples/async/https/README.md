# HTTPS Streaming JPEG Decoder — ESP32 Example

Fetches a JPEG image over HTTPS and decodes it in a single forward pass, sending
each completed pixel row to a host PC over UART. No full-image buffer is ever
allocated — pixel rows flow from socket to display line by line.

---

![Lenna](https://i.gzn.jp/img/2009/06/18/lenna/000.jpg)

*Test image — the classic Lenna photograph (512 × 512 px, ~32 KB JPEG)*

---

## What it does

```
WiFi → HTTPS socket → http_read_cb → jpeg_roi_decoder → on_chunk → UART → PC
```

The JPEG decoder drives the data pull. It calls `http_read_cb` whenever it needs
more compressed bytes. `http_read_cb` calls `esp_http_client_read`, which pulls
from the TLS socket on demand. The decoder never sees the transport layer; the
transport layer never sees the decoder. They are decoupled by a single callback.

---

## Memory layout

```c
static uint8_t  workbuf[JPEG_DECODER_WORK_BUF_DEFAULT];   // TJpgDec scratch (4 KB)
static uint16_t chunk_buf[JPEG_CHUNK_BUF_PIXELS(LCD_W)];  // one MCU row band (10 KB)
static uint8_t  input_buf[JPEG_INPUT_BUF_SIZE];            // HTTP prefetch (2 KB)
```

The three buffers are the complete memory cost of the decode pipeline. There is
no full-image buffer. A 320 × 240 RGB565 frame would require 150 KB; this example
uses 16 KB regardless of image size.

---

## The input prefetch buffer

TJpgDec parses JPEG headers with many small internal reads — 1, 4, 14, 65 bytes
at a time. For an in-memory source this costs nothing. Over HTTPS, each of those
calls goes through `esp_http_client_read` → TLS decrypt → `recv()` syscall. During
header parsing alone the decoder makes 15–25 such calls, each carrying full TLS
stack overhead.

`view.input_buffer` solves this. When set, `input_func` always fetches
`JPEG_INPUT_BUF_SIZE` bytes from the source in one call and serves TJpgDec's
small requests from that buffer. The TLS stack sees only large, aligned reads;
header parse time drops from many small round-trips to one or two.

```c
view.input_buffer = input_buf;   // one line — that is the entire change
```

Setting `input_buffer = NULL` restores the original zero-copy pass-through, which
is correct for in-memory or file sources where small reads are free.

---

## HTTP reader callback

```c
static size_t http_read_cb(uint8_t *dst, size_t max, void *vctx)
```

Implements `jpeg_read_cb_t`. The decoder calls it with a destination buffer and
a byte count. The callback calls `esp_http_client_read` and returns however many
bytes arrived. Partial reads are valid — the decoder retries internally.

When `dst == NULL` the decoder wants to skip forward past bytes it does not need
(unknown EXIF blocks, padding). HTTP is not seekable, so the callback reads and
discards into a small stack buffer rather than calling `fseek`. With
`input_buffer` set this path is rarely hit — skips are handled inside the prefetch
buffer without touching the HTTP client at all.

---

## HTTP lifecycle — why `http_close` lives in `on_done`

`jpeg_decoder_decode_view` on the RTOS path enqueues the job and returns
immediately. The decode runs in a worker task. If `http_close` is called from
`app_main` after `decode_view` returns, it races with the worker task and frees
the TLS cipher context while decryption is in progress — a NULL-dereference crash
inside `mbedtls_cipher_aead_decrypt`.

The fix is to treat `on_done` as the destructor for the HTTP session:

```c
static void on_done(const jpeg_done_event_t *evt)
{
    http_close();   // safe — decoder guarantees no more reader.cb calls after this
}
```

The component's documentation states: *"reader.ctx must remain valid until
done_callback fires."* Flipping that around — `on_done` is the correct and only
safe place to release reader resources.

---

## `esp_http_client_set_timeout_ms`

The `timeout_ms` field in `esp_http_client_config_t` applies to the connection
phase. For the manual `open → fetch_headers → read` pattern, call
`esp_http_client_set_timeout_ms` explicitly after open to ensure the per-read
socket timeout is enforced:

```c
esp_http_client_set_timeout_ms(http_ctx.client, 10000);
```

Without this, a server that stops sending mid-stream will block
`esp_http_client_read` indefinitely and `on_done` will never fire.

---

## UART sync protocol

Before sending pixel data the firmware synchronises with the host:

| Field | Size | Value |
|-------|------|-------|
| `sync` | 3 bytes | `0xAA 0xAA 0xAA` |
| `magic` | 4 bytes | `0xDEADBEEF` |
| `width` | 2 bytes | LCD width in pixels |
| `height` | 2 bytes | LCD height in pixels |
| `format` | 1 byte | `0` = RGB565 |

After the header, pixel rows follow back-to-back with no framing. Each row is
`width × 2` bytes of little-endian RGB565.

The host sends a single trigger byte to start the sync sequence. The firmware
then switches baud rate to 921600 and transmits the header.

---

## Configuration

```c
#define LCD_W       320
#define LCD_H       240
#define JPEG_URL    "https://i.gzn.jp/img/2009/06/18/lenna/000.jpg"
#define DEBUG       1   // 1 = skip UART, log to console only
```

`JPEG_INPUT_BUF_SIZE` (default 2048) is defined in `jpeg_roi_decoder.h` and can
be overridden per-project:

```cmake
target_compile_definitions(${COMPONENT_TARGET} PRIVATE JPEG_INPUT_BUF_SIZE=4096)
```

---

## Build

```bash
idf.py set-target esp32
idf.py menuconfig   # set WiFi SSID / password under Example Connection
idf.py build flash monitor
```

Requires ESP-IDF v5.x. The `example_connect` helper comes from
`esp-idf/examples/common_components/protocol_examples_common`.

---

## Notes

**Worker task stack size.** The full call depth over HTTPS is deep: decoder →
`input_func` → `http_read_cb` → `esp_http_client_read` → TLS → `mbedtls_ssl_read`.
Set the worker task stack to at least 16 KB. With mbedTLS debug logging enabled,
increase further — each log line adds call-frame depth inside the TLS stack.

**HTTP resource lifetime.** The `http_ctx` handle must not be closed or modified
from any task other than the decoder worker after `jpeg_decoder_decode_view` is
called. All cleanup belongs in `on_done`.