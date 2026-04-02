#!/usr/bin/env python3

import serial
import struct
import numpy as np
import cv2
import time

PORT  = "COM5"
MAGIC = 0xDEADBEEF

# ── 1. Open at 115200 (handshake baud) ───────────────────────────────────────
ser = serial.Serial(PORT, 115200, timeout=30)
ser.dtr = False
ser.rts = False

# ── 2. Wait for READY ─────────────────────────────────────────────────────────
print("[1] Waiting for ESP READY at 115200...")
while True:
    line = ser.readline().decode(errors='replace').strip()
    if line:
        print(f"  ESP: {line}")
    if "READY" in line:
        break

ser.reset_input_buffer()

# ── 3. Send trigger ───────────────────────────────────────────────────────────
print("[2] Sending trigger...")
ser.write(b'\x01')
ser.flush()

# Wait for trigger acknowledgement (optional but good)
ser.timeout = 5
for _ in range(10):
    line = ser.readline().decode(errors='replace').strip()
    if line:
        print(f"  ESP: {line}")
    if "Trigger" in line:
        print("  ✅ Trigger acknowledged")
        break

# ── 4. Switch to high-speed baud ──────────────────────────────────────────────
time.sleep(0.3)                 # allow ESP to switch cleanly
ser.baudrate = 921600           # IMPORTANT: do NOT reopen port
ser.reset_input_buffer()

print("[3] Switched to 921600 — scanning for header...")

# ── Helpers ───────────────────────────────────────────────────────────────────
def read_exact(ser, size):
    data = b''
    while len(data) < size:
        chunk = ser.read(size - len(data))
        if not chunk:
            raise TimeoutError(f"Timeout: got {len(data)}/{size}")
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
img_bytes = width * height * 2
print(f"[5] Reading {img_bytes} bytes...")

data = read_exact(ser, img_bytes)

print(f"[6] Received {len(data)} bytes")

# ── 7. Decode RGB565 ──────────────────────────────────────────────────────────
raw = np.frombuffer(data, dtype=np.uint16).reshape((height, width))

r8 = (((raw >> 11) & 0x1F) << 3).astype(np.int32)
g8 = (((raw >> 5)  & 0x3F) << 2).astype(np.int32)
b8 = (( raw        & 0x1F) << 3).astype(np.int32)

cv2.imwrite("received.png", np.dstack((b8, g8, r8)).astype(np.uint8))
print("[+] Saved received.png")

# ── 8. Pixel verification (your original logic) ───────────────────────────────
TOLERANCE = 8
W, H = width, height

xs = np.arange(W, dtype=np.float32)
ys = np.arange(H, dtype=np.float32)

exp_r = (np.round(xs * 255 / (W - 1)).astype(np.int32) & 0xF8)
exp_g = (np.round(ys * 255 / (H - 1)).astype(np.int32) & 0xFC)

exp_r_grid = np.broadcast_to(exp_r[np.newaxis, :], (H, W))
exp_g_grid = np.broadcast_to(exp_g[:, np.newaxis], (H, W))

err_r = np.abs(r8 - exp_r_grid)
err_g = np.abs(g8 - exp_g_grid)
err_b = b8

bad = (err_r > TOLERANCE) | (err_g > TOLERANCE) | (err_b > 20)

total_errors = int(np.sum(bad))
print(f"[7] Pixel errors: {total_errors}/{W*H}")

if total_errors == 0:
    print("    ✅ All pixels correct.")
else:
    print("    ❌ Errors detected")

    heatmap = np.zeros((H, W, 3), dtype=np.uint8)
    heatmap[:,:,2] = np.clip(err_r * 4, 0, 255)
    heatmap[:,:,1] = np.clip(err_g * 4, 0, 255)
    heatmap[:,:,0] = np.clip(b8,        0, 255)

    cv2.imwrite("error_heatmap.png", heatmap)
    print("[+] Saved error_heatmap.png")

