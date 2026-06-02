# Changelog
---
## [0.5.2] — 2026-06-02
 
### Added
- **Synchronous threading adapter** (`adapter/sync/`) — `jpeg_decoder_decode_view()` and
  `jpeg_decoder_decode()` block until decode is fully complete on the sync path.
  The caller's task drives the decode loop; no internal queue or worker task is created.
- **CMake threading selector** — set `JPEG_DECODER_THREADING` to `"sync"` (default) or
  `"async"` in your project `CMakeLists.txt` to choose the adapter at build time.
  The public API is identical either way.
- **Sync variants of three examples** — `examples/https/sync`, `examples/lcd/sync`,
  `examples/ipcam/sync` demonstrate the blocking path with correct HTTP resource lifetime.
### Changed
- Platform adapter folder renamed from `platform/{desktop,esp}` to `adapter/{sync,async}` —
  the distinction is threading model, not target platform. Both adapters run on ESP32.
- Default threading model is now `sync`. Projects relying on the implicit async behaviour
  must set `JPEG_DECODER_THREADING=async` explicitly — one line, no API changes.
- `jpeg_decoder_init()` and `jpeg_decoder_deinit()` are no-ops on the sync path.
  Calling them is harmless but not required.
- README restructured: threading model gets its own top-level section; buffer lifetime and
  return value semantics documented separately for sync and async paths.
### Fixed
- Removed two hardcoded debug coordinate checks left over from a development session
  (`rect->left == 304 && rect->top == 48` and `roi_y >= 55 && roi_y <= 65`) from
  `jpeg_decoder_core.c`.
---

## [0.5.1] — 2026-06-02
### Fixed
- The esp registry link used for esp-lcd-ili9486 insead of git link


---
## [0.5.0] - 2026-05-29
### Added
- **lcd** and **ipcam** examples added
### Fixed
- Image with dimensions less than lcd/roi now supported

---
## [0.4.0] - 2026-05-09
### Added
- **Input prefetch buffer** — new optional `input_buffer` field on `jpeg_view_intent_t` and
  `jpeg_decode_request_t`. When set, `input_func` reads from the source exclusively in
  `JPEG_INPUT_BUF_SIZE`-byte chunks and serves TJpgDec's small internal requests (1, 4, 14,
  65 bytes during header parsing) from that buffer.

  TJpgDec makes 15–25 `reader.cb` calls during header parsing alone. For sources where each
  call has non-trivial overhead — HTTP (TLS stack + `recv()` syscall), sockets, UART, FreeRTOS
  queues — batching those into one or two chunk-sized calls measurably reduces header parse
  time and overall callback pressure. For in-memory and file sources the overhead is negligible
  and `input_buffer` should be left `NULL`.

- `JPEG_INPUT_BUF_SIZE` — compile-time constant (default 2048 bytes) controlling the prefetch
  chunk size. 2048 bytes covers most JFIF and Exif headers in a single refill. Override
  per-project: `target_compile_definitions(... PRIVATE JPEG_INPUT_BUF_SIZE=4096)`.

### Changed
- `input_func` now has two paths: the original zero-copy direct path when `input_buffer == NULL`,
  and the new prefetch path when `input_buffer` is set. The direct path is entirely unchanged —
  no performance impact for file and buffer sources.
- Skip requests (`dst == NULL` from TJpgDec) are handled inside the prefetch buffer without
  calling the source with a NULL destination. The caller's callback always receives a real
  pointer — non-seekable sources need no special skip handling.
- `jpeg_view_default()` zero-initialises `input_buffer` to `NULL`. Existing callers are
  unaffected.
- `decode_job_t` internal type: `raw` union carries `input_buffer` so the RTOS worker
  correctly propagates it through `jpeg_decoder_core_run_request`.

### Not changed
- Public API signatures for `jpeg_decoder_decode_view()` and `jpeg_decoder_decode()`.
- Default behaviour (`input_buffer = NULL`) is identical to 0.3.x. No migration required.

### Migration from 0.3.x

No breaking changes. To opt in for HTTP or other high-overhead sources:

```c
/* add one buffer */
static uint8_t input_buf[JPEG_INPUT_BUF_SIZE];

/* add one assignment */
view.input_buffer = input_buf;
```

Leave `input_buffer = NULL` for file, buffer, and PSRAM sources.

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