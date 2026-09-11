"""Reproduce full original PNG in a transparent square ICO; requires Pillow."""
from pathlib import Path
from io import BytesIO
import struct
from PIL import Image


def main():
    root = Path(__file__).resolve().parent.parent
    source = Image.open(root / "input" / "LOGO.png").convert("RGBA")
    sizes = (16, 24, 32, 48, 64, 128, 256)
    frames = []
    for size in sizes:
        ratio = size / max(source.size)
        dims = tuple(max(1, round(d * ratio)) for d in source.size)
        resized = source.resize(dims, Image.Resampling.LANCZOS)
        frame = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        frame.alpha_composite(resized, ((size - dims[0]) // 2, (size - dims[1]) // 2))
        encoded = BytesIO()
        frame.save(encoded, format="PNG", optimize=False, compress_level=9)
        frames.append(encoded.getvalue())
    result = bytearray(struct.pack("<HHH", 0, 1, len(sizes)))
    offset = 6 + 16 * len(sizes)
    for size, frame in zip(sizes, frames):
        result.extend(struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(frame), offset))
        offset += len(frame)
    for frame in frames:
        result.extend(frame)
    target = root / "resources" / "GameWindowMapper.ico"
    target.write_bytes(result)
    print(f"{target}: {len(result)} bytes, sizes={sizes}")


if __name__ == "__main__":
    main()
