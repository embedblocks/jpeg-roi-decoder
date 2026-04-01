# generate_test.py
from PIL import Image

W, H = 320, 240
img = Image.new("RGB", (W, H))

for y in range(H):
    for x in range(W):
        # R encodes x (0-255 scaled from 0-319)
        # G encodes y (0-255 scaled from 0-239)
        # B = 0 always — any blue means corruption
        r = int(x * 255 / (W - 1))
        g = int(y * 255 / (H - 1))
        img.putpixel((x, y), (r, g, 0))

img.save("test.jpg", quality=95)