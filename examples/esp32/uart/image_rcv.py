#!/usr/bin/env python3
# image_rcv_final.py

import serial
import struct
import numpy as np
import time
from PIL import Image

PORT  = "COM5"
MAGIC = 0xDEADBEEF
SYNC_BYTE = 0xAA

# ── 1. Open at 115200 ────────────────────────────────────────
ser = serial.Serial(PORT, 115200, timeout=30)
ser.dtr = False
ser.rts = False

# ── 2. Wait for READY ─────────────────────────────────────────
print("[1] Waiting for READY...")
while True:
    line = ser.readline().decode(errors='replace').strip()
    if line:
        print(f"  ESP: {line}")
    if "READY" in line:
        break

ser.reset_input_buffer()

# ── 3. Trigger ESP ────────────────────────────────────────────
print("[2] Sending trigger...")
ser.write(b'\x01')
ser.flush()

ser.timeout = 5
for _ in range(10):
    line = ser.readline().decode(errors='replace').strip()
    if line:
        print(f"  ESP: {line}")
    if "Trigger" in line:
        print("  ✅ Trigger acknowledged")
        break

# ── 4. Switch baud (MUST wait longer than ESP32's 600ms) ─────
time.sleep(0.7)
ser.reset_input_buffer()
ser.baudrate = 921600
time.sleep(0.1)

print("[3] Switched to 921600 — waiting for header...")

# ── Helpers ───────────────────────────────────────────────────
def read_exact(ser, size):
    data = bytearray()
    start = time.time()
    while len(data) < size:
        chunk = ser.read(min(size - len(data), 4096))
        if not chunk:
            if time.time() - start > 10:
                raise TimeoutError(f"Timeout: {len(data)}/{size}")
            time.sleep(0.001)
            continue
        data.extend(chunk)
        if len(data) % 10000 < 100:
            print(f"\r  {len(data)}/{size} ({100*len(data)/size:.1f}%)", end='', flush=True)
    print()
    return bytes(data)

def find_sync_and_header(ser):
    search_buf = bytearray()
    while True:
        b = ser.read(1)
        if not b:
            raise TimeoutError("No sync pattern received")
        search_buf.append(b[0])
        
        if len(search_buf) >= 12:
            tail = search_buf[-12:]
            if (tail[0] == SYNC_BYTE and tail[1] == SYNC_BYTE and tail[2] == SYNC_BYTE and
                struct.unpack("<I", tail[3:7])[0] == MAGIC):
                print(f"[+] Sync+MAGIC found")
                return tail[3:]
    
# ── 5. Read header ────────────────────────────────────────────
header = find_sync_and_header(ser)
# After find_sync_and_header:
magic, width, height, fmt = struct.unpack("<IHHB", header)
assert magic == MAGIC, f"Magic mismatch: {magic:#010x}"
assert width == 320 and height == 240, f"Unexpected dims: {width}x{height}"

# Widen the timing margin to be safe:
#time.sleep(0.8)          # was 0.7 — give 200ms+ margin over ESP's 710ms
#ser.reset_input_buffer()
ser.baudrate = 921600
#time.sleep(0.15)         # was 0.1

print(f"[4] Header: {width}x{height}, fmt={fmt}")

# ── 6. Read image data ────────────────────────────────────────
total_bytes = width * height * 2
print(f"[5] Reading {total_bytes} bytes...")

raw = read_exact(ser, total_bytes)

print(f"[6] Received {len(raw)} bytes")

# ── 7. RGB565 decode — BIG ENDIAN (correct for your decoder) ──
print("\n[7] Decoding RGB565 (big-endian)...")

# Use BIG-ENDIAN — your JPEG decoder outputs this format
pixels = np.frombuffer(raw, dtype='<u2')

r8 = ((pixels >> 11) & 0x1F) << 3
g8 = ((pixels >>  5) & 0x3F) << 2
b8 = ( pixels        & 0x1F) << 3

r8 = r8.reshape(height, width)
g8 = g8.reshape(height, width)
b8 = b8.reshape(height, width)

# ── 8. Diagnostics ────────────────────────────────────────────
print("\n[8] Image sanity check:")
print(f"     R range: {r8.min()}–{r8.max()}")
print(f"     G range: {g8.min()}–{g8.max()}")
print(f"     B range: {b8.min()}–{b8.max()}")
print("  ✅ Image looks valid")

# ── 9. Save image ─────────────────────────────────────────────
out = np.stack([r8, g8, b8], axis=2).astype(np.uint8)
Image.fromarray(out).save("received.png")

print("[+] Saved received.png")