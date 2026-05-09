# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "numpy",
#   "opencolorio",
# ]
# ///

CONFIG_NAME = "cg-config-v2.2.0_aces-v1.3_ocio-v2.4"
PROCESSOR_INPUT_SPACE = "ACEScct"
SCENE_INPUT_SPACE = "Linear Rec.709 (sRGB)"

VIEWS = [
    {
        "name": "sdr_rec709",
        "ident": "kAcesLutSdrRec709",
        "display": "sRGB - Display",
        "view": "ACES 1.0 - SDR Video",
    },
    {
        "name": "hdr_rec2020_pq_1000nits",
        "ident": "kAcesLutHdrRec2020Pq1000Nits",
        "display": "Rec.2100-PQ - Display",
        "view": "ACES 1.1 - HDR Video (1000 nits & Rec.2020 lim)",
    },
]

from __future__ import annotations

import struct
import time
from pathlib import Path

import numpy as np
import PyOpenColorIO as ocio  # type: ignore[reportMissingImports]




# Small enough to iterate quickly while testing the pipeline. Use 64 for production.
LUT_SIZE = 33
DDS_OUTPUT_DIR = Path(__file__).resolve().parent.parent / "Editor" / "Render" / "Data"

DDS_MAGIC = b"DDS "
DDS_FOURCC = 0x00000004
DDS_HEADER_FLAGS_TEXTURE = 0x00001007
DDS_HEADER_FLAGS_VOLUME = 0x00800000
DDS_SURFACE_FLAGS_TEXTURE = 0x00001000
DDS_FLAGS_VOLUME = 0x00200000
DDS_DX10_FOURCC = 0x30315844
DXGI_FORMAT_R16G16B16A16_FLOAT = 10
DDS_DIMENSION_TEXTURE3D = 4

def create_config() -> ocio.Config:
    try:
        return ocio.Config.CreateFromBuiltinConfig(CONFIG_NAME)
    except Exception as exc:
        builtins = [name for name, *_ in ocio.BuiltinConfigRegistry().getBuiltinConfigs()]
        raise RuntimeError(
            f"OCIO built-in config '{CONFIG_NAME}' is unavailable. Available configs: {builtins}"
        ) from exc


def create_processor(config: ocio.Config, display: str, view: str) -> ocio.CPUProcessor:
    transform = ocio.DisplayViewTransform(
        src=PROCESSOR_INPUT_SPACE,
        display=display,
        view=view,
    )
    return config.getProcessor(transform).getDefaultCPUProcessor()


def apply_processor(processor: ocio.CPUProcessor, rgb: np.ndarray) -> np.ndarray:
    # applyRGB returns a new Python list and leaves the input list unchanged.
    return np.array(processor.applyRGB(rgb.astype(np.float32).tolist()), dtype=np.float32)


def bake_lut_rgba(processor: ocio.CPUProcessor, size: int) -> np.ndarray:
    """Bake a 3D LUT in raw ACEScct [0,1] domain, packed as RGBA32F per texel.

    Returns a flat float32 array of length size*size*size*4. Voxel order matches a
    standard 3D texture upload: x = R fastest, then y = G, then z = B. The alpha
    channel is 1.0 padding so the buffer can be uploaded as RGBA32F directly.
    """
    out = np.empty(size * size * size * 4, dtype=np.float32)
    denom = float(size - 1)
    write = 0
    for b in range(size):
        for g in range(size):
            for r in range(size):
                acescct = np.array([r / denom, g / denom, b / denom], dtype=np.float32)
                rgb = apply_processor(processor, acescct)
                out[write + 0] = rgb[0]
                out[write + 1] = rgb[1]
                out[write + 2] = rgb[2]
                out[write + 3] = 1.0
                write += 4
    return out


def write_lut_dds(path: Path, lut: np.ndarray, size: int) -> None:
    """Write a single-mip 3D DDS using DXGI_FORMAT_R16G16B16A16_FLOAT."""
    texels = size * size * size
    expected_values = texels * 4
    if lut.size != expected_values:
        raise ValueError(f"Expected {expected_values} float values for {size}^3 RGBA LUT, got {lut.size}")

    payload = lut.astype(np.dtype("<f2"), copy=False).tobytes(order="C")
    expected_payload_size = texels * 4 * 2
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
        DXGI_FORMAT_R16G16B16A16_FLOAT,
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


def _format_floats(values: np.ndarray, per_line: int = 8, indent: str = "    ") -> str:
    flat = values.reshape(-1)
    lines = []
    for i in range(0, len(flat), per_line):
        chunk = flat[i:i + per_line]
        lines.append(indent + ", ".join(f"{v:.9f}f" for v in chunk) + ",")
    return "\n".join(lines)


def write_aces_luts_header(path: Path, baked: list[tuple[dict, np.ndarray]]) -> None:
    """Emit Editor/Render/ACESLuts.hpp in the same constexpr style as Tables.hpp.

    Each LUT is a flat RGBA32F float[size*size*size*4] ready for direct 3D texture
    upload. The companion BT.709 -> AP1 Bradford matrix is included for the runtime
    shader's analytic shaper.
    """
    size = LUT_SIZE
    total = size * size * size * 4

    lines: list[str] = []
    lines.append(f"// Generated by Scripts/{Path(__file__).name}")
    lines.append(f"// OCIO config: {CONFIG_NAME}")
    lines.append("// Runtime pipeline (BT.709 -> AP1 Bradford and ACEScct encode are owned by the shader,")
    lines.append("// see Editor/Shaders/IPostprocess.slang):")
    lines.append("//   exposedLinearBT709")
    lines.append("//     -> BT709_AP1_BFD * rgb")
    lines.append("//     -> ACEScct encode")
    lines.append("//     -> saturate to [0,1] for LUT sampling")
    lines.append("//     -> sample one of the kAcesLut* tables")
    lines.append("//")
    lines.append("// Each LUT is a 3D texture with extent (kAcesLutSize)^3, packed RGBA32F.")
    lines.append("// Voxel order matches a standard 3D texture upload: x = R fastest,")
    lines.append("// then y = G, then z = B. Alpha is 1.0 padding.")
    lines.append("// See Scripts/ColorPrimaries.ipynb for the BT.709/D65 -> AP1/D60 derivation.")
    lines.append("#pragma once")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append(f"constexpr uint32_t kAcesLutSize = {size};")
    lines.append(f"constexpr uint32_t kAcesLutTexelCount = {size * size * size};")
    lines.append(f"constexpr uint32_t kAcesLutFloatCount = {total};")
    lines.append("")

    for view, lut in baked:
        lines.append(f"// View: {view['display']} / {view['view']}")
        lines.append("// Input: ACEScct in [0,1]; Output: encoded display RGB ready for present.")
        lines.append(f"constexpr float /* RGBA32F */ {view['ident']}[kAcesLutFloatCount] = {{")
        lines.append(_format_floats(lut, per_line=8))
        lines.append("};")
        lines.append("")

    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    header_path = Path(__file__).resolve().parent.parent / "Editor" / "Render" / "ACESLuts.hpp"

    config = create_config()

    print(f"Using OCIO {ocio.__version__}")
    print(f"Config: {CONFIG_NAME}")
    print(f"Scene input:     {SCENE_INPUT_SPACE}")
    print(f"Processor input: {PROCESSOR_INPUT_SPACE}")
    print("Shaper: BT.709/D65 -> AP1/D60 Bradford -> ACEScct (raw [0,1] LUT domain)")
    print(f"Size:   {LUT_SIZE}^3, baked RGBA32F, DDS RGBA16F")

    baked: list[tuple[dict, np.ndarray]] = []
    for view in VIEWS:
        print(f"\nBaking {view['name']}: {view['display']} / {view['view']}")
        processor = create_processor(config, view["display"], view["view"])

        start = time.perf_counter()
        lut = bake_lut_rgba(processor, LUT_SIZE)
        elapsed = time.perf_counter() - start

        size_kb = lut.nbytes / 1024.0
        print(f"    {lut.size} floats, {size_kb:.1f} KiB, {elapsed:.2f}s")
        dds_path = DDS_OUTPUT_DIR / f"aces_{view['name']}.dds"
        write_lut_dds(dds_path, lut, LUT_SIZE)
        print(f"    wrote {dds_path}")
        baked.append((view, lut))

    write_aces_luts_header(header_path, baked)
    print(f"\nwrote {header_path}")


if __name__ == "__main__":
    main()
