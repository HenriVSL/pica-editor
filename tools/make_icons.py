#!/usr/bin/env python3
"""Generate res/toolbar.bmp (15 buttons, 16x16 each, 16 colours, magenta = transparent) and res/pica.ico
(32x32 + 16x16, 16 colours). Deterministic; writes 4-bpp files by hand (Pillow is only used for drawing).
T00 placeholder art (simple shapes); T07 improves it.   python3 tools/make_icons.py
"""
import os
import struct
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# standard Windows 16-colour palette (index order of the VGA palette)
PAL = [(0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0), (0, 0, 128), (128, 0, 128), (0, 128, 128),
       (192, 192, 192), (128, 128, 128), (255, 0, 0), (0, 255, 0), (255, 255, 0), (0, 0, 255), (255, 0, 255),
       (0, 255, 255), (255, 255, 255)]
BLACK, MAROON, GREEN, OLIVE, NAVY, PURPLE, TEAL, SILVER, GRAY, RED, LIME, YELLOW, BLUE, MAGENTA, AQUA, WHITE = PAL

FONT3x5 = {  # tiny digits/letters for the hex button
    '0': ['111', '101', '101', '101', '111'], 'F': ['111', '100', '110', '100', '100'],
    '1': ['010', '110', '010', '010', '111'], 'A': ['010', '101', '111', '101', '101'],
}


def text3x5(d, x, y, s, col):
    for ch in s:
        for r, row in enumerate(FONT3x5[ch]):
            for c, bit in enumerate(row):
                if bit == '1':
                    d.point((x + c, y + r), fill=col)
        x += 4


def page(d, x0, y0, x1, y1, fold=3):
    d.polygon([(x0, y0), (x1 - fold, y0), (x1, y0 + fold), (x1, y1), (x0, y1)], fill=WHITE, outline=GRAY)
    d.line([(x1 - fold, y0), (x1 - fold, y0 + fold), (x1, y0 + fold)], fill=GRAY)


def floppy(d, x, y, body=NAVY):
    d.rectangle([x, y, x + 11, y + 11], fill=body)
    d.rectangle([x + 2, y, x + 9, y + 4], fill=SILVER)
    d.rectangle([x + 7, y + 1, x + 8, y + 3], fill=body)
    d.rectangle([x + 2, y + 7, x + 9, y + 11], fill=WHITE)


def magnifier(d, x, y, glass=AQUA):
    d.ellipse([x, y, x + 7, y + 7], fill=glass, outline=BLACK)
    d.line([(x + 7, y + 7), (x + 10, y + 10)], fill=BLACK, width=2)


def arrow(d, right):
    if right:
        d.arc([2, 4, 12, 14], 180, 360, fill=BLUE, width=2)
        d.polygon([(10, 9), (14, 9), (12, 12)], fill=BLUE)
    else:
        d.arc([3, 4, 13, 14], 180, 360, fill=BLUE, width=2)
        d.polygon([(1, 9), (5, 9), (3, 12)], fill=BLUE)


def buttons():
    imgs = []
    for i in range(15):
        im = Image.new('RGB', (16, 16), MAGENTA)
        d = ImageDraw.Draw(im)
        if i == 0:      # New
            page(d, 3, 1, 12, 14)
        elif i == 1:    # Open
            d.rectangle([1, 4, 7, 6], fill=OLIVE)
            d.rectangle([1, 6, 14, 13], fill=YELLOW, outline=OLIVE)
        elif i == 2:    # Save
            floppy(d, 2, 2)
        elif i == 3:    # Save All
            floppy(d, 0, 0, PURPLE)
            floppy(d, 4, 4)
        elif i == 4:    # Cut
            d.ellipse([2, 9, 6, 13], outline=BLACK)
            d.ellipse([9, 9, 13, 13], outline=BLACK)
            d.line([(5, 9), (10, 2)], fill=BLACK)
            d.line([(10, 9), (5, 2)], fill=BLACK)
        elif i == 5:    # Copy
            page(d, 1, 1, 9, 11)
            page(d, 6, 4, 14, 14)
        elif i == 6:    # Paste
            d.rectangle([2, 2, 12, 14], fill=OLIVE)
            d.rectangle([5, 1, 9, 3], fill=SILVER, outline=BLACK)
            page(d, 6, 6, 14, 15, 2)
        elif i == 7:    # Undo
            arrow(d, False)
        elif i == 8:    # Redo
            arrow(d, True)
        elif i == 9:    # Find
            magnifier(d, 2, 2)
        elif i == 10:   # Replace
            page(d, 1, 1, 10, 14)
            d.line([(3, 5), (8, 5)], fill=GRAY)
            d.line([(3, 8), (8, 8)], fill=GRAY)
            d.line([(8, 14), (14, 6)], fill=RED, width=2)
        elif i == 11:   # Find in Files
            d.rectangle([0, 6, 11, 14], fill=YELLOW, outline=OLIVE)
            magnifier(d, 5, 1)
        elif i == 12:   # Hex view
            d.rectangle([0, 3, 15, 12], fill=WHITE, outline=NAVY)
            text3x5(d, 1, 5, '0F', NAVY)
            text3x5(d, 9, 5, 'A1', RED)
        elif i == 13:   # Word wrap
            for yy in (3, 7):
                d.line([(1, yy), (13, yy)], fill=BLACK)
            d.line([(13, 7), (13, 11)], fill=BLUE)
            d.line([(13, 11), (5, 11)], fill=BLUE)
            d.polygon([(4, 11), (7, 8), (7, 14)], fill=BLUE)
        elif i == 14:   # Show whitespace (pilcrow)
            d.ellipse([3, 2, 9, 8], fill=BLACK)
            d.line([(8, 2), (8, 14)], fill=BLACK)
            d.line([(11, 2), (11, 14)], fill=BLACK)
            d.line([(8, 2), (13, 2)], fill=BLACK)
        imgs.append(im)
    return imgs


def to_indices(im):
    w, h = im.size
    px = im.load()
    out = []
    for y in range(h):
        row = []
        for x in range(w):
            c = px[x, y]
            best = min(range(16), key=lambda k: sum((a - b) ** 2 for a, b in zip(PAL[k], c)))
            row.append(best)
        out.append(row)
    return out


def pack4(row):
    b = bytearray()
    for x in range(0, len(row), 2):
        hi = row[x]
        lo = row[x + 1] if x + 1 < len(row) else 0
        b.append((hi << 4) | lo)
    while len(b) % 4:
        b.append(0)
    return bytes(b)


def palette_bytes():
    return b''.join(struct.pack('<BBBB', c[2], c[1], c[0], 0) for c in PAL)


def write_bmp4(path, idx):
    h = len(idx)
    w = len(idx[0])
    rows = b''.join(pack4(r) for r in reversed(idx))
    info = struct.pack('<IiiHHIIiiII', 40, w, h, 1, 4, 0, len(rows), 2835, 2835, 16, 16)
    off = 14 + 40 + 64
    hdr = struct.pack('<2sIHHI', b'BM', off + len(rows), 0, 0, off)
    with open(path, 'wb') as f:
        f.write(hdr + info + palette_bytes() + rows)


def icon_image(size):
    im = Image.new('RGB', (size, size), MAGENTA)
    d = ImageDraw.Draw(im)
    if size == 32:
        page(d, 4, 1, 27, 30, 6)
        d.rectangle([4, 1, 21, 6], fill=NAVY)
        for yy in range(10, 28, 4):
            d.line([(8, yy), (14, yy)], fill=GRAY)
        d.rectangle([16, 10, 18, 26], fill=RED)            # big "P"
        d.rectangle([16, 10, 23, 12], fill=RED)
        d.rectangle([22, 10, 24, 18], fill=RED)
        d.rectangle([16, 17, 23, 19], fill=RED)
    else:
        page(d, 2, 0, 13, 15, 3)
        d.rectangle([2, 0, 10, 2], fill=NAVY)
        d.rectangle([7, 5, 8, 13], fill=RED)
        d.rectangle([7, 5, 11, 6], fill=RED)
        d.rectangle([11, 5, 12, 9], fill=RED)
        d.rectangle([7, 9, 11, 10], fill=RED)
    return im


def ico_entry(im):
    idx = to_indices(im)
    size = len(idx)
    mask_rows = []
    xor_idx = []
    for r in idx:
        bits = 0
        mrow = bytearray()
        xr = []
        for x, k in enumerate(r):
            transparent = (k == 13)   # magenta
            xr.append(0 if transparent else k)
            bits = (bits << 1) | (1 if transparent else 0)
            if x % 8 == 7:
                mrow.append(bits)
                bits = 0
        while len(mrow) % 4:
            mrow.append(0)
        mask_rows.append(bytes(mrow))
        xor_idx.append(xr)
    xor = b''.join(pack4(r) for r in reversed(xor_idx))
    andm = b''.join(reversed(mask_rows))
    info = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 4, 0, len(xor) + len(andm), 0, 0, 16, 0)
    return size, info + palette_bytes() + xor + andm


def write_ico(path, images):
    entries = [ico_entry(im) for im in images]
    off = 6 + 16 * len(entries)
    out = struct.pack('<HHH', 0, 1, len(entries))
    data = b''
    for size, blob in entries:
        out += struct.pack('<BBBBHHII', size, size, 16, 0, 1, 4, len(blob), off + len(data))
        data += blob
    with open(path, 'wb') as f:
        f.write(out + data)


def main():
    strip = Image.new('RGB', (16 * 15, 16), MAGENTA)
    for i, im in enumerate(buttons()):
        strip.paste(im, (16 * i, 0))
    write_bmp4(os.path.join(ROOT, 'res', 'toolbar.bmp'), to_indices(strip))
    write_ico(os.path.join(ROOT, 'res', 'pica.ico'), [icon_image(32), icon_image(16)])
    print('wrote res/toolbar.bmp and res/pica.ico')


if __name__ == '__main__':
    main()
