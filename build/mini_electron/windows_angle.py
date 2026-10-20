# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from .angle import ensure_angle


def _tool(name: str) -> str:
    configured = os.environ.get(f"MINI_ELECTRON_{name.upper()}")
    executable = shutil.which(configured or name)
    if executable:
        return executable
    raise RuntimeError(
        f"Windows WebGL requires {name}.exe. Install GN and Ninja (for example "
        "from Chromium depot_tools), add them to PATH, or set "
        f"MINI_ELECTRON_{name.upper()} to its executable path. WebGL cannot be skipped."
    )


def _junction(link: Path, source: Path) -> None:
    if link.exists():
        if not link.is_junction() or link.resolve() != source.resolve():
            raise RuntimeError(f"ANGLE source junction does not point to {source}: {link}")
        return
    link.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        ["cmd.exe", "/d", "/c", "mklink", "/J", str(link), str(source)],
        check=False,
    )
    if result.returncode:
        raise RuntimeError(f"Cannot create ANGLE source junction {link} -> {source}")


def build_angle(
    root: Path,
    output: Path,
    visual_studio: Path,
    msvc_version: str,
    sdk_version: str,
    llvm_bin: Path,
    jobs: int,
) -> Path:
    """Build upstream ANGLE D3D11 as one native MSBuild-compatible /MT archive."""
    if sys.version_info < (3, 12):
        raise RuntimeError("Windows WebGL source bootstrap requires Python 3.12 or newer.")
    gn = _tool("gn")
    ninja = _tool("ninja")
    angle = ensure_angle(root)

    # The application is still built by native MSBuild. This small independent
    # source root avoids requiring Chromium's omitted Windows GN infrastructure.
    # Junctions need neither administrator rights nor Windows Developer Mode and
    # keep all ANGLE compilation on the checksum-pinned original source tree.
    source_root = output / "angle-src"
    templates = root / "build" / "windows" / "angle"
    for template in templates.rglob("*"):
        if not template.is_file():
            continue
        destination = source_root / template.relative_to(templates)
        destination.parent.mkdir(parents=True, exist_ok=True)
        if not destination.is_file() or destination.read_bytes() != template.read_bytes():
            shutil.copyfile(template, destination)
    _junction(source_root / "third_party" / "angle", angle)
    _junction(source_root / "third_party" / "zlib" / "src", root / "third_party" / "zlib")

    msvc = visual_studio / "VC" / "Tools" / "MSVC" / msvc_version
    kits = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Windows Kits" / "10"
    includes = [msvc / "include", msvc / "atlmfc" / "include"]
    includes.extend(kits / "Include" / sdk_version / part for part in ("ucrt", "shared", "um", "winrt", "cppwinrt"))
    libraries = [msvc / "lib" / "x64", msvc / "atlmfc" / "lib" / "x64"]
    libraries.extend(kits / "Lib" / sdk_version / part / "x64" for part in ("ucrt", "um"))
    environment = os.environ.copy()
    environment["INCLUDE"] = os.pathsep.join(str(path) for path in includes)
    environment["LIB"] = os.pathsep.join(str(path) for path in libraries)
    environment["PATH"] = os.pathsep.join(
        (
            str(Path(sys.executable).parent),
            str(llvm_bin),
            str(msvc / "bin" / "Hostx64" / "x64"),
            str(kits / "bin" / sdk_version / "x64"),
            environment.get("PATH", ""),
        )
    )
    # Ninja's MSVC dependency parser must see English /showIncludes output.
    environment["VSLANG"] = "1033"
    build_dir = output / "angle"
    arguments = "mini_electron_llvm_bin=" + json.dumps(llvm_bin.as_posix())
    commands = (
        [gn, "gen", str(build_dir), f"--root={source_root}", f"--script-executable={sys.executable}", f"--args={arguments}"],
        [ninja, "-C", str(build_dir), "-j", str(jobs), "mini_electron_angle"],
    )
    for command in commands:
        print("[windows] Building ANGLE D3D11: " + subprocess.list2cmdline(command))
        result = subprocess.run(command, cwd=source_root, env=environment, check=False)
        if result.returncode:
            raise RuntimeError(f"ANGLE D3D11 build failed with exit code {result.returncode}")
    library = build_dir / "mini_electron_angle.lib"
    if not library.is_file() or not library.stat().st_size:
        raise RuntimeError(f"ANGLE build did not produce {library}")
    return library
