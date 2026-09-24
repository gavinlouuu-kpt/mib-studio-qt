# -*- mode: python ; coding: utf-8 -*-
"""
PyInstaller spec for HDF5 Export GUI (MIB Studio Tools).

Run from tools/ with: pyinstaller hdf5_export_app/hdf5_export.spec
Or from tools/hdf5_export_app/ with: pyinstaller hdf5_export.spec

Script and core modules live in repo scripts/; this spec points at them.
"""

from pathlib import Path

block_cipher = None

# Build script is run from tools/, so cwd is tools/
tools_dir = Path.cwd().resolve()
repo_root = tools_dir.parent
scripts_dir = repo_root / "scripts"

script_path = scripts_dir / "hdf5_export_app.py"
if not script_path.exists():
    raise FileNotFoundError(
        f"Script not found: {script_path}\n"
        f"Repo root: {repo_root}\n"
        f"Scripts dir: {scripts_dir}"
    )

a = Analysis(
    [str(script_path)],
    pathex=[str(scripts_dir)],
    binaries=[],
    datas=[],
    hiddenimports=[
        "h5py",
        "h5py._hl",
        "h5py.h5ac",
        "numpy",
        "numpy.core._methods",
        "numpy.lib.format",
        "cv2",
        "PySide6.QtCore",
        "PySide6.QtGui",
        "PySide6.QtWidgets",
        "export_hdf5",
        "export_worker",
        "hdf_export_engine",
    ],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    win_no_prefer_redirects=False,
    win_private_assemblies=False,
    cipher=block_cipher,
    noarchive=False,
)

pyz = PYZ(a.pure, a.zipped_data, cipher=block_cipher)

# Build script passes --distpath tools/dist so all tools go to tools/dist/
exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.zipfiles,
    a.datas,
    [],
    name="hdf5_export_app",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=None,
)
