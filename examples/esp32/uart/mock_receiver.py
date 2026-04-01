#!/usr/bin/env python3
"""
receive_mock.py
Receives the mock pattern from mock_sender.c and verifies every pixel exactly.
No JPEG, no tolerance — every byte is deterministic.
"""

import serial
import struct
import numpy as np
import cv2

PORT  = "COM5"
BAUD  = 921600
MAGIC = 0xDEADBEEF
W, H  = 320, 240

# ── Open port ─────────────────────────────────────────────────────────────────
ser = serial.Serial()
ser.port     = PORT
ser.baudrate = BAUD
ser.timeout  = 30
ser.dtr      = False
ser.rts      = False
ser.open()

# ── Wait for READY, flush, then trigger ───────────────────────────────────────
print("[+] Waiting for READY...")
while True:
    line = ser.readline().decode(errors='replace').strip()
    print(f"  ESP: {line}")
    if "READY" in line:
        break

ser.reset_input_buffer()   # flush BEFORE trigger

print("[+] Sending trigger...")
ser.write(b'\x01')
ser.flush()

# ── Helpers ───────────────────────────────────────────────────────────────────
def read_exact(ser, n):
    buf = b''
    while len(buf) < n:
        chunk = ser.read(n - len(buf))
        if not chunk:
            raise TimeoutError(f"Stalled at {len(buf)}/{n}")
        buf += chunk
        print(f"  {len(buf)}/{n} bytes", end='\r')
    print()
    return buf

def find_header(ser):
    buf = b''
    print("[+] Scanning for magic...")
    while True:
        b = ser.read(1)
        if not b:
            continue
        buf += b
        if len(buf) >= 4:
            if struct.unpack("<I", buf[-4:])[0] == MAGIC:
                rest = read_exact(ser, 5)   # HHB = 2+2+1
                print("  MAGIC found")
                return buf[-4:] + rest

# ── Read header ───────────────────────────────────────────────────────────────
header = find_header(ser)
magic, width, height, fmt = struct.unpack("<IHHB", header)
print(f"[+] Header: {width}x{height}  fmt={fmt}")
assert width == W and height == H, f"Unexpected size {width}x{height}"

# ── Read pixels ───────────────────────────────────────────────────────────────
total = width * height * 2
print(f"[+] Reading {total} bytes...")
data = read_exact(ser, total)
print(f"[+] Got {len(data)} bytes")

pixels = np.frombuffer(data, dtype=np.uint16).reshape((H, W))

# ── Exact verification ────────────────────────────────────────────────────────
# Reconstruct expected grid — same formula as mock_sender.c
xs = np.arange(W, dtype=np.uint16)
ys = np.arange(H, dtype=np.uint16)

r5_grid = (xs[np.newaxis, :] // 10) & 0x1F   # (1, W) broadcast
g6_grid = (ys[:, np.newaxis] //  4) & 0x3F   # (H, 1) broadcast
expected = ((r5_grid << 11) | (g6_grid << 5)).astype(np.uint16)

diff = pixels.astype(np.int32) - expected.astype(np.int32)
errors = np.sum(diff != 0)

print(f"\n[+] Pixel errors: {errors} / {W*H}")

if errors == 0:
    print("    PERFECT — every byte correct. Transport is clean.")
else:
    bad_y, bad_x = np.where(diff != 0)
    print(f"\n    First bad pixels:")
    for i in range(min(20, len(bad_x))):
        x, y = int(bad_x[i]), int(bad_y[i])
        got  = int(pixels[y, x])
        exp  = int(expected[y, x])
        print(f"      ({x:3d},{y:3d})  got=0x{got:04X}  exp=0x{exp:04X}  "
              f"diff={got-exp:+d}")

    # ── Pattern analysis ──────────────────────────────────────────────────────
    print("\n[+] Pattern analysis:")

    # Check if stream is shifted by a fixed byte offset
    # If every pixel is wrong by the same amount, it's a shift
    flat_got = pixels.flatten().astype(np.int32)
    flat_exp = expected.flatten().astype(np.int32)

    # Try byte shifts 1..8 and see if any alignment fixes it
    raw = np.frombuffer(data, dtype=np.uint8)
    print("    Testing byte shift alignment...")
    for shift in range(1, 9):
        if len(raw) - shift < W * H * 2:
            break
        try_pixels = np.frombuffer(raw[shift:shift + W*H*2],
                                   dtype=np.uint16).reshape((H, W))
        try_diff   = np.sum(try_pixels.astype(np.int32)
                            - expected.astype(np.int32) != 0)
        print(f"      shift={shift:2d} byte(s): {try_diff} errors", end='')
        if try_diff < errors * 0.1:
            print("  ← SIGNIFICANT IMPROVEMENT")
        else:
            print()

    # Are errors uniform per row? (whole rows wrong vs scattered)
    row_err = np.sum(diff != 0, axis=1)
    all_bad_rows  = np.where(row_err == W)[0]
    all_good_rows = np.where(row_err == 0)[0]
    print(f"\n    Rows fully correct:  {len(all_good_rows)}")
    print(f"    Rows fully wrong:    {len(all_bad_rows)}")
    print(f"    Rows partially wrong: {H - len(all_good_rows) - len(all_bad_rows)}")

    if len(all_good_rows) > 0 and len(all_bad_rows) > 0:
        print(f"    First good row: {all_good_rows[0]}")
        print(f"    First bad row:  {all_bad_rows[0]}")
        if all_bad_rows[0] < all_good_rows[0]:
            print("    → Bad rows come BEFORE good rows: stream start misaligned")
        else:
            print("    → Good rows come first, then bad: mid-stream corruption")

    # Save received image for visual inspection
    r8 = (((pixels >> 11) & 0x1F) << 3).astype(np.uint8)
    g8 = (((pixels >>  5) & 0x3F) << 2).astype(np.uint8)
    b8 = (( pixels        & 0x1F) << 3).astype(np.uint8)
    cv2.imwrite("mock_received.png",
                np.dstack((b8, g8, r8)))
    print("\n[+] Saved mock_received.png")

    # Save expected for comparison
    r8e = (((expected >> 11) & 0x1F) << 3).astype(np.uint8)
    g8e = (((expected >>  5) & 0x3F) << 2).astype(np.uint8)
    b8e = np.zeros_like(r8e)
    cv2.imwrite("mock_expected.png",
                np.dstack((b8e, g8e, r8e)))
    print("[+] Saved mock_expected.png")