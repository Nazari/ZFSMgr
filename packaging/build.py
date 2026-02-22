#!/usr/bin/env python3
"""Build standalone ZFSMgr binary with PyInstaller.

Creates a platform artifact under dist/packages:
- Linux/macOS: tar.gz
- Windows: zip
"""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
APP = ROOT / "app.py"
DIST = ROOT / "dist"
BUILD = ROOT / "build"
PACKAGES = DIST / "packages"
NAME = "ZFSMgr"


def _run(cmd: list[str]) -> None:
    proc = subprocess.run(cmd, cwd=ROOT)
    if proc.returncode != 0:
        raise SystemExit(proc.returncode)


def _add_data_arg(src: Path, dest: str) -> str:
    sep = ";" if os.name == "nt" else ":"
    return f"{src}{sep}{dest}"


def build() -> Path:
    if not APP.exists():
        raise SystemExit(f"Missing app entrypoint: {APP}")

    PACKAGES.mkdir(parents=True, exist_ok=True)
    cmd = [
        sys.executable,
        "-m",
        "PyInstaller",
        "--noconfirm",
        "--clean",
        "--onefile",
        "--windowed",
        "--name",
        NAME,
        "--add-data",
        _add_data_arg(ROOT / "locales", "locales"),
        "--collect-submodules",
        "pypsrp",
        "--collect-submodules",
        "paramiko",
        str(APP),
    ]
    _run(cmd)

    exe = DIST / (f"{NAME}.exe" if os.name == "nt" else NAME)
    if not exe.exists():
        raise SystemExit(f"Expected built binary not found: {exe}")
    return exe


def package_binary(binary: Path) -> Path:
    system = platform.system().lower()
    machine = platform.machine().lower().replace(" ", "_")
    version = "dev"
    base = f"{NAME}-{system}-{machine}-{version}"

    if os.name == "nt":
        out = PACKAGES / f"{base}.zip"
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
            zf.write(binary, arcname=binary.name)
    else:
        out = PACKAGES / f"{base}.tar.gz"
        with tarfile.open(out, "w:gz") as tf:
            tf.add(binary, arcname=binary.name)
    return out


def clean() -> None:
    for p in (BUILD, ROOT / f"{NAME}.spec"):
        if p.exists():
            if p.is_dir():
                shutil.rmtree(p, ignore_errors=True)
            else:
                p.unlink(missing_ok=True)


def main() -> None:
    binary = build()
    artifact = package_binary(binary)
    clean()
    print(f"Built binary: {binary}")
    print(f"Packaged artifact: {artifact}")


if __name__ == "__main__":
    main()
