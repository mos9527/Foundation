# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "numpy",
#   "opencolorio",
# ]
# ///

from __future__ import annotations

import os
import re
import struct
import time
from pathlib import Path

import numpy as np
import PyOpenColorIO as ocio  # type: ignore[reportMissingImports]

DEFAULT_BLENDER_OCIO_CONFIG = Path(__file__).resolve().parent / "blender-5-1-colormanagement" / "config.ocio"
BLENDER_OCIO_CONFIG = Path(os.environ.get("FOUNDATION_BLENDER_OCIO_CONFIG", DEFAULT_BLENDER_OCIO_CONFIG))
CONFIG_NAME = str(BLENDER_OCIO_CONFIG)
PROCESSOR_INPUT_SPACE = "ACEScct"
SCENE_INPUT_SPACE = "Linear Rec.709 (sRGB)"

LUT_OUTPUTS = [
    {
        "kind": "sdr",
        "display": "sRGB",
        "default_view": "ACES 1.3",
        "default_look": None,
        "array_name": "kViewLUTsSdr",
        "count_name": "kViewLUTSdrCount",
        "default_name": "kDefaultViewLUTSdr",
    },
    {
        "kind": "hdr",
        "display": "Rec.2100-PQ",
        "default_view": "ACES 1.3 - HDR 1000 nits",
        "default_look": None,
        "array_name": "kViewLUTsHdr",
        "count_name": "kViewLUTHdrCount",
        "default_name": "kDefaultViewLUTHdr",
    },
]

FILMIC_LOOKS = [
    "Very High Contrast",
    "High Contrast",
    "Medium High Contrast",
    "Medium Contrast",
    "Medium Low Contrast",
    "Low Contrast",
    "Very Low Contrast",
]

AGX_LOOKS = [
    "AgX - Punchy",
    "AgX - Greyscale",
    "AgX - Very High Contrast",
    "AgX - High Contrast",
    "AgX - Medium High Contrast",
    "AgX - Base Contrast",
    "AgX - Medium Low Contrast",
    "AgX - Low Contrast",
    "AgX - Very Low Contrast",
]

LUT_SELECTIONS: dict[str, list[tuple[str, list[str | None]]]] = {
    "sdr": [
        ("Standard", [None]),
        ("ACES 1.3", [None, "ACES 1.3 - Reference Gamut Compression"]),
        ("ACES 2.0", [None, "ACES 2.0 - Reference Gamut Compression"]),
        ("AgX", [None, *AGX_LOOKS]),
        ("Filmic", [None, *FILMIC_LOOKS]),
    ],
    "hdr": [
        ("Standard", [None]),
        ("ACES 1.3 - HDR 1000 nits", [None, "ACES 1.3 - Reference Gamut Compression"]),
        ("ACES 2.0 - HDR 1000 nits", [None, "ACES 2.0 - Reference Gamut Compression"]),
        ("AgX - HDR 1000 nits", [None, *AGX_LOOKS]),
    ],
}

LUT_SIZE = 32
DDS_OUTPUT_DIR = Path(__file__).resolve().parent.parent / "Editor" / "Render" / "Data"
VIEW_LUTS_HEADER_PATH = Path(__file__).resolve().parent.parent / "Editor" / "Render" / "ViewLUTs.hpp"
WRITE_CPP_HEADER = False

DDS_MAGIC = b"DDS "
DDS_FOURCC = 0x00000004
DDS_HEADER_FLAGS_TEXTURE = 0x00001007
DDS_HEADER_FLAGS_VOLUME = 0x00800000
DDS_SURFACE_FLAGS_TEXTURE = 0x00001000
DDS_FLAGS_VOLUME = 0x00200000
DDS_DX10_FOURCC = 0x30315844
DXGI_FORMAT_R10G10B10A2_UNORM = 24
DDS_DIMENSION_TEXTURE3D = 4

def create_config() -> ocio.Config:
    try:
        return ocio.Config.CreateFromFile(str(BLENDER_OCIO_CONFIG))
    except Exception as exc:
        raise RuntimeError(
            f"Blender OCIO config '{BLENDER_OCIO_CONFIG}' is unavailable. "
            "Set FOUNDATION_BLENDER_OCIO_CONFIG to Blender's datafiles/colormanagement/config.ocio."
        ) from exc


def _slug(value: str) -> str:
    return re.sub(r"_+", "_", re.sub(r"[^a-z0-9]+", "_", value.lower())).strip("_")


def _ident(value: str) -> str:
    return "".join(part.capitalize() for part in _slug(value).split("_"))


def _cpp_string(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def build_view_specs(config: ocio.Config) -> list[dict]:
    available_looks = {look.getName() for look in config.getLooks()}
    specs: list[dict] = []
    seen_names: set[str] = set()

    for output in LUT_OUTPUTS:
        display = output["display"]
        available_views = set(config.getViews(display))
        for view, looks in LUT_SELECTIONS[output["kind"]]:
            if view not in available_views:
                raise ValueError(f"Display '{display}' has no view '{view}'. Available views: {sorted(available_views)}")
            for look in looks:
                if look is not None and look not in available_looks:
                    raise ValueError(f"OCIO config has no look '{look}'. Available looks: {sorted(available_looks)}")

                look_slug = "no_look" if look is None else f"look_{_slug(look)}"
                name = f"view_lut_{output['kind']}_{_slug(view)}_{look_slug}"
                if name in seen_names:
                    raise ValueError(f"Duplicate LUT name generated: {name}")
                seen_names.add(name)

                label = f"{view} / {'No Look' if look is None else look}"
                specs.append({
                    "kind": output["kind"],
                    "display": display,
                    "view": view,
                    "look": look,
                    "name": name,
                    "ident": f"kViewLut{_ident(output['kind'])}{_ident(view)}{_ident(look_slug)}",
                    "label": label,
                    "path": f"data/assets/{name}.dds",
                })

    return specs


def create_processor(config: ocio.Config, view_spec: dict) -> ocio.CPUProcessor:
    display = view_spec["display"]
    view = view_spec["view"]
    look = view_spec.get("look")

    available_views = set(config.getViews(display))
    if view not in available_views:
        raise ValueError(f"Display '{display}' has no view '{view}'. Available views: {sorted(available_views)}")

    display_transform = ocio.DisplayViewTransform(
        src=PROCESSOR_INPUT_SPACE,
        display=display,
        view=view,
    )
    if look is None:
        return config.getProcessor(display_transform).getDefaultCPUProcessor()

    available_looks = {config_look.getName() for config_look in config.getLooks()}
    if look not in available_looks:
        raise ValueError(f"OCIO config has no look '{look}'. Available looks: {sorted(available_looks)}")

    look_transform = ocio.LookTransform(
        src=PROCESSOR_INPUT_SPACE,
        dst=PROCESSOR_INPUT_SPACE,
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


def write_view_luts_header(path: Path, views: list[dict]) -> None:
    lines: list[str] = []
    lines.append(f"// Generated by Scripts/{Path(__file__).name}")
    lines.append(f"// OCIO config: {CONFIG_NAME}")
    lines.append("#pragma once")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append("struct ViewLUTEntry")
    lines.append("{")
    lines.append("    const char* label;")
    lines.append("    const char* path;")
    lines.append("};")
    lines.append("")

    for output in LUT_OUTPUTS:
        output_views = [view for view in views if view["kind"] == output["kind"]]
        if not output_views:
            raise ValueError(f"No LUTs generated for {output['kind']}")

        lines.append(f"inline constexpr ViewLUTEntry {output['array_name']}[] = {{")
        for view in output_views:
            lines.append(f"    {{\"{_cpp_string(view['label'])}\", \"{_cpp_string(view['path'])}\"}},")
        lines.append("};")
        lines.append("")

        lines.append(
            f"inline constexpr int {output['count_name']} = "
            f"static_cast<int>(sizeof({output['array_name']}) / sizeof({output['array_name']}[0]));"
        )
        default_index = next(
            (
                index for index, view in enumerate(output_views)
                if view["view"] == output["default_view"] and view["look"] == output["default_look"]
            ),
            None,
        )
        if default_index is None:
            raise ValueError(f"Default LUT not found for {output['kind']}")
        lines.append(f"inline constexpr int {output['default_name']} = {default_index};")
        lines.append("")

    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    header_path = Path(__file__).resolve().parent.parent / "Editor" / "Render" / "ACESLuts.hpp"

    config = create_config()
    views = build_view_specs(config)

    print(f"Using OCIO {ocio.__version__}")
    print(f"Config: {CONFIG_NAME}")
    print(f"Scene input:     {SCENE_INPUT_SPACE}")
    print(f"Processor input: {PROCESSOR_INPUT_SPACE}")
    print("Shaper: BT.709/D65 -> AP1/D60 Bradford -> ACEScct (raw [0,1] LUT domain)")
    print(f"Size:   {LUT_SIZE}^3, baked RGBA32F, DDS RGB10A2 UNORM")
    print(f"Views:  {len(views)} ({sum(1 for view in views if view['kind'] == 'sdr')} SDR, "
          f"{sum(1 for view in views if view['kind'] == 'hdr')} HDR)")

    DDS_OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    for stale in DDS_OUTPUT_DIR.glob("view_lut_*.dds"):
        stale.unlink()

    baked: list[tuple[dict, np.ndarray]] = []
    for view in views:
        look = "No Look" if view["look"] is None else view["look"]
        print(f"\nBaking {view['name']}: {view['display']} / {view['view']} / {look}")
        processor = create_processor(config, view)

        start = time.perf_counter()
        lut = bake_lut_rgba(processor, LUT_SIZE)
        elapsed = time.perf_counter() - start

        size_kb = lut.nbytes / 1024.0
        print(f"    {lut.size} floats, {size_kb:.1f} KiB, {elapsed:.2f}s")
        dds_path = DDS_OUTPUT_DIR / f"{view['name']}.dds"
        write_lut_dds(dds_path, lut, LUT_SIZE)
        print(f"    wrote {dds_path}")
        if WRITE_CPP_HEADER:
            baked.append((view, lut))

    write_view_luts_header(VIEW_LUTS_HEADER_PATH, views)
    print(f"\nwrote {VIEW_LUTS_HEADER_PATH}")

    if WRITE_CPP_HEADER:
        write_aces_luts_header(header_path, baked)
        print(f"\nwrote {header_path}")


if __name__ == "__main__":
    main()
