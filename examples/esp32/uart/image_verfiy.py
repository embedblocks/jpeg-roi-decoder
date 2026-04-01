# receive.py
import serial
import struct
import numpy as np

PORT   = "COM5"   # or /dev/ttyUSB0
BAUD   = 921600
W, H   = 320, 240

ser = serial.Serial(PORT, BAUD, timeout=5)

# Read header
hdr_fmt  = "<IHHB"
hdr_size = struct.calcsize(hdr_fmt)
hdr_raw  = ser.read(hdr_size)
magic, width, height, fmt = struct.unpack(hdr_fmt, hdr_raw)

print(f"magic: {magic:#010x}  size: {width}x{height}  fmt: {fmt}")
assert magic == 0xDEADBEEF, "Bad header"

# Read raw RGB565 pixels
total_bytes = width * height * 2
raw = ser.read(total_bytes)
print(f"received {len(raw)} bytes (expected {total_bytes})")

# Unpack RGB565
pixels = np.frombuffer(raw, dtype=np.uint16)
r8 = ((pixels >> 11) & 0x1F) << 3
g8 = ((pixels >>  5) & 0x3F) << 2
b8 = ( pixels        & 0x1F) << 3

r8 = r8.reshape(H, W)
g8 = g8.reshape(H, W)
b8 = b8.reshape(H, W)

# ── Verification ──────────────────────────────────────────────
errors = 0
for y in range(H):
    for x in range(W):
        expected_r = int(x * 255 / (W - 1)) & 0xF8  # mask to 5-bit precision
        expected_g = int(y * 255 / (H - 1)) & 0xFC  # mask to 6-bit precision

        got_r = r8[y, x]
        got_g = g8[y, x]

        if abs(int(got_r) - expected_r) > 8 or abs(int(got_g) - expected_g) > 8:
            if errors < 20:   # print first 20 only
                print(f"ERROR at ({x},{y}): "
                      f"R got={got_r} expected={expected_r}  "
                      f"G got={got_g} expected={expected_g}")
            errors += 1

print(f"\nTotal errors: {errors} / {W*H} pixels")

# ── Save what was received for visual inspection ───────────────
from PIL import Image
out = np.stack([r8, g8, b8], axis=2).astype(np.uint8)
Image.fromarray(out).save("received.png")
print("Saved received.png")