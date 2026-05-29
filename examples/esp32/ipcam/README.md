# JPEG ROI Decoder — ILI9486 Landscape Example

This example demonstrates streaming JPEG decoding from an HTTP source (IPCAM JPEG URL) onto an ILI9486-based LCD in landscape orientation using the `jpeg_decoder` component.

## What It Does

- Opens an HTTP connection and streams a JPEG image incrementally
- Decodes the image on the fly using the JPEG ROI decoder
- Displays it row-by-row on a 480×320 landscape LCD with no full framebuffer required

## Hardware

- ESP32 (or compatible)
- ILI9486-based LCD panel (native portrait 320×480, driven over SPI)
- SPI wiring: MOSI, MISO, CLK, CS, DC, RST, backlight

## Display Orientation

The panel is driven in landscape mode (480 wide × 320 tall) using the hardware `swap_xy` (MV bit in MADCTL). No software rotation is performed — the hardware handles it transparently.

When configuring the decoder, pass the **logical landscape dimensions**:

```c
jpeg_view_intent_t view = jpeg_view_default(480, 320);
```

## Pixel Format

The decoder outputs RGB565. The SPI bus is configured with `swap_color_bytes = 1` in the IO config to handle byte order automatically — no manual byte swapping needed in the chunk callback.

## Scaling and Panning

Use `jpeg_decode_scale_t` to fit the image to the display:

```c
view.scale = JPEG_SCALE_1_2;   // halves a 1280×720 image to 640×360
view.pan_x = 0;                // horizontal offset within the scaled image
view.pan_y = 0;                // vertical offset — keep within valid range
```

The decoder automatically centers the crop window on the scaled image. Valid pan range is `0` to `scaled_dimension - lcd_dimension`. Exceeding this clips the ROI and leaves undrawn rows.

## Chunk Callback

The decoder fires one callback per decoded row. Draw it directly to the panel:

```c
bool lcd_on_chunk(const jpeg_chunk_event_t *evt)
{
    xSemaphoreTake(signal_color_done, portMAX_DELAY);
    esp_lcd_panel_draw_bitmap(
        s_panel,
        0,          evt->y,
        evt->width, evt->y + 1,
        evt->pixels
    );
    return true;
}
```

`evt->y` is the row index within the decoded ROI (0-based). `evt->width` equals the ROI width — `lcd_width` unless the image is narrower after scaling.

The semaphore is released in the `on_color_trans_done` callback registered with the SPI panel IO, preventing the next row from being written before the current DMA transfer completes.

## Input Buffering

For HTTP sources, provide an input buffer so the decoder can prefetch data rather than making many small socket reads:

```c
static uint8_t input_buf[JPEG_INPUT_BUF_SIZE];
view.input_buffer = input_buf;
```

This significantly improves throughput on network sources.

