"""生成 Windows M0 使用的原创几何标记；仅依赖 Python 标准库。"""
from pathlib import Path
import struct
import zlib

ASSET_ROOT = Path(__file__).resolve().parents[2] / 'apps/windows/CPEditorNativeWindows/Assets'
SIZES = {
    'Square150x150Logo.scale-200.png': (300, 300),
    'Square44x44Logo.scale-200.png': (88, 88),
    'Square44x44Logo.targetsize-24_altform-unplated.png': (24, 24),
    'StoreLogo.png': (50, 50),
    'Wide310x150Logo.scale-200.png': (620, 300),
    'SplashScreen.scale-200.png': (1240, 600),
}


def chunk(kind, data):
    return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data))


def logo(width, height):
    size = min(width, height) * 0.62
    left, top = (width - size) / 2, (height - size) / 2
    pixels = bytearray()
    # Three segments on each side form angle brackets around a central cursor.
    for y in range(height):
        pixels.append(0)
        for x in range(width):
            u, v = (x - left) / size, (y - top) / size
            bracket = (0.08 <= u <= 0.22 and 0.16 <= v <= 0.84) or \
                      (0.08 <= u <= 0.38 and (0.16 <= v <= 0.28 or 0.72 <= v <= 0.84)) or \
                      (0.78 <= u <= 0.92 and 0.16 <= v <= 0.84) or \
                      (0.62 <= u <= 0.92 and (0.16 <= v <= 0.28 or 0.72 <= v <= 0.84))
            cursor = 0.46 <= u <= 0.54 and 0.30 <= v <= 0.70
            pixels.extend((68, 137, 232, 255) if bracket else (35, 175, 165, 255) if cursor else (0, 0, 0, 0))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!IIBBBBB', width, height, 8, 6, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(pixels, 9)) + chunk(b'IEND', b'')


def main():
    ASSET_ROOT.mkdir(parents=True, exist_ok=True)
    for name, (width, height) in SIZES.items():
        (ASSET_ROOT / name).write_bytes(logo(width, height))


if __name__ == '__main__':
    main()
