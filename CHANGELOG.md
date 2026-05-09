# Changelog
---
## [0.4.0] - 2026-05-09
### Added
- **Input prefetch buffer** — new optional `input_buffer` field on `jpeg_view_intent_t` and `jpeg_decode_request_t`. When set, `input_func` reads from the source exclusively in `JPEG_INPUT_BUF_SIZE`-byte chunks and serves TJpgDec's small internal requests (1, 4, 14, 65 bytes during header parse) from that buffer. The caller's callback never sees reads smaller than one full chunk. Transport layers — HTTP, HTTPS, TCP, UART — only ever receive large, aligned reads, preventing the rapid-fire small-request pattern that firewalls and CDNs treat as malicious.
- `JPEG_INPUT_BUF_SIZE` — compile-time constant (default 2048 bytes) controlling the prefetch chunk size. Override per-project: `target_compile_definitions(... PRIVATE JPEG_INPUT_BUF_SIZE=4096)`.
- `JPEG_INPUT_BUF_SIZE` sizing rationale: 2048 bytes covers most JFIF and Exif JPEG headers in a single refill. For images with embedded XMP or large ICC profiles, increase accordingly.

### Changed
- `input_func` now has two paths: the original zero-copy direct path (when `input_buffer == NULL`) and the new prefetch buffer path (when `input_buffer` is set). The direct path is entirely unchanged — no performance regression for file and buffer sources.
- Skip requests (`dst == NULL` from TJpgDec) are handled inside the prefetch buffer when `input_buffer` is set: bytes are drained from the buffer and discarded, refilling from the source as needed. The caller's callback always receives a real destination pointer — it never needs to handle `dst == NULL` for non-seekable sources.
- `jpeg_view_default()` zero-initialises `input_buffer` to `NULL` (direct pass-through). Existing callers require no changes.
- `decode_job_t` internal type: `raw` union carries `input_buffer` pointer so the RTOS worker correctly propagates it to `jpeg_decoder_core_run_request`.

### Not changed
- Public API signatures for `jpeg_decoder_decode_view()` and `jpeg_decoder_decode()` are unchanged.
- `jpeg_reader_t`, `jpeg_view_intent_t` (other fields), `jpeg_decode_request_t` (other fields) are unchanged except for the new `input_buffer` field.
- Default behaviour (`input_buffer = NULL`) is identical to 0.3.x — no migration required for existing callers.

### Migration from 0.3.x

No breaking changes. To opt in to the prefetch buffer for HTTP or other non-seekable sources:

```c
/* 0.3.x — unchanged, still valid */
static uint8_t  workbuf[JPEG_DECODER_WORK_BUF_DEFAULT];
static uint16_t chunk_buf[JPEG_CHUNK_BUF_PIXELS(LCD_W)];

/* 0.4.0 — add these two lines for HTTP/socket sources */
static uint8_t  input_buf[JPEG_INPUT_BUF_SIZE];
view.input_buffer = input_buf;
```

File, buffer, and PSRAM sources: leave `input_buffer = NULL`. The direct path is faster for local memory.

---
## [0.3.0] - 2026-05-02
### Added
- **True streaming input** — decoder now runs a single forward pass through the JPEG byte stream. The source never rewinds. Non-seekable sources (HTTP, UART, TCP, FreeRTOS queue, DMA ring buffer) are now fully supported.
- `jpeg_read_cb_t` — new read callback typedef: `size_t (*)(uint8_t *dst, size_t max, void *ctx)`. This is the sole source abstraction. When `dst` is `NULL`, the decoder is requesting a skip; seekable sources can implement this as `fseek` (zero RAM, zero copy).
- `jpeg_reader_t` — lightweight `{ cb, ctx }` struct that pairs the callback with a caller-owned context. Embeds no internal state.
- `jpeg_view_intent_t` — replaces `jpeg_view_t`. Now includes `reader` and `chunk_buffer` fields directly. `work_buffer` is intentionally kept separate (passed to `jpeg_decoder_decode_view()`) to keep execution resources out of the intent struct.
- `input_func` retry loop — handles partial reads from the callback without any internal staging buffer. TJpgDec's own buffer is passed directly to `reader.cb` (zero copy).

### Changed
- `jpeg_decoder_decode_view()` signature changed: first argument is now `const jpeg_view_intent_t *intent` (source embedded inside intent). Old signature took a separate `jpeg_source_t` as the first argument.
- `jpeg_decode_request_t.source` renamed to `.reader` (`jpeg_reader_t`).
- `decode_context_t` internal field `source` replaced by `reader`.
- RTOS worker task now owns the **full decode lifecycle** — `tjpgd_sys_prepare` and `tjpgd_sys_decomp` both run in the worker. Previously, `prepare` (header parse + probe) ran in the calling task before the job was queued.
- `jpeg_decoder_probe()` signature changed: takes `jpeg_reader_t` by value instead of `jpeg_source_t`. Caller is responsible for resetting their own source context after a probe call; the component provides no reset mechanism.
- `jpeg_view_default()` now returns `jpeg_view_intent_t` with `reader = {NULL, NULL}` and `chunk_buffer = NULL`.
- `decode_job_t` internal type updated: carries `jpeg_reader_t reader` instead of stream fields; `use_intent` flag selects high-level vs low-level core path.
- `done_callback` is now always fired — even on early parameter validation errors — so callers can always rely on it for cleanup (e.g. closing files, freeing resources).

### Removed
- `jpeg_source_t` — replaced by `jpeg_reader_t`.
- `jpeg_view_t` — replaced by `jpeg_view_intent_t`.
- `jpeg_decoder_source_from_file()` — callers implement a 5-line `file_read_cb`. Example provided in README and header comments.
- `jpeg_decoder_source_from_buffer()` — callers implement a 7-line `buf_read_cb` with a `buf_ctx_t`. Example provided in README and header comments.
- `jpeg_roi_decoder_helpers.c` — file deleted entirely. The component ships no source backends.
- `seek()` field on source abstraction — no seek operation exists anywhere in the new API. Callers who need to seek do so inside their own callback.
- `jpeg_decoder_prepare_view_request()` internal helper — logic absorbed into `jpeg_decoder_core_run_view()`.
- Double header parse — `probe()` followed by `seek(0)` followed by a second `prepare()` inside `core_run` is gone. One prepare, one decomp, no rewind.

### Migration from 0.2.x

| 0.2.x | 0.3.x |
|---|---|
| `jpeg_source_t` | `jpeg_reader_t` |
| `jpeg_view_t` | `jpeg_view_intent_t` |
| `jpeg_decoder_source_from_file(fp)` | Write a `file_read_cb`; set `reader = { file_read_cb, fp }` |
| `jpeg_decoder_source_from_buffer(data, len)` | Write a `buf_read_cb` with a `buf_ctx_t` |
| `jpeg_decoder_decode_view(src, &view, ...)` | `jpeg_decoder_decode_view(&view, ...)` |
| `jpeg_decode_request_t.source` | `jpeg_decode_request_t.reader` |

---
## [0.2.0] - 2026-04-04
### Fix
- UART example fixed, now correctly received

---
## [0.1.0] - 2026-04-04
### Release
- First release