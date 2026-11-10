#!/usr/bin/env python3
"""Generate res/toolbar.bmp (15 buttons, 16x16 each, 16 colours, magenta = transparent) and res/pica.ico
(32x32 + 16x16, 16 colours). Deterministic; writes 4-bpp files by hand (no Pillow needed for the files).
Art is hand-made pixel grids in the classic Win98 toolbar style (black outlines, VGA palette).

    python3 tools/make_icons.py            writes res/toolbar.bmp and res/pica.ico
    python3 tools/make_icons.py --preview  also writes docs/shots/t07_icons.png (8x zoom, needs Pillow)
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# standard Windows 16-colour palette (index order of the VGA palette)
PAL = [(0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0), (0, 0, 128), (128, 0, 128), (0, 128, 128),
       (192, 192, 192), (128, 128, 128), (255, 0, 0), (0, 255, 0), (255, 255, 0), (0, 0, 255), (255, 0, 255),
       (0, 255, 255), (255, 255, 255)]
# grid characters -> palette index ('.' = magenta = transparent)
CH = {'k': 0, 'm': 1, 'g': 2, 'o': 3, 'n': 4, 'p': 5, 't': 6, 's': 7, 'G': 8, 'r': 9, 'l': 10, 'y': 11,
      'b': 12, '.': 13, 'a': 14, 'w': 15}
KEY = 13


class Grid:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[KEY] * w for _ in range(h)]

    def set(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y][x] = CH[c] if isinstance(c, str) else c

    def rect(self, x0, y0, x1, y1, fill=None, outline=None):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                edge = x in (x0, x1) or y in (y0, y1)
                if edge and outline:
                    self.set(x, y, outline)
                elif fill:
                    self.set(x, y, fill)

    def hline(self, x0, x1, y, c):
        for x in range(x0, x1 + 1):
            self.set(x, y, c)

    def vline(self, x, y0, y1, c):
        for y in range(y0, y1 + 1):
            self.set(x, y, c)

    def paste(self, rows, x0=0, y0=0):
        """rows: list of strings; '.' cells are skipped (transparent overlay)."""
        for y, row in enumerate(rows):
            for x, ch in enumerate(row):
                if ch != '.' and ch != ' ':
                    self.set(x0 + x, y0 + y, ch)

    def page(self, x0, y0, x1, y1, fold=3, lines=None, line_col='G'):
        """White page with a black outline and a folded top-right corner; optional text lines."""
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                if x > x1 - fold and y < y0 + fold and (x - (x1 - fold)) > (y - y0):
                    continue                                   # cut corner
                self.set(x, y, 'w')
        self.hline(x0, x1 - fold, y0, 'k')
        self.vline(x0, y0, y1, 'k')
        self.hline(x0, x1, y1, 'k')
        self.vline(x1, y0 + fold, y1, 'k')
        for i in range(fold + 1):                              # diagonal of the fold
            self.set(x1 - fold + i, y0 + i, 'k')
        self.vline(x1 - fold, y0, y0 + fold, 'k')              # fold flap
        self.hline(x1 - fold, x1, y0 + fold, 'k')
        for i in range(1, fold):
            for j in range(1, i + 1):
                self.set(x1 - fold + j, y0 + i, 's')
        if lines:
            for (lx0, lx1, ly) in lines:
                self.hline(lx0, lx1, ly, line_col)


def btn_new():
    g = Grid(16, 16)
    g.page(3, 1, 12, 14, fold=4)
    return g


def btn_open():
    g = Grid(16, 16)
    g.paste([
        "................",
        "..........kkk...",
        ".........k...k.k",
        "..............kk",
        ".kkk.........kkk",
        "kyyykkkkkkk.....",
        "kywywywywyk.....",
        "kwywywykkkkkkkkk",
        "kywywykoooooooko",
        "kwywykoooooooko.",
        "kywykoooooooko..",
        "kwykoooooooko...",
        "kykoooooooko....",
        "kkkkkkkkkkk.....",
        "................",
        "................",
    ])
    # the front flap's outline: fix the stray pixels at the right end of rows 8-12
    for y, x in ((8, 15), (9, 14), (10, 13), (11, 12), (12, 11)):
        g.set(x, y, '.')
    for y, x in ((8, 14), (9, 13), (10, 12), (11, 11), (12, 10)):
        g.set(x, y, 'k')
    return g


FLOPPY16 = [
    ".kkkkkkkkkkkkkk.",
    ".knkwwwwwwwwknk.",
    ".knkwwwwwwwwkkk.",
    ".knkwwwwwwwwknk.",
    ".knkwwwwwwwwknk.",
    ".knkwwwwwwwwknk.",
    ".knnkkkkkkkknnk.",
    ".knnnnnnnnnnnnk.",
    ".knnkkkkkkkknnk.",
    ".knnksskksssnnk.",
    ".knnksskksssnnk.",
    ".knnksskksssnnk.",
    ".kkkkkkkkkkkkkk.",
]

FLOPPY11 = [
    "kkkkkkkkkkk",
    "knkwwwwwknk",
    "knkwwwwwkkk",
    "knkwwwwwknk",
    "knnkkkkknnk",
    "knnnnnnnnnk",
    "knnkkkkknnk",
    "knnksksknnk",
    "knnksksknnk",
    "kkkkkkkkkkk",
]


def btn_save():
    g = Grid(16, 16)
    g.paste(FLOPPY16, 0, 1)
    return g


def btn_saveall():
    g = Grid(16, 16)
    back = [r.replace('n', 'p') for r in FLOPPY11]
    g.paste(back, 0, 0)
    # clear the area of the front floppy (so the back one doesn't show through its transparent parts)
    g.rect(4, 5, 14, 14, fill='w')
    g.paste(FLOPPY11, 4, 5)
    g.vline(15, 5, 14, '.')
    return g


def btn_cut():
    g = Grid(16, 16)
    g.paste([
        "................",
        "....k......k....",
        "....k......k....",
        "....kk....kk....",
        ".....k....k.....",
        ".....kk..kk.....",
        "......k..k......",
        "......kkkk......",
        ".......kk.......",
        "......n..n......",
        "...nnnn..nnnn...",
        "..n...n..n...n..",
        "..n...n..n...n..",
        "..n...n..n...n..",
        "...nnn....nnn...",
        "................",
    ])
    return g


def btn_copy():
    g = Grid(16, 16)
    g.page(0, 0, 8, 10, fold=3, lines=[(2, 4, 3), (2, 6, 5), (2, 6, 7)])
    g.page(6, 4, 15, 15, fold=3, lines=[(8, 11, 7), (8, 13, 9), (8, 13, 11), (8, 12, 13)])
    return g


def btn_paste():
    g = Grid(16, 16)
    g.rect(1, 2, 11, 14, fill='o', outline='k')
    g.rect(4, 1, 8, 3, fill='s', outline='k')
    g.page(6, 6, 15, 15, fold=3, lines=[(8, 11, 9), (8, 13, 11), (8, 12, 13)])
    return g


UNDO = [
    "................",
    "................",
    "................",
    "......nnnnn.....",
    "....nnnnnnnnn...",
    "...nnn.....nnn..",
    "..nn........nn..",
    "nnnnnn......nnn.",
    ".nnnn........nn.",
    "..nn.........nn.",
    "...........nnn..",
    ".........nnnn...",
    ".........nnn....",
    "................",
    "................",
    "................",
]


def btn_undo():
    g = Grid(16, 16)
    g.paste(UNDO)
    return g


def btn_redo():
    g = Grid(16, 16)
    g.paste([r[::-1] for r in UNDO])
    return g


MAGNIFIER = [
    "...kkkkk...",
    "..kwwwwwk..",
    ".kwwaaawwk.",
    ".kwaawwwwk.",
    ".kwawwwwwk.",
    ".kwwwwwwwk.",
    ".kwwwwwwwk.",
    "..kwwwwwkk.",
    "...kkkkkmmk",
]


def magnifier(g, x, y, handle=4):
    g.paste(MAGNIFIER, x, y)
    for i in range(handle):
        g.set(x + 9 + i, y + 8 + i, 'm')
        g.set(x + 10 + i, y + 8 + i, 'm')
        g.set(x + 9 + i, y + 9 + i, 'k')
        g.set(x + 11 + i, y + 8 + i, 'k')


def btn_find():
    g = Grid(16, 16)
    magnifier(g, 0, 1, handle=5)
    return g


def btn_replace():
    g = Grid(16, 16)
    g.page(0, 0, 9, 13, fold=3, lines=[(2, 5, 3), (2, 7, 5), (2, 7, 7), (2, 6, 9), (2, 7, 11)])
    # red pencil from bottom-left to top-right
    g.paste([
        "..........kk",
        ".........kyrk",
        "........kyyrk",
        ".......kyyrk.",
        "......kyyrk..",
        ".....kyyrk...",
        "....kyyrk....",
        "....kwrk.....",
        "....kkk......",
    ], 3, 6)
    return g


def btn_fif():
    g = Grid(16, 16)
    g.paste([
        "................",
        "................",
        "................",
        "................",
        "................",
        ".kkkk...........",
        "kyyyykkkkkk.....",
        "kywywywywyk.....",
        "kwywywywywk.....",
        "kywywywywyk.....",
        "kwywywywywk.....",
        "kywywywywyk.....",
        "kwywywywywk.....",
        "kkkkkkkkkkk.....",
        "................",
        "................",
    ])
    magnifier(g, 5, 0, handle=3)
    return g


FONT3x5 = {
    '0': ['111', '101', '101', '101', '111'], '1': ['010', '110', '010', '010', '111'],
    'A': ['010', '101', '111', '101', '101'], 'F': ['111', '100', '110', '100', '100'],
    '3': ['111', '001', '011', '001', '111'], '7': ['111', '001', '010', '010', '010'],
    'C': ['111', '100', '100', '100', '111'], '4': ['101', '101', '111', '001', '001'],
}


def text3x5(g, x, y, s, col):
    for ch in s:
        for r, row in enumerate(FONT3x5[ch]):
            for c, bit in enumerate(row):
                if bit == '1':
                    g.set(x + c, y + r, col)
        x += 4


def btn_hexview():
    g = Grid(16, 16)
    g.rect(0, 0, 15, 15, fill='w', outline='k')
    g.hline(1, 14, 1, 'n')
    g.hline(1, 14, 2, 'n')
    text3x5(g, 2, 4, '0F', 'n')
    text3x5(g, 2, 10, 'A3', 'n')
    g.vline(10, 4, 14, 's')
    text3x5(g, 12, 4, '1', 'r')
    text3x5(g, 12, 10, '4', 'g')
    return g


def btn_wrap():
    g = Grid(16, 16)
    g.hline(1, 13, 3, 'k')
    g.hline(1, 10, 6, 'k')
    g.hline(1, 6, 13, 'k')
    # wrap arrow: from the end of line 2 right, down, and back left into line 3
    g.hline(11, 13, 6, 'b')
    g.vline(14, 7, 9, 'b')
    g.set(13, 6, 'b'); g.set(14, 6, '.')
    g.hline(5, 13, 10, 'b')
    g.set(14, 10, '.')
    g.paste([
        "..b...",
        ".bb...",
        "bbbbbb",
        ".bb...",
        "..b...",
    ], 1, 8)
    g.hline(7, 13, 10, 'b')
    return g


def btn_showws():
    g = Grid(16, 16)
    g.paste([
        "................",
        "....bbbbbbbbb...",
        "...bbbbbbb..b...",
        "..bbbbbbbb..b...",
        "..bbbbbbbb..b...",
        "..bbbbbbbb..b...",
        "...bbbbbbb..b...",
        "....bbbbbb..b...",
        ".......bbb..b...",
        ".......bbb..b...",
        ".......bbb..b...",
        ".......bbb..b...",
        ".......bbb..b...",
        ".......bbb..b...",
        "......bbbbbbbb..",
        "................",
    ])
    # keep it a classic one-colour pilcrow: thin second stem
    for y in range(2, 14):
        g.set(11, y, '.')
        g.set(12, y, 'b')
    return g


def buttons():
    return [btn_new(), btn_open(), btn_save(), btn_saveall(), btn_cut(), btn_copy(), btn_paste(), btn_undo(),
            btn_redo(), btn_find(), btn_replace(), btn_fif(), btn_hexview(), btn_wrap(), btn_showws()]


def icon32():
    g = Grid(32, 32)
    g.page(4, 1, 27, 30, fold=6)
    # text lines (left), hex bytes (right): "one document, two views"
    for i, y in enumerate(range(6, 16, 3)):
        g.hline(7, 7 + (9 if i % 3 else 6), y, 'G')
    g.vline(18, 9, 27, 's')
    text3x5(g, 20, 9, '0F', 'n')
    text3x5(g, 20, 15, 'A3', 'n')
    text3x5(g, 20, 21, '10', 'r')
    # big navy "P" badge in the lower left
    g.rect(1, 17, 13, 31, fill='n', outline='k')
    g.paste([
        "wwwwww.",
        "ww...ww",
        "ww...ww",
        "ww...ww",
        "wwwwww.",
        "ww.....",
        "ww.....",
        "ww.....",
        "ww.....",
    ], 4, 20)
    return g


def icon16():
    g = Grid(16, 16)
    g.page(3, 0, 15, 15, fold=4, lines=[(5, 8, 3), (5, 10, 5)])
    g.vline(10, 8, 13, 's')
    g.hline(11, 13, 9, 'n')
    g.hline(11, 13, 11, 'r')
    g.hline(11, 13, 13, 'n')
    g.rect(0, 7, 8, 15, fill='n', outline='k')
    g.paste([
        "wwww.",
        "w...w",
        "w...w",
        "wwww.",
        "w....",
        "w....",
        "w....",
    ], 2, 8)
    return g


# ---- file writers ----

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


def ico_entry(g):
    idx = g.px
    size = g.w
    mask_rows = []
    xor_idx = []
    for r in idx:
        bits = 0
        mrow = bytearray()
        xr = []
        for x, k in enumerate(r):
            transparent = (k == KEY)
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


def write_ico(path, grids):
    entries = [ico_entry(g) for g in grids]
    off = 6 + 16 * len(entries)
    out = struct.pack('<HHH', 0, 1, len(entries))
    data = b''
    for size, blob in entries:
        out += struct.pack('<BBBBHHII', size, size, 16, 0, 1, 4, len(blob), off + len(data))
        data += blob
    with open(path, 'wb') as f:
        f.write(out + data)


def preview(path, btns, icons):
    from PIL import Image
    z = 8
    face = (192, 192, 192)
    w = (16 * 15 + 14 * 4 + 8) * z // 2
    img = Image.new('RGB', (max(w, 1300), 16 * z + 40 * z // 2 + 300), face)
    px = img.load()

    def blit(g, ox, oy, zoom):
        for y in range(g.h):
            for x in range(g.w):
                k = g.px[y][x]
                col = face if k == KEY else PAL[k]
                for yy in range(zoom):
                    for xx in range(zoom):
                        px[ox + x * zoom + xx, oy + y * zoom + yy] = col
    x = 4
    for g in btns:
        blit(g, x, 4, 5)
        x += 16 * 5 + 6
    # 1:1 strip as it appears on screen
    x = 4
    for g in btns:
        blit(g, x, 100, 1)
        x += 23
    blit(icons[0], 4, 130, 5)
    blit(icons[1], 180, 130, 5)
    blit(icons[0], 280, 130, 1)
    blit(icons[1], 320, 130, 1)
    img.save(path)


def main():
    btns = buttons()
    strip = [[KEY] * (16 * 15) for _ in range(16)]
    for i, g in enumerate(btns):
        for y in range(16):
            for x in range(16):
                strip[y][16 * i + x] = g.px[y][x]
    write_bmp4(os.path.join(ROOT, 'res', 'toolbar.bmp'), strip)
    icons = [icon32(), icon16()]
    write_ico(os.path.join(ROOT, 'res', 'pica.ico'), icons)
    print('wrote res/toolbar.bmp and res/pica.ico')
    if '--preview' in sys.argv:
        out = os.path.join(ROOT, 'docs', 'shots', 't07_icons.png')
        preview(out, btns, icons)
        print('wrote', out)


if __name__ == '__main__':
    main()
