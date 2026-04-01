#!/usr/bin/env python3
"""
receive_verify.py
Receives the test image over UART and verifies every pixel's position encoding.
Built on the working serial pattern — DTR/RTS held low to prevent ESP reset.
"""

import serial
import struct
import numpy as np
import cv2

PORT  = "COM5"
BAUD  = 921600
MAGIC = 0xDEADBEEF

# ── 1. Open port without toggling DTR/RTS ─────────────────────────────────────
ser = serial.Serial()
ser.port      = PORT
ser.baudrate  = BAUD
ser.timeout   = 30
ser.dtr       = False
ser.rts       = False
ser.open()
ser.reset_input_buffer()

# ── 2. Wait for ESP READY marker, then trigger ────────────────────────────────
print("[+] Waiting for ESP READY...")
while True:
    line = ser.readline().decode(errors='replace').strip()
    print(f"  ESP: {line}")
    if "READY" in line or "trigger" in line.lower():
        break

print("[+] Sending trigger...")
ser.write(b'\x01')
ser.flush()

# ── Helpers ───────────────────────────────────────────────────────────────────
def read_exact(ser, size):
    data = b''
    while len(data) < size:
        chunk = ser.read(size - len(data))
        if not chunk:
            raise TimeoutError(f"Stalled: got {len(data)}/{size} bytes")
        data += chunk
        print(f"  Progress: {len(data)}/{size} bytes ({100*len(data)//size}%)", end='\r')
    print()
    return data

def find_header(ser):
    buf = b''
    print("[+] Scanning for magic...")
    while True:
        b = ser.read(1)
        if not b:
            continue
        buf += b
        if len(buf) >= 4:
            val = struct.unpack("<I", buf[-4:])[0]
            if val == MAGIC:
                rest = read_exact(ser, 9 - 4)   # IHHB = 4+2+2+1 = 9 bytes
                print("  MAGIC found")
                return buf[-4:] + rest

# ── 3. Read header + pixel data ───────────────────────────────────────────────
header = find_header(ser)
magic, width, height, fmt = struct.unpack("<IHHB", header)
print(f"[+] Header: {width}x{height}  format={fmt}")

img_bytes = width * height * 2
print(f"[+] Reading {img_bytes} bytes...")
data = read_exact(ser, img_bytes)
print(f"[+] Received {len(data)} bytes")

# ── 4. Decode RGB565 ──────────────────────────────────────────────────────────
raw  = np.frombuffer(data, dtype=np.uint16).reshape((height, width))
r8   = (((raw >> 11) & 0x1F) << 3).astype(np.int32)
g8   = (((raw >>  5) & 0x3F) << 2).astype(np.int32)
b8   = (( raw        & 0x1F) << 3).astype(np.int32)

# Save what arrived — visually inspect this first
cv2.imwrite("received.png", np.dstack((b8, g8, r8)).astype(np.uint8))
print("[+] Saved received.png")

# ── 5. Position verification ──────────────────────────────────────────────────
#
# Expected values at each pixel (accounting for RGB565 precision loss):
#   R encodes X:  expected_r = round(x * 255 / (W-1))  masked to 5-bit (& 0xF8)
#   G encodes Y:  expected_g = round(y * 255 / (H-1))  masked to 6-bit (& 0xFC)
#   B should be 0 (JPEG compression may push it slightly above 0)
#
# Tolerance of 8 covers the rounding from JPEG compression at quality=95.
# If you see errors > tolerance, increase quality in generate_test.py first.

TOLERANCE = 8
W, H = width, height

xs = np.arange(W, dtype=np.float32)
ys = np.arange(H, dtype=np.float32)

exp_r = (np.round(xs * 255 / (W - 1)).astype(np.int32) & 0xF8)   # shape (W,)
exp_g = (np.round(ys * 255 / (H - 1)).astype(np.int32) & 0xFC)   # shape (H,)

# Broadcast to full grid
exp_r_grid = np.broadcast_to(exp_r[np.newaxis, :], (H, W))        # (H, W)
exp_g_grid = np.broadcast_to(exp_g[:, np.newaxis], (H, W))        # (H, W)

err_r = np.abs(r8 - exp_r_grid)
err_g = np.abs(g8 - exp_g_grid)
err_b = b8   # should be ~0

bad_r = err_r > TOLERANCE
bad_g = err_g > TOLERANCE
bad_b = err_b > 20          # JPEG can push B slightly above 0

total_errors = int(np.sum(bad_r | bad_g | bad_b))
print(f"\n[+] Pixel errors: {total_errors} / {W*H}")

# ── 6. Diagnosis ──────────────────────────────────────────────────────────────
if total_errors == 0:
    print("    All pixels correct.")
else:
    # Print first 20 bad pixels
    ys_bad, xs_bad = np.where(bad_r | bad_g | bad_b)
    print(f"\n    First bad pixels (x, y, got_r, exp_r, got_g, exp_g, got_b):")
    for i in range(min(20, len(xs_bad))):
        x, y = int(xs_bad[i]), int(ys_bad[i])
        print(f"      ({x:3d},{y:3d})  "
              f"R got={r8[y,x]:3d} exp={exp_r_grid[y,x]:3d}  "
              f"G got={g8[y,x]:3d} exp={exp_g_grid[y,x]:3d}  "
              f"B got={b8[y,x]:3d}")

    # ── Pattern analysis ──────────────────────────────────────────────────────
    print("\n[+] Pattern analysis:")

    # Are X and Y channels swapped? (R encodes Y, G encodes X)
    swap_r = np.abs(r8 - exp_g_grid)
    swap_g = np.abs(g8 - exp_r_grid)
    if np.mean(swap_r | swap_g) < np.mean(err_r | err_g):
        print("    LIKELY: R and G channels swapped — X/Y axes inverted")
        print("    Check: RGB565 bit extraction order")

    # Are rows arriving in the wrong order? Check if image is vertically flipped.
    r8_flip = np.flipud(r8)
    g8_flip = np.flipud(g8)
    if np.mean(np.abs(g8_flip - exp_g_grid)) < np.mean(err_g):
        print("    LIKELY: Image is vertically flipped — rows arriving bottom-up")
        print("    Check: roi_y calculation or pan_y centering sign")

    # Is there a constant horizontal shift?
    # If every pixel is shifted by N columns, err_r will be uniform across rows.
    row_mean_err_r = np.mean(err_r, axis=0)   # error per column
    if np.std(row_mean_err_r) < 5 and np.mean(row_mean_err_r) > TOLERANCE:
        print("    LIKELY: Constant horizontal offset (ROI left edge wrong)")
        print("    Check: pan_x / ROI left calculation")

    # Is there a constant vertical shift?
    col_mean_err_g = np.mean(err_g, axis=1)   # error per row
    if np.std(col_mean_err_g) < 5 and np.mean(col_mean_err_g) > TOLERANCE:
        print("    LIKELY: Constant vertical offset (ROI top edge wrong)")
        print("    Check: pan_y / ROI top calculation")

    # Are only certain rows wrong? (MCU boundary issue)
    row_error_count = np.sum(bad_r | bad_g | bad_b, axis=1)   # errors per row
    bad_rows = np.where(row_error_count > W // 2)[0]
    if len(bad_rows) > 0 and len(bad_rows) < H // 2:
        print(f"    LIKELY: Specific rows corrupted: {bad_rows[:10].tolist()}")
        diffs = np.diff(bad_rows)
        if len(diffs) > 0 and np.all(diffs == diffs[0]):
            print(f"    Pattern repeats every {diffs[0]} rows — MCU height mismatch?")
        print("    Check: output_func row accumulation, row_flushed[] indexing")

    # Is blue channel high everywhere? (format/byte order issue)
    if np.mean(b8) > 20:
        print(f"    LIKELY: Blue channel high (mean={np.mean(b8):.1f}) — byte order wrong")
        print("    Check: RGB565 endianness — try swapping bytes in raw[]")

    # Save an error heatmap for visual diagnosis
    heatmap = np.zeros((H, W, 3), dtype=np.uint8)
    heatmap[:,:,2] = np.clip(err_r * 4, 0, 255).astype(np.uint8)   # R errors → red
    heatmap[:,:,1] = np.clip(err_g * 4, 0, 255).astype(np.uint8)   # G errors → green
    heatmap[:,:,0] = np.clip(b8,        0, 255).astype(np.uint8)   # B value  → blue
    cv2.imwrite("error_heatmap.png", heatmap)
    print("\n[+] Saved error_heatmap.png")
    print("    Red   = wrong X position")
    print("    Green = wrong Y position")
    print("    Blue  = unexpected blue channel (corruption)")