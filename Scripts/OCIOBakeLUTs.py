# /// script
# requires-python = ">=3.11,<3.12"
# dependencies = [
#   "numpy",
#   "opencolorio",
# ]
# ///

from __future__ import annotations

import argparse
import struct
import time
from pathlib import Path

import numpy as np
import PyOpenColorIO as ocio  # type: ignore[reportMissingImports]

DEFAULT_INPUT_SPACE = "ACEScct"
DEFAULT_LUT_SIZE = 32
DEFAULT_DISPLAY = "sRGB"

DDS_MAGIC = b"DDS "
DDS_FOURCC = 0x00000004
DDS_HEADER_FLAGS_TEXTURE = 0x00001007
DDS_HEADER_FLAGS_VOLUME = 0x00800000
DDS_SURFACE_FLAGS_TEXTURE = 0x00001000
DDS_FLAGS_VOLUME = 0x00200000
DDS_DX10_FOURCC = 0x30315844
DXGI_FORMAT_R10G10B10A2_UNORM = 24
DDS_DIMENSION_TEXTURE3D = 4


def create_config(config_path: Path) -> ocio.Config:
    try:
        return ocio.Config.CreateFromFile(str(config_path))
    except Exception as exc:
        raise RuntimeError(f"OCIO config '{config_path}' is unavailable.") from exc


def create_processor(
    config: ocio.Config,
    *,
    input_space: str,
    display: str,
    view: str,
    look: str | None,
) -> ocio.CPUProcessor:
    available_views = set(config.getViews(display))
    if view not in available_views:
        raise ValueError(f"Display '{display}' has no view '{view}'. Available views: {sorted(available_views)}")

    display_transform = ocio.DisplayViewTransform(
        src=input_space,
        display=display,
        view=view,
    )
    if look is None:
        return config.getProcessor(display_transform).getDefaultCPUProcessor()

    available_looks = {config_look.getName() for config_look in config.getLooks()}
    if look not in available_looks:
        raise ValueError(f"OCIO config has no look '{look}'. Available looks: {sorted(available_looks)}")

    look_transform = ocio.LookTransform(
        src=input_space,
        dst=input_space,
        looks=look,
    )
    transform = ocio.GroupTransform()
    transform.appendTransform(look_transform)
    transform.appendTransform(display_transform)
    return config.getProcessor(transform).getDefaultCPUProcessor()


def apply_processor(processor: ocio.CPUProcessor, rgb: np.ndarray) -> np.ndarray:
    # applyRGB returns a new Python list and leaves the input list unchanged.
    return np.array(processor.applyRGB(rgb.astype(np.float32).tolist()), dtype=np.float32)


def bake_lut_rgba(processor: ocio.CPUProcessor, size: int) -> np.ndarray:
    """Bake a 3D LUT in the raw input-space [0,1] domain, packed as RGBA32F."""
    out = np.empty(size * size * size * 4, dtype=np.float32)
    denom = float(size - 1)
    write = 0
    for b in range(size):
        for g in range(size):
            for r in range(size):
                sample = np.array([r / denom, g / denom, b / denom], dtype=np.float32)
                rgb = apply_processor(processor, sample)
                out[write + 0] = rgb[0]
                out[write + 1] = rgb[1]
                out[write + 2] = rgb[2]
                out[write + 3] = 1.0
                write += 4
    return out


def pack_rgb10a2_unorm(lut: np.ndarray) -> bytes:
    rgba = np.clip(lut, 0.0, 1.0).reshape(-1, 4)
    rgb = np.rint(rgba[:, :3] * 1023.0).astype(np.uint32)
    alpha = np.rint(rgba[:, 3] * 3.0).astype(np.uint32)
    packed = rgb[:, 0] | (rgb[:, 1] << 10) | (rgb[:, 2] << 20) | (alpha << 30)
    return packed.astype(np.dtype("<u4"), copy=False).tobytes(order="C")


def write_lut_dds(path: Path, lut: np.ndarray, size: int) -> None:
    """Write a single-mip 3D DDS using DXGI_FORMAT_R10G10B10A2_UNORM."""
    texels = size * size * size
    expected_values = texels * 4
    if lut.size != expected_values:
        raise ValueError(f"Expected {expected_values} float values for {size}^3 RGBA LUT, got {lut.size}")

    payload = pack_rgb10a2_unorm(lut)
    expected_payload_size = texels * 4
    if len(payload) != expected_payload_size:
        raise ValueError(f"Expected {expected_payload_size} DDS payload bytes, got {len(payload)}")

    dds_pixel_format = struct.pack(
        "<8I",
        32,  # DDS_PIXELFORMAT size
        DDS_FOURCC,
        DDS_DX10_FOURCC,
        0, 0, 0, 0, 0,
    )
    header = struct.pack(
        "<7I11I",
        124,  # DDS_HEADER size
        DDS_HEADER_FLAGS_TEXTURE | DDS_HEADER_FLAGS_VOLUME,
        size,
        size,
        0,
        size,
        1,
        *([0] * 11),
    )
    header += dds_pixel_format
    header += struct.pack(
        "<5I",
        DDS_SURFACE_FLAGS_TEXTURE,
        DDS_FLAGS_VOLUME,
        0,
        0,
        0,
    )
    header10 = struct.pack(
        "<5I",
        DXGI_FORMAT_R10G10B10A2_UNORM,
        DDS_DIMENSION_TEXTURE3D,
        0,
        1,
        0,
    )

    if len(header) != 124:
        raise AssertionError(f"DDS_HEADER must be 124 bytes, got {len(header)}")
    if len(header10) != 20:
        raise AssertionError(f"DDS_HEADER_DXT10 must be 20 bytes, got {len(header10)}")

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(DDS_MAGIC + header + header10 + payload)


def unorm8(value: float) -> int:
    return min(max(int(value * 255.0 + 0.5), 0), 255)


def build_u_tiled_bgr(lut: np.ndarray, size: int, *, flip_u: bool, flip_v: bool) -> tuple[int, int, bytes]:
    texels = size * size * size
    expected_values = texels * 4
    if lut.size != expected_values:
        raise ValueError(f"Expected {expected_values} float values for {size}^3 RGBA LUT, got {lut.size}")

    output_width = size * size
    output_height = size
    if output_width > 65535 or output_height > 65535:
        raise ValueError(f"TGA dimensions {output_width}x{output_height} exceed the 65535 pixel limit")

    pixels = bytearray(output_width * output_height * 3)
    rgba = np.clip(lut.reshape(-1, 4), 0.0, 1.0)

    for slice_z in range(size):
        for y in range(size):
            for x in range(size):
                src = slice_z * size * size + y * size + x
                local_x = size - 1 - x if flip_u else x
                local_y = size - 1 - y if flip_v else y
                dst_x = slice_z * size + local_x
                dst = (local_y * output_width + dst_x) * 3
                pixels[dst + 0] = unorm8(float(rgba[src, 2]))
                pixels[dst + 1] = unorm8(float(rgba[src, 1]))
                pixels[dst + 2] = unorm8(float(rgba[src, 0]))

    return output_width, output_height, bytes(pixels)


def write_lut_tga(path: Path, lut: np.ndarray, size: int, *, flip_u: bool, flip_v: bool) -> None:
    width, height, bgr_pixels = build_u_tiled_bgr(lut, size, flip_u=flip_u, flip_v=flip_v)
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


def bake_dds(
    config_path: Path,
    output_path: Path,
    *,
    display: str,
    view: str,
    look: str | None,
    input_space: str = DEFAULT_INPUT_SPACE,
    size: int = DEFAULT_LUT_SIZE,
) -> None:
    config = create_config(config_path)
    processor = create_processor(
        config,
        input_space=input_space,
        display=display,
        view=view,
        look=look,
    )
    lut = bake_lut_rgba(processor, size)
    write_lut_dds(output_path, lut, size)


def print_config_listing(config: ocio.Config, config_path: Path) -> None:
    print(f"Using OCIO {ocio.__version__}")
    print(f"Config: {config_path}")

    print("\nDisplays and Views:")
    displays = list(config.getDisplays())
    if not displays:
        print("  <none>")
    for display in displays:
        print(f"  {display}")
        views = list(config.getViews(display))
        if not views:
            print("    <none>")
        for view in views:
            print(f"    - {view}")

    print("\nLooks:")
    looks = [look.getName() for look in config.getLooks()]
    if not looks:
        print("  <none>")
    for look in looks:
        print(f"  - {look}")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Bake one OCIO display/view transform to a single DDS or U-tiled TGA LUT.",
    )
    parser.add_argument("output", nargs="?", type=Path, help="Output LUT path")
    parser.add_argument("--config", required=True, type=Path, help="Input OCIO config path")
    parser.add_argument("--display", default=DEFAULT_DISPLAY, help=f"OCIO display name, default: {DEFAULT_DISPLAY}")
    parser.add_argument("--view", help="OCIO view name")
    parser.add_argument("--look", help="Optional OCIO look name")
    parser.add_argument("--input-space", default=DEFAULT_INPUT_SPACE, help=f"Input color space, default: {DEFAULT_INPUT_SPACE}")
    parser.add_argument("--size", type=int, default=DEFAULT_LUT_SIZE, help=f"3D LUT edge size, default: {DEFAULT_LUT_SIZE}")
    parser.add_argument("--list", action="store_true", help="List displays, views, and looks in the config, then exit")

    format_group = parser.add_mutually_exclusive_group()
    format_group.add_argument("--dds", action="store_true", help="Write a 3D DDS LUT (default)")
    format_group.add_argument("--tga", action="store_true", help="Write a U-tiled 2D TGA LUT")

    parser.add_argument("--flip-u", action="store_true", help="TGA only: flip X/U within each slice tile")
    parser.add_argument("--flip-v", action="store_true", help="TGA only: flip Y/V within each slice tile")

    args = parser.parse_args(argv)
    if args.size < 2:
        parser.error("--size must be at least 2")
    if args.list:
        return args
    if not args.tga and (args.flip_u or args.flip_v):
        parser.error("--flip-u and --flip-v require --tga")
    if args.output is None:
        parser.error("output is required unless --list is used")
    if args.view is None:
        parser.error("--view is required unless --list is used")
    return args


def main(argv: list[str] | None = None) -> None:
    args = parse_args(argv)
    config = create_config(args.config)

    if args.list:
        print_config_listing(config, args.config)
        return

    look = "No Look" if args.look is None else args.look
    print(f"Using OCIO {ocio.__version__}")
    print(f"Config: {args.config}")
    print(f"Input:  {args.input_space}")
    print(f"View:   {args.display} / {args.view} / {look}")
    output_format = "TGA U-tiled 24-bit BGR" if args.tga else "DDS RGB10A2 UNORM"
    print(f"Size:   {args.size}^3, {output_format}")

    start = time.perf_counter()
    processor = create_processor(
        config,
        input_space=args.input_space,
        display=args.display,
        view=args.view,
        look=args.look,
    )
    lut = bake_lut_rgba(processor, args.size)
    elapsed = time.perf_counter() - start

    if args.tga:
        write_lut_tga(args.output, lut, args.size, flip_u=args.flip_u, flip_v=args.flip_v)
    else:
        write_lut_dds(args.output, lut, args.size)
    size_kb = lut.nbytes / 1024.0
    print(f"Wrote {args.output} ({lut.size} floats, {size_kb:.1f} KiB, {elapsed:.2f}s)")


if __name__ == "__main__":
    main()
