# jpeg_roi_decoder

![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.x-blue)
![Espressif Component Registry](https://img.shields.io/badge/Espressif-Component%20Registry-orange)
![License](https://img.shields.io/badge/license-MIT-green)

Region-of-interest JPEG decoder for ESP32 and desktop host testing.
Built on [TJpgDec](http://elm-chan.org/fsw/tjpgd/) with an LCD-aware API that handles scale selection, ROI math, and row-by-row streaming internally.

---

# Features

* Decode any rectangular region of a JPEG — no full image load required
* Stream output row-by-row — no full-frame output buffer needed
* High-level LCD API: provide display size and pan offset, component does the rest
* Automatic scale selection (`JPEG_SCALE_AUTO`)
* RGB565 (native) and RGB888 output formats
* Works with `FILE*`, flash blobs, PSRAM buffers, or any custom I/O backend
* Runs on ESP32 (FreeRTOS async) and desktop (synchronous, for host testing)
* Configurable via Kconfig

---

# Chip Support

| Chip | Status |
|---|---|
| ESP32 | ✅ Tested |
| ESP32-S3 | ⚠️ Expected to work |
| ESP32-S2 | ⚠️ Expected to work |
| ESP32-C3 | ⚠️ Expected to work |

---

# Installation

## Using ESP-IDF Component Manager (Recommended)

```bash
idf.py add-dependency "jpeg_roi_decoder^0.1.0"
```

Or in your project's `idf_component.yml`:

```yaml
dependencies:
  jpeg_roi_decoder: "^0.1.0"
```

Then configure via:

```
idf.py menuconfig
Component config → JPEG ROI Decoder
```

---

# Usage

## High-level API (most users)

Provide your LCD dimensions. The component probes the image, picks the best scale, centers the viewport, and streams rows to your callback.

```c
#include "jpeg_roi_decoder.h"

static uint8_t  work_buf[JPEG_DECODER_WORK_BUF_DEFAULT];
static uint16_t framebuf[480 * 320];

static bool on_chunk(const jpeg_chunk_event_t *evt) {
    uint16_t *dst = framebuf + (size_t)evt->y * 480 + evt->x;
    memcpy(dst, evt->pixels, evt->byte_count);
    return true;
}

void show_image(void) {
    FILE *fp = fopen("/sdcard/photo.jpg", "rb");

    jpeg_view_t view = jpeg_view_default(480, 320);
    // view.pan_x = 50;  // optional: shift viewport from center

    jpeg_decoder_decode_view(
        jpeg_decoder_source_from_file(fp),
        &view,
        work_buf, sizeof(work_buf),
        on_chunk, NULL, NULL
    );

    fclose(fp);
}
```

## Low-level API (advanced)

Provide a ROI in original JPEG coordinates for precise tile control.

```c
jpeg_decode_request_t req = {
    .source              = jpeg_decoder_source_from_file(fp),
    .roi                 = { .left=0, .top=0, .right=1919, .bottom=1279 },
    .scale               = JPEG_SCALE_1_4,
    .out_format          = JPEG_OUTPUT_RGB565,
    .work_buffer         = work_buf,
    .work_buffer_size    = sizeof(work_buf),
    .chunk_buffer        = chunk_buf,
    .chunk_buffer_pixels = 480,
    .chunk_callback      = on_chunk,
};
jpeg_decoder_decode(&req);
```

For complete examples see the `examples/` directory.

---

# Examples

* `examples/uart` — Decodes a file and send through uart to PC where image_rcv.py can be used to read
* `examples/sdcard` — Reads from sdcard module and write back the decode raw rgb565 file to it

---

# Important Notes

## 1️⃣ Scale is discrete, not continuous

TJpgDec supports only four scale factors: `1/1`, `1/2`, `1/4`, `1/8`.
There are no intermediate values. Use `JPEG_SCALE_AUTO` to let the component pick the best fit for your LCD, or set a fixed value if you need predictable output dimensions.

```c
view.scale = JPEG_SCALE_AUTO;   // recommended
view.scale = JPEG_SCALE_1_4;    // fixed
```

## 2️⃣ Pan is in LCD pixels, not JPEG pixels

`pan_x` and `pan_y` are always in output (LCD) pixel units, independent of scale. A value of `(0, 0)` centers the image. Values that push the viewport outside the image are clamped automatically.

```c
view.pan_x = 50;   // shift 50 LCD pixels right — same meaning at any scale
```

## 3️⃣ Filesystem must be mounted by caller

This component does not initialize SPI, SDMMC, SPIFFS, FATFS, or LittleFS. Mount your filesystem before calling `fopen()` and passing the source to the decoder.

## 4️⃣ Work buffer must be in accessible RAM

On ESP32, place `work_buf` in DRAM or SPIRAM. Do not place it in flash (rodata).
Minimum size is `JPEG_DECODER_WORK_BUF_MIN` (3096 bytes). `JPEG_DECODER_WORK_BUF_DEFAULT` (4096 bytes) is safe for all images.

## 5️⃣ Low-level chunk buffer must cover one full ROI row

```c
// chunk_buffer_pixels must be >= (roi.right - roi.left + 1) / scale_divisor
// The component returns JPEG_DECODE_ERR_PARAM if this is violated.
```

## 6️⃣ JPEG_SCALE_AUTO is not valid in the low-level API

`JPEG_SCALE_AUTO` is resolved internally by `jpeg_decoder_decode_view()`.
When using `jpeg_decoder_decode()` directly, you must provide an explicit scale value.

---

# Using Images from Flash (No Filesystem)

Embed a JPEG as a binary blob via `CMakeLists.txt`:

```cmake
target_add_binary_files(${COMPONENT_TARGET} "splash.jpg")
```

Then pass it directly — no `FILE*` needed:

```c
extern const uint8_t splash_jpg[]     asm("_binary_splash_jpg_start");
extern const uint8_t splash_jpg_end[] asm("_binary_splash_jpg_end");

jpeg_decoder_decode_view(
    jpeg_decoder_source_from_buffer(splash_jpg, splash_jpg_end - splash_jpg),
    &view, work_buf, sizeof(work_buf), on_chunk, NULL, NULL
);
```

---

# Configuration (Kconfig)

Available under:

```
Component config → JPEG ROI Decoder
```

Configurable parameters include:

* Default work buffer size
* Maximum ROI height
* Debug logging

---

# Error Handling

All functions return `jpeg_decode_result_t`. Use `jpeg_decoder_err_to_str()` for readable messages:

```c
jpeg_decode_result_t res = jpeg_decoder_decode_view(...);
if (res != JPEG_DECODE_OK) {
    ESP_LOGE(TAG, "decode failed: %s", jpeg_decoder_err_to_str(res));
}
```

---

# Known Limitations

* Maximum ROI height is bounded by `JPEG_MAX_ROI_HEIGHT` (512 rows by default)
* RGB888 conversion uses a stack buffer — not recommended for 1:1 decodes of very wide images
* TJpgDec is not reentrant — do not call from multiple tasks simultaneously
* Progressive JPEGs are not supported (TJpgDec limitation)

---

# License

MIT License — see LICENSE file.