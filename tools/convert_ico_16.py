#!/usr/bin/env python3
"""Convert .ico files to 16x16 4-bit raw icon data for FRANK OS.

Reads a Windows 95/2000 .ico file, extracts or downscales to 16x16,
applies the AND mask (transparent -> 0xFF), and remaps palette indices
from Windows ordering to CGA ordering.

Modes:
    --inf "Name" input.ico output.inf
        Writes display name + newline + 256 raw bytes to output.inf

    --c-array name input.ico
        Writes a C const uint8_t name[256] array to stdout
"""

import struct
import sys

# Windows 95 palette index -> CGA palette index remap table.
WIN_TO_CGA = [0, 4, 2, 6, 1, 5, 3, 7, 8, 12, 10, 14, 9, 13, 11, 15]


def read_ico_image(data, target_size):
    """Find an image entry of target_size in the ICO data.
    Returns (width, height, pixels_32x32_or_target) or None."""
    reserved, ico_type, count = struct.unpack_from("<HHH", data, 0)
    if ico_type != 1:
        raise ValueError(f"Not an icon file (type={ico_type})")

    best = None
    for i in range(count):
        off = 6 + i * 16
        width, height, color_count, reserved2 = struct.unpack_from("<BBBB", data, off)
        planes, bpp, size, offset = struct.unpack_from("<HHII", data, off + 4)
        w = width if width != 0 else 256
        h = height if height != 0 else 256
        if w == target_size and h == target_size:
            best = (w, h, size, offset)
            break

    return best


def extract_4bpp_image(data, img_offset, expected_w):
    """Extract a 4bpp image at the given offset, return pixel list."""
    (hdr_size, bmp_w, bmp_h, planes, bpp, compression,
     img_data_size, xppm, yppm, colors_used, colors_important) = \
        struct.unpack_from("<IiiHHIIiiII", data, img_offset)

    actual_h = bmp_h // 2

    if bpp != 4:
        raise ValueError(f"Expected 4bpp, got {bpp}bpp")
    if actual_h != expected_w or bmp_w != expected_w:
        raise ValueError(f"Expected {expected_w}x{expected_w}, got {bmp_w}x{actual_h}")

    palette_offset = img_offset + hdr_size
    num_colors = colors_used if colors_used > 0 else 16

    xor_offset = palette_offset + num_colors * 4
    row_bytes = (bmp_w * bpp + 7) // 8
    row_stride = (row_bytes + 3) & ~3

    and_offset = xor_offset + row_stride * actual_h
    and_stride = ((bmp_w + 7) // 8 + 3) & ~3

    pixels = []
    for row in range(actual_h):
        bmp_row = actual_h - 1 - row
        row_data = data[xor_offset + bmp_row * row_stride:
                        xor_offset + bmp_row * row_stride + row_bytes]
        and_data = data[and_offset + bmp_row * and_stride:
                        and_offset + bmp_row * and_stride + ((bmp_w + 7) // 8)]

        for col in range(bmp_w):
            byte_idx = col // 2
            if col % 2 == 0:
                win_idx = (row_data[byte_idx] >> 4) & 0x0F
            else:
                win_idx = row_data[byte_idx] & 0x0F

            and_byte = and_data[col // 8]
            and_bit = (and_byte >> (7 - (col % 8))) & 1

            if and_bit:
                pixels.append(0xFF)
            else:
                pixels.append(WIN_TO_CGA[win_idx])

    return pixels


def downscale_nn(pixels_32, src_size, dst_size):
    """Nearest-neighbor downscale from src_size to dst_size."""
    result = []
    for y in range(dst_size):
        sy = y * src_size // dst_size
        for x in range(dst_size):
            sx = x * src_size // dst_size
            result.append(pixels_32[sy * src_size + sx])
    return result


def read_ico_16(filename):
    """Read an .ico and return 16x16 pixel data (256 bytes)."""
    with open(filename, "rb") as f:
        data = f.read()

    # Try 16x16 first
    entry = read_ico_image(data, 16)
    if entry:
        w, h, size, offset = entry
        return extract_4bpp_image(data, offset, 16)

    # Fall back to 32x32 and downscale
    entry = read_ico_image(data, 32)
    if entry:
        w, h, size, offset = entry
        pixels_32 = extract_4bpp_image(data, offset, 32)
        return downscale_nn(pixels_32, 32, 16)

    raise ValueError("No 16x16 or 32x32 image found in .ico file")


def write_inf(name, pixels, output_path, ext_line=None):
    """Write name + newline + 256 raw bytes + optional ext line to output .inf file."""
    with open(output_path, "wb") as f:
        f.write(name.encode("ascii"))
        f.write(b"\n")
        f.write(bytes(pixels))
        if ext_line:
            f.write(ext_line.encode("ascii"))
            f.write(b"\n")


def format_c_array(name, pixels):
    """Format 16x16 pixel data as a C array."""
    lines = []
    lines.append(f"const uint8_t {name}[256] = {{")
    for row in range(16):
        row_data = pixels[row * 16:(row + 1) * 16]
        hex_vals = ", ".join(f"0x{v:02X}" for v in row_data)
        lines.append(f"    {hex_vals},")
    lines.append("};")
    return "\n".join(lines)


def main():
    if len(sys.argv) < 2:
        print("Usage:", file=sys.stderr)
        print(f"  {sys.argv[0]} --inf \"Name\" input.ico output.inf", file=sys.stderr)
        print(f"  {sys.argv[0]} --c-array name input.ico", file=sys.stderr)
        sys.exit(1)

    mode = sys.argv[1]

    if mode == "--inf":
        # Parse optional --ext argument
        ext_line = None
        args = sys.argv[2:]
        if len(args) >= 2 and args[0] == "--ext":
            ext_line = args[1]
            args = args[2:]
        if len(args) != 3:
            print(f"Usage: {sys.argv[0]} --inf [--ext EXT_LINE] \"Name\" input.ico output.inf",
                  file=sys.stderr)
            sys.exit(1)
        name = args[0]
        ico_path = args[1]
        out_path = args[2]
        pixels = read_ico_16(ico_path)
        write_inf(name, pixels, out_path, ext_line)

    elif mode == "--c-array":
        if len(sys.argv) != 4:
            print(f"Usage: {sys.argv[0]} --c-array name input.ico",
                  file=sys.stderr)
            sys.exit(1)
        array_name = sys.argv[2]
        ico_path = sys.argv[3]
        pixels = read_ico_16(ico_path)
        print(format_c_array(array_name, pixels))

    else:
        print(f"Unknown mode: {mode}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
