"""Draws Glasswork Bitmap's icon (a little DOS-style code window) and writes it as PNG and ICO files.

Run with any Python 3: python packaging/make_icon.py
"""
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

# 16x16 design; each letter is a palette color.
PALETTE = {
    '.': (0, 0, 0, 0),
    'k': (0, 0, 0, 255),
    'b': (0, 0, 170, 255),
    'w': (255, 255, 255, 255),
    'g': (170, 170, 170, 255),
    'y': (255, 255, 85, 255),
    'c': (85, 255, 255, 255),
    'r': (255, 85, 85, 255),
    'l': (85, 255, 85, 255),
}
DESIGN = [
    "gggggggggggggg..",
    "gryggggggggggg..",
    "bbbbbbbbbbbbbbk.",
    "bwwwwwwwwwwwwbk.",
    "bwbbbbbbbbbbwbk.",
    "bwbyyybcccbbwbk.",
    "bwbbbbbbbbbbwbk.",
    "bwbbcccclllbwbk.",
    "bwbbbbbbbbbbwbk.",
    "bwbyyyywwbbbwbk.",
    "bwbbbbbbbbbbwbk.",
    "bwbbrrbbbbbbwbk.",
    "bwbbbbbbbbbbwbk.",
    "bwwwwwwwwwwwwbk.",
    "bbbbbbbbbbbbbbk.",
    ".kkkkkkkkkkkkkk.",
]


def pixels(size):
    scale = size // 16
    rows = []
    for y in range(size):
        row = bytearray()
        for x in range(size):
            row += bytes(PALETTE[DESIGN[min(15, y // scale)][min(15, x // scale)]])
        rows.append(bytes(row))
    return rows


def png(size):
    raw = b''.join(b'\x00' + row for row in pixels(size))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def ico(sizes):
    images = [png(size) for size in sizes]
    header = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b''
    for size, data in zip(sizes, images):
        entries += struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    return header + entries + b''.join(images)


if __name__ == '__main__':
    with open(os.path.join(HERE, 'windows', 'glasswork-bitmap.ico'), 'wb') as f:
        f.write(ico([16, 32, 48, 64, 128, 256]))
    with open(os.path.join(HERE, 'linux', 'glasswork-bitmap.png'), 'wb') as f:
        f.write(png(256))
    print('icons written')
