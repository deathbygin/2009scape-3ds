# Draws the plain icon and banner for the installed title, and a moment of silence for the banner sound.
# All generated here, so nothing of the game's own artwork goes into the file.
import os
import struct
import sys
import wave
import zlib

FONT = {
    '0': ['111', '101', '101', '101', '111'], '2': ['111', '001', '111', '100', '111'],
    '9': ['111', '101', '111', '001', '111'], '3': ['111', '001', '111', '001', '111'],
    'D': ['110', '101', '101', '101', '110'], 'S': ['111', '100', '111', '001', '111'],
}


def png(path, width, height, pixels):
    rows = b''.join(b'\0' + bytes(c for p in pixels[y * width:(y + 1) * width] for c in p) for y in range(height))

    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def stone(width, height):
    out = []
    for y in range(height):
        for x in range(width):
            shade = 60 + (x * 7 + y * 13) % 11 - (6 if (x // 8 + y // 8) % 2 else 0)
            edge = x < 2 or y < 2 or x >= width - 2 or y >= height - 2
            out.append((150, 120, 60, 255) if edge else (shade, shade - 6, shade - 16, 255))
    return out


def text(pixels, width, string, left, top, scale, colour):
    x = left
    for letter in string:
        for row, bits in enumerate(FONT[letter]):
            for column, bit in enumerate(bits):
                if bit == '1':
                    for dy in range(scale):
                        for dx in range(scale):
                            pixels[(top + row * scale + dy) * width + x + column * scale + dx] = colour
        x += 4 * scale


out = sys.argv[1]
gold = (255, 200, 60, 255)
white = (235, 230, 215, 255)
icon = stone(48, 48)
text(icon, 48, '2009', 8, 10, 2, gold)
text(icon, 48, '3DS', 12, 26, 2, white)
png(os.path.join(out, 'icon.png'), 48, 48, icon)
banner = stone(256, 128)
text(banner, 256, '2009', 34, 22, 12, gold)
text(banner, 256, '3DS', 74, 88, 5, white)
png(os.path.join(out, 'banner.png'), 256, 128, banner)
with wave.open(os.path.join(out, 'silence.wav'), 'wb') as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(22050)
    w.writeframes(b'\0' * 4 * 11025)
print('art ok')
