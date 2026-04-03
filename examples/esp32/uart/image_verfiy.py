#!/usr/bin/env python3
# receive_fixed.py

import serial
import struct
import numpy as np
import time
from PIL import Image

PORT  = "COM5"
MAGIC = 0xDEADBEEF

# ── 1. Open at 115200 (handshake phase) ──────────────────────────────────────
ser = serial.Serial(PORT, 115200, timeout=30)
ser.dtr = False
ser.rts = False

# ── 2. Wait for READY ─────────────────────────────────────────────────────────
print("[1] Waiting for READY...")
while True:
    line = ser.readline().decode(errors='replace').strip()
    if line:
        print(f"  ESP: {line}")
    if "READY" in line:
        break

ser.reset_input_buffer()

# ── 3. Trigger ESP ────────────────────────────────────────────────────────────
print("[2] Sending trigger...")
ser.write(b'\x01')
ser.flush()

# Optional: confirm trigger
ser.timeout = 5
for _ in range(10):
    line = ser.readline().decode(errors='replace').strip()
    if line:
        print(f"  ESP: {line}")
    if "Trigger" in line:
        print("  ✅ Trigger acknowledged")
        break

# ── 4. Switch to high-speed baud ──────────────────────────────────────────────
time.sleep(0.3)
ser.baudrate = 921600
ser.reset_input_buffer()

print("[3] Switched to 921600 — scanning for header...")

# ── Helpers ───────────────────────────────────────────────────────────────────
def read_exact(ser, size):
    data = b''
    while len(data) < size:
        chunk = ser.read(size - len(data))
        if not chunk:
            raise TimeoutError(f"Timeout: {len(data)}/{size}")
        data += chunk
        print(f"  {len(data)}/{size}", end='\r')
    print()
    return data

def find_header(ser):
    buf = b''
    while True:
        b = ser.read(1)
        if not b:
            raise TimeoutError("No MAGIC received")
        buf += b
        if len(buf) >= 4:
            if struct.unpack("<I", buf[-4:])[0] == MAGIC:
                rest = read_exact(ser, 5)
                print("[+] MAGIC found")
                return buf[-4:] + rest

# ── 5. Read header ────────────────────────────────────────────────────────────
header = find_header(ser)
magic, width, height, fmt = struct.unpack("<IHHB", header)

print(f"[4] Header: {width}x{height}, fmt={fmt}")

# ── 6. Read image data ────────────────────────────────────────────────────────
total_bytes = width * height * 2
print(f"[5] Reading {total_bytes} bytes...")

raw = read_exact(ser, total_bytes)

print(f"[6] Received {len(raw)} bytes")

# ── 7. Decode RGB565 ──────────────────────────────────────────────────────────
pixels = np.frombuffer(raw, dtype=np.uint16)

r8 = ((pixels >> 11) & 0x1F) << 3
g8 = ((pixels >>  5) & 0x3F) << 2
b8 = ( pixels        & 0x1F) << 3

r8 = r8.reshape(height, width)
g8 = g8.reshape(height, width)
b8 = b8.reshape(height, width)

# ── 8. Verification (your original loop style preserved) ──────────────────────
errors = 0
for y in range(height):
    for x in range(width):
        expected_r = int(x * 255 / (width - 1)) & 0xF8
        expected_g = int(y * 255 / (height - 1)) & 0xFC

        got_r = int(r8[y, x])
        got_g = int(g8[y, x])

        if abs(got_r - expected_r) > 8 or abs(got_g - expected_g) > 8:
            if errors < 20:
                print(f"ERROR at ({x},{y}): "
                      f"R got={got_r} expected={expected_r}  "
                      f"G got={got_g} expected={expected_g}")
            errors += 1

print(f"\n[7] Total errors: {errors} / {width*height} pixels")

# ── 9. Save image ─────────────────────────────────────────────────────────────
out = np.stack([r8, g8, b8], axis=2).astype(np.uint8)
Image.fromarray(out).save("received.png")
print("[+] Saved received.png")

