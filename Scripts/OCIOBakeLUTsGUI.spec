# -*- mode: python ; coding: utf-8 -*-

from pathlib import Path
from importlib.util import find_spec
import sys

from PyInstaller.utils.hooks import collect_all, collect_dynamic_libs, collect_submodules


spec_dir = Path(SPECPATH)

if sys.version_info[:2] != (3, 11):
    raise RuntimeError(
        f"Build OCIOBakeLUTs with Python 3.11, not Python {sys.version_info.major}.{sys.version_info.minor}. "
        "For uv, run: uv run --python 3.11 --with pyinstaller --with numpy --with opencolorio "
        "pyinstaller --noconfirm --clean OCIOBakeLUTsGUI.spec"
    )

datas = []
binaries = []
hiddenimports = [
    "tkinter",
    "tkinter.filedialog",
    "tkinter.messagebox",
    "tkinter.ttk",
]

for package in ("numpy", "PyOpenColorIO"):
    if find_spec(package) is None:
        raise RuntimeError(
            f"Missing Python package '{package}'. Run PyInstaller from the same environment "
            "that can import numpy and PyOpenColorIO."
        )

    hiddenimports.append(package)
    hiddenimports += collect_submodules(package)
    binaries += collect_dynamic_libs(package)

    package_datas, package_binaries, package_hiddenimports = collect_all(package)
    datas += package_datas
    binaries += package_binaries
    hiddenimports += package_hiddenimports

hiddenimports = sorted(set(hiddenimports))


a = Analysis(
    [str(spec_dir / "OCIOBakeLUTsGUI.py")],
    pathex=[str(spec_dir)],
    binaries=binaries,
    datas=datas,
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name="OCIOBakeLUTs",
    debug=False,
    bootloader_ignore_signals=False,
    strip=True,
    upx=True,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon="NONE",
)

coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=True,
    upx_exclude=[],
    name="OCIOBakeLUTs",
)
