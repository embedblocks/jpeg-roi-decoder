# jpeg_roi_decoder — SD Card Example

Decodes a JPEG from an SD card and writes the raw RGB565 output to a file on the same card.
Demonstrates the high-level `jpeg_decoder_decode_view` API with a `FILE*` source and streaming chunk output.

---

## What it does

1. Mounts the SD card
2. Opens `flower.jpg` from the SD card root
3. Decodes it to fit a 320×240 LCD viewport (centered, auto scale)
4. Streams each decoded row directly to `rgb565.raw` via `fwrite`
5. Closes the output file when done

No full-frame buffer is needed — the decoder writes one row at a time to disk.

---

## Hardware required

| Component | Notes |
|---|---|
| ESP32 | Any variant |
| SD card module | SPI mode |
| SD card | FAT32 formatted |

---

## SD card contents

Place these files on the SD card before running:

```
/sdcard/flower.jpg      ← input JPEG (any size)
```

After running, the SD card will contain:

```
/sdcard/rgb565.raw      ← raw RGB565 output, lcd_w × lcd_h × 2 bytes
```

---

## Configuration

Edit the defines at the top of `main.c`:

```c
#define LCD_W  320    /* output viewport width  in pixels */
#define LCD_H  240    /* output viewport height in pixels */
```

The decoder centers the JPEG in the viewport and picks the best scale automatically.
To override scale:

```c
view.scale = JPEG_SCALE_1_2;   /* 1/1, 1/2, 1/4, 1/8 */
```

To pan from center (in LCD pixels):

```c
view.pan_x = 50;    /* shift viewport 50px right */
view.pan_y = -30;   /* shift viewport 30px up    */
```

---

## Build and flash

```bash
idf.py set-target esp32
idf.py build flash monitor
```

---

## Expected output

```
I (319) APP: Decoded byte count 640
I (329) APP: Decoded byte count 640
...                                    ← 240 lines, one per row
I (2100) APP: Streaming finished
```

Total bytes written: `320 × 240 × 2 = 153 600 bytes`

---

## Verifying the output on PC

Use Python to convert `rgb565.raw` to a viewable PNG:

```python
import numpy as np
import cv2

W, H = 320, 240
raw  = np.fromfile("rgb565.raw", dtype=np.uint16).reshape((H, W))

r = ((raw >> 11) & 0x1F) << 3
g = ((raw >>  5) & 0x3F) << 2
b = ( raw        & 0x1F) << 3

cv2.imwrite("output.png", np.dstack((b, g, r)).astype(np.uint8))
print("Saved output.png")
```

---

## Important notes

**`fwrite` return value** — `fwrite(ptr, size, 1, fp)` returns the number of items written (1), not bytes. The chunk callback checks `written != 1` to detect write errors.

**Logging during decode** — the example keeps `ESP_LOGI` inside `on_chunk` for demonstration. In a production streaming application where output goes to UART, remove all logging from callbacks to avoid corrupting the binary stream.

**SD card SPI pins** — configure in `sd_mount_init()` to match your hardware. Default pins follow the ESP32 SPI2 (HSPI) mapping.

**`on_done` is called from the worker task** — on FreeRTOS, `jpeg_decoder_decode_view` returns immediately after queuing. The `fclose` in `on_done` fires asynchronously from the decoder worker task, which is safe as long as `app_main` does not close the file itself.

---

## File structure

```
examples/sdcard/
├── main/
│   ├── CMakeLists.txt
│   └── main.c
├── CMakeLists.txt
└── README.md          ← this file
```

---

## License

MIT License — see root LICENSE file.