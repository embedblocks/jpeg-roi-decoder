# jpeg_roi_decoder — UART Streaming Example

Decodes a JPEG embedded in flash and streams the raw RGB565 output row-by-row to a PC over UART.
Demonstrates the high-level `jpeg_decoder_decode_view` API with a `buf_read_cb` source
streaming directly from a flash-embedded binary blob — no SD card, no filesystem required.

A companion Python script (`image_rcv.py`) receives the stream, reconstructs the image, and saves it as a PNG.

---

## What it does

### ESP32 side

1. Waits for a single trigger byte from the PC
2. Switches UART baud rate to 921600 for high-speed transfer
3. Sends a small binary header (sync pattern, magic, width, height, format)
4. Creates a `jpeg_reader_t` backed by a `buf_read_cb` pointing at the flash blob
5. Decodes to fit a 320×240 LCD viewport (centered, auto scale)
6. Streams each decoded row over UART via `uart_write_bytes` inside `on_chunk`

### PC side

1. Sends the trigger byte
2. Switches to 921600 baud
3. Reads and validates the binary header
4. Receives raw RGB565 rows until the full frame is complete
5. Reconstructs and saves the image as a PNG

No full-frame buffer is needed on the ESP32 — pixel rows go from the decoder callback straight onto the wire.

---

## Hardware required

| Component | Notes |
|---|---|
| ESP32 | Any variant |
| USB–UART cable | The same port used for flashing |

No SD card or external storage needed — the JPEG is embedded directly in the firmware.

---

## Embedding the JPEG

Add the image to your `CMakeLists.txt`:

```cmake
target_add_binary_files(${COMPONENT_TARGET} "test_image.jpg")
```

Then reference it in `main.c`:

```c
extern const uint8_t test_jpg_start[] asm("_binary_test_image_jpg_start");
extern const uint8_t test_jpg_end[]   asm("_binary_test_image_jpg_end");
```

The image is linked directly into flash — no filesystem mount, no `fopen`.

---

## Configuration

Edit the defines at the top of `main.c`:

```c
#define LCD_W  320    /* output viewport width  in pixels */
#define LCD_H  240    /* output viewport height in pixels */
```

To override scale or pan:

```c
view.scale = JPEG_SCALE_1_2;   /* 1/1, 1/2, 1/4, 1/8 — or leave JPEG_SCALE_AUTO */
view.pan_x =  50;              /* shift viewport 50px right */
view.pan_y = -30;              /* shift viewport 30px up    */
```

---

## Build and flash

```bash
idf.py set-target esp32
idf.py build flash monitor
```

Leave the monitor open — the ESP32 will print `=== READY, waiting for trigger ===` and wait.

---

## Running the receiver

In a separate terminal:

```bash
python image_rcv.py
```

The script will:
- Send the trigger byte
- Switch baud rate
- Receive the header and all pixel rows
- Save `output.png`

---

## Expected output

**ESP32 serial monitor:**

```
I (319) JPEG_UART: === READY, waiting for trigger ===
I (341) JPEG_UART: Trigger 0x01 received — switching baud
I (JD_CORE) JPEG_PAN: ROI(scaled): left=0 top=0 right=319 bottom=239
I (XXX) JPEG_UART: Streaming finished
```

**Python receiver:**

```
Waiting for header...
Header OK — 320x240 RGB565
Receiving 153600 bytes...
Done. Saved output.png
```

Total bytes transferred: `320 × 240 × 2 = 153 600 bytes`

---

## Important notes

**Do not use `ESP_LOG` inside `on_chunk`.** On this example, `stdout` and the image data share
the same UART. Any log output inside the chunk callback will corrupt the binary pixel stream
on the PC side. All logging is suppressed with `esp_log_level_set("*", ESP_LOG_NONE)` before
streaming begins.

**`on_chunk` must return quickly.** The callback calls `uart_write_bytes` and
`uart_wait_tx_done`, which block until the TX buffer drains. At 921600 baud one row
(640 bytes) takes roughly 7 ms. This is acceptable here but sets a hard floor on decode
throughput. For faster peripherals or tighter timing, enqueue rows into a FreeRTOS queue
and drain from a separate task.

**Baud rate switch timing.** The ESP32 waits 600 ms after the trigger before switching baud
and a further 100 ms before sending the header. The Python script must match this delay. Do
not reduce these without also adjusting the receiver.

**Buffer lifetime.** `workbuf`, `chunk_buf`, and the flash blob context are all `static`.
`jpeg_decoder_decode_view` returns immediately after queuing — the worker task decodes in
the background and `on_done` fires asynchronously. Static allocation ensures nothing goes
out of scope.

---

## File structure

```
examples/uart/
├── main/
│   ├── CMakeLists.txt
│   ├── main.c
│   └── test_image.jpg      ← embedded at build time
├── image_rcv.py            ← PC-side receiver script
├── CMakeLists.txt
└── README.md
```

---

## License

MIT License — see root LICENSE file.