# /// script
# requires-python = ">=3.11"
# ///

from __future__ import annotations

import argparse
import math
import struct
from dataclasses import dataclass
from pathlib import Path


DDS_MAGIC = b"DDS "
DDS_FOURCC = 0x00000004
DDS_DX10_FOURCC = 0x30315844
DXGI_FORMAT_R10G10B10A2_UNORM = 24
DDS_DIMENSION_TEXTURE3D = 4


@dataclass(frozen=True)
class Dds3DLut:
    width: int
    height: int
    depth: int
    payload: bytes


def _fourcc_name(value: int) -> str:
    try:
        return value.to_bytes(4, "little").decode("ascii")
    except UnicodeDecodeError:
        return f"0x{value:08x}"


def read_rgb10a2_3d_dds(path: Path) -> Dds3DLut:
    data = path.read_bytes()
    if len(data) < 148:
        raise ValueError(f"{path} is too small to contain a DX10 DDS header")
    if data[:4] != DDS_MAGIC:
        raise ValueError(f"{path} is not a DDS file")

    header_size, _flags, height, width, _pitch, depth, mip_count = struct.unpack_from("<7I", data, 4)
    if header_size != 124:
        raise ValueError(f"unsupported DDS header size {header_size}; expected 124")

    pf_size, pf_flags, pf_fourcc = struct.unpack_from("<3I", data, 4 + 72)
    if pf_size != 32:
        raise ValueError(f"unsupported DDS pixel format size {pf_size}; expected 32")
    if (pf_flags & DDS_FOURCC) == 0 or pf_fourcc != DDS_DX10_FOURCC:
        raise ValueError(f"unsupported DDS pixel format FourCC {_fourcc_name(pf_fourcc)}; expected DX10")

    dxgi_format, resource_dimension, _misc_flag, array_size, _misc_flags2 = struct.unpack_from("<5I", data, 128)
    if dxgi_format != DXGI_FORMAT_R10G10B10A2_UNORM:
        raise ValueError(f"unsupported DXGI format {dxgi_format}; expected R10G10B10A2_UNORM ({DXGI_FORMAT_R10G10B10A2_UNORM})")
    if resource_dimension != DDS_DIMENSION_TEXTURE3D:
        raise ValueError(f"unsupported DDS resource dimension {resource_dimension}; expected TEXTURE3D ({DDS_DIMENSION_TEXTURE3D})")
    if array_size != 1:
        raise ValueError(f"unsupported DDS array size {array_size}; expected 1")
    if mip_count not in (0, 1):
        raise ValueError(f"unsupported DDS mip count {mip_count}; expected a single mip")
    if width == 0 or height == 0 or depth == 0:
        raise ValueError(f"invalid DDS dimensions {width}x{height}x{depth}")
    if width != height or height != depth:
        raise ValueError(f"DDS is {width}x{height}x{depth}; expected a cubic 3D LUT")

    expected_payload_size = width * height * depth * 4
    payload_offset = 148
    payload_end = payload_offset + expected_payload_size
    if len(data) < payload_end:
        raise ValueError(f"{path} payload is {len(data) - payload_offset} bytes; expected {expected_payload_size}")

    return Dds3DLut(width=width, height=height, depth=depth, payload=data[payload_offset:payload_end])


def srgb_encode(linear: float) -> float:
    value = min(max(linear, 0.0), 1.0)
    if value <= 0.0031308:
        return value * 12.92
    return 1.055 * math.pow(value, 1.0 / 2.4) - 0.055


def unorm8(value: float) -> int:
    return min(max(int(value * 255.0 + 0.5), 0), 255)


def unpack_rgb10a2_unorm(packed: int) -> tuple[float, float, float]:
    r = (packed & 0x000003ff) / 1023.0
    g = ((packed >> 10) & 0x000003ff) / 1023.0
    b = ((packed >> 20) & 0x000003ff) / 1023.0
    return r, g, b


def build_u_tiled_bgr(lut: Dds3DLut, linear_input: bool) -> tuple[int, int, bytes]:
    output_width = lut.width * lut.depth
    output_height = lut.height
    if output_width > 65535 or output_height > 65535:
        raise ValueError(f"TGA dimensions {output_width}x{output_height} exceed the 65535 pixel limit")

    pixels = bytearray(output_width * output_height * 3)
    texels = struct.iter_unpack("<I", lut.payload)

    for slice_z in range(lut.depth):
        for y in range(lut.height):
            for x in range(lut.width):
                packed = next(texels)[0]
                r, g, b = unpack_rgb10a2_unorm(packed)
                if linear_input:
                    r = srgb_encode(r)
                    g = srgb_encode(g)
                    b = srgb_encode(b)

                dst_x = slice_z * lut.width + x
                dst = (y * output_width + dst_x) * 3
                pixels[dst + 0] = unorm8(b)
                pixels[dst + 1] = unorm8(g)
                pixels[dst + 2] = unorm8(r)

    return output_width, output_height, bytes(pixels)


def write_tga(path: Path, width: int, height: int, bgr_pixels: bytes) -> None:
    expected_size = width * height * 3
    if len(bgr_pixels) != expected_size:
        raise ValueError(f"got {len(bgr_pixels)} BGR bytes for {width}x{height}; expected {expected_size}")

    header = struct.pack(
        "<BBBHHBHHHHBB",
        0,      # ID length
        0,      # no color map
        2,      # uncompressed true-color image
        0,      # color map first entry
        0,      # color map length
        0,      # color map entry size
        0,      # x origin
        0,      # y origin
        width,
        height,
        24,     # bits per pixel
        0x20,   # top-left origin
    )

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(header + bgr_pixels)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Convert a cubic RGB10A2 DX10 3D DDS LUT to a U-tiled 24-bit TGA. "
            "Slices are laid out horizontally by B/Z."
        )
    )
    parser.add_argument("input", type=Path, help="Input 3D DDS LUT")
    parser.add_argument("output", type=Path, help="Output U-tiled TGA")
    parser.add_argument(
        "--linear-input",
        action="store_true",
        help=(
            "apply linear-to-sRGB encoding before writing; omit this for DDS files "
            "from OCIOBakeLUTs.py, which are already display-encoded"
        ),
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    lut = read_rgb10a2_3d_dds(args.input)
    width, height, bgr_pixels = build_u_tiled_bgr(lut, linear_input=args.linear_input)
    write_tga(args.output, width, height, bgr_pixels)
    print(f"Read  {args.input} ({lut.width}x{lut.height}x{lut.depth} RGB10A2 3D DDS)")
    print(f"Wrote {args.output} ({width}x{height} U-tiled 24-bit TGA)")


if __name__ == "__main__":
    main()
