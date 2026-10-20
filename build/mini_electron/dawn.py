# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.
"""Pinned standalone Dawn build used by the renderer-local WebGPU service."""
from __future__ import annotations

import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from xml.sax.saxutils import escape

DAWN_REVISION = "73dbf565079c89a531e6e01c4e8fc048a8a9660b"
DAWN_SHA256 = "18a461ebadea4a2b06eb87c5237acbe85a659bdec1eb0bcdcfc14f3c6648c234"
# These are Dawn DEPS revisions. In particular, do not substitute the engine's
# older Abseil: its containers and configuration are not Dawn's ABI.
_DEPENDENCIES = (
    ("abseil-cpp", "https://chromium.googlesource.com/chromium/src/third_party/abseil-cpp", "f81f6c011baf9b0132a5594c034fe0060820711d", "2bf1dc89e586105c3f38639c9077391f4a4023e69d5565e7ea0ccb0c496af710"),
    ("jinja2", "https://chromium.googlesource.com/chromium/src/third_party/jinja2", "e2d024354e11cc6b041b0cff032d73f0c7e43a07", "b5d3bb26aba286b750bb7deecc12511622881bcbec2b7daf07595cb3f508f1f6"),
    ("markupsafe", "https://chromium.googlesource.com/chromium/src/third_party/markupsafe", "0bad08bb207bbfc1d6f3bbc82b9242b0c50e5794", "db58248cccf951df0c0a368e6ba15052e0fc678aebc4ae4de77c2dd2e7940fb2"),
    # Dawn's CMake always adds this header-only target, even without SPIR-V.
    ("spirv-headers/src", "https://chromium.googlesource.com/external/github.com/KhronosGroup/SPIRV-Headers", "996c728cf7dcfb29845cfa15222822318f047810", "885130af46be5c9543bb92f47b51d2100677bf1feb9bc98e8bfba588e5f6a3a8"),
)


def _archive(root: Path, name: str, url: str, checksum: str) -> Path:
    cache = root / ".build-tools"
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / name
    if not archive.is_file():
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=cache, delete=False) as output:
                temporary = Path(output.name)
                print(f"+ download {url}", flush=True)
                with urllib.request.urlopen(url, timeout=180) as response:
                    shutil.copyfileobj(response, output)
            temporary.replace(archive)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    digest = hashlib.sha256()
    with archive.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    actual = digest.hexdigest()
    if actual != checksum:
        raise RuntimeError(f"Dawn checksum mismatch for {archive}: expected {checksum}, got {actual}")
    return archive


def _source(archive: Path, source: Path, revision: str) -> None:
    marker = source / ".mini-electron-revision"
    if marker.is_file() and marker.read_text(encoding="utf-8").strip() == revision:
        return
    # An archive may have been manually unpacked before the first bootstrap.
    # Adopt it only if every byte and every path matches the verified archive.
    # Never stamp an arbitrary populated vendor tree with the requested revision.
    with tarfile.open(archive, "r:gz") as package:
        if source.exists() and any(source.iterdir()):
            expected = set()
            matches = True
            for member in package.getmembers():
                relative = Path(member.name)
                if relative.is_absolute() or ".." in relative.parts:
                    raise RuntimeError(f"Unsafe Dawn archive path: {member.name}")
                path = source / relative
                if member.isdir():
                    continue
                expected.add(relative.as_posix())
                if member.issym():
                    matches = matches and path.is_symlink() and str(path.readlink()) == member.linkname
                elif member.isfile():
                    original = package.extractfile(member)
                    matches = matches and not path.is_symlink() and path.is_file() and path.read_bytes() == original.read()
                else:
                    matches = False
                if not matches:
                    break
            if matches:
                actual = {path.relative_to(source).as_posix() for path in source.rglob("*") if path.is_file() or path.is_symlink()}
                matches = actual == expected
            if not matches:
                raise RuntimeError(f"Dawn source at {source} is not the pinned revision {revision}; move it aside before rebuilding")
        else:
            source.parent.mkdir(parents=True, exist_ok=True)
            with tempfile.TemporaryDirectory(dir=source.parent) as temporary:
                unpacked = Path(temporary) / "source"
                unpacked.mkdir()
                options = {"filter": "data"} if hasattr(tarfile, "data_filter") else {}
                package.extractall(unpacked, **options)
                if source.exists():
                    source.rmdir()
                unpacked.replace(source)
    marker.write_text(revision + "\n", encoding="utf-8")


def ensure_dawn(root: Path) -> Path:
    source = root / "third_party/dawn"
    archive = _archive(root, f"dawn-{DAWN_REVISION}.tar.gz", f"https://dawn.googlesource.com/dawn/+archive/{DAWN_REVISION}.tar.gz", DAWN_SHA256)
    _source(archive, source, DAWN_REVISION)
    for relative, repository, revision, checksum in _DEPENDENCIES:
        name = relative.split("/")[0]
        archive = _archive(root, f"dawn-{name}-{revision}.tar.gz", f"{repository}/+archive/{revision}.tar.gz", checksum)
        _source(archive, source / "third_party" / relative, revision)
    return source


def _run(command: list[str], root: Path) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, cwd=root, check=True)


def build_dawn(root: Path, jobs: int, *, clang_base: Path | None = None,
               visual_studio: Path | None = None, msvc_version: str | None = None,
               windows_sdk: str | None = None) -> Path:
    cmake = shutil.which("cmake")
    if not cmake:
        raise RuntimeError("Dawn requires CMake 3.24 or newer on PATH (install CMake before building WebGPU)")
    source = ensure_dawn(root)
    windows = sys.platform == "win32"
    if not windows and sys.platform != "darwin":
        raise RuntimeError("The native Dawn bootstrap currently supports macOS arm64 and Windows x64")
    platform = "windows-x64" if windows else "darwin-arm64"
    output = root / ".build-tools" / f"dawn-{DAWN_REVISION}-{platform}"
    command = [cmake, "-S", str(root / "build/mini_electron/dawn"), "-B", str(output),
               f"-DMINI_ELECTRON_DAWN_SOURCE={source}", "-DCMAKE_BUILD_TYPE=Release",
               f"-DPython3_EXECUTABLE={sys.executable}", "-DBUILD_SHARED_LIBS=OFF"]
    disabled = (
        "DAWN_FETCH_DEPENDENCIES", "DAWN_ENABLE_D3D11", "DAWN_ENABLE_NULL",
        "DAWN_ENABLE_DESKTOP_GL", "DAWN_ENABLE_OPENGLES", "DAWN_ENABLE_VULKAN",
        "DAWN_ENABLE_SPIRV_VALIDATION", "DAWN_USE_WAYLAND", "DAWN_USE_X11",
        "DAWN_USE_GLFW", "DAWN_USE_BUILT_DXC", "DAWN_BUILD_SAMPLES",
        "DAWN_BUILD_TESTS", "DAWN_BUILD_NODE_BINDINGS", "DAWN_ENABLE_SWIFTSHADER",
        "DAWN_BUILD_BENCHMARKS", "DAWN_BUILD_PROTOBUF", "DAWN_ENABLE_INSTALL",
        "TINT_ENABLE_INSTALL", "TINT_BUILD_CMD_TOOLS", "TINT_BUILD_SPV_READER",
        "TINT_BUILD_GLSL_WRITER", "TINT_BUILD_GLSL_VALIDATOR", "TINT_BUILD_SPV_WRITER",
        "TINT_BUILD_FUZZERS", "TINT_BUILD_IR_FUZZER", "TINT_BUILD_BENCHMARKS",
        "TINT_BUILD_TESTS", "TINT_BUILD_TINTD", "TINT_BUILD_IR_BINARY",
    )
    command += [f"-D{option}=OFF" for option in disabled]
    command += ["-DTINT_BUILD_WGSL_READER=ON", "-DTINT_BUILD_WGSL_WRITER=ON",
                f"-DDAWN_ENABLE_METAL={'OFF' if windows else 'ON'}",
                f"-DTINT_BUILD_MSL_WRITER={'OFF' if windows else 'ON'}",
                f"-DDAWN_ENABLE_D3D12={'ON' if windows else 'OFF'}",
                f"-DTINT_BUILD_HLSL_WRITER={'ON' if windows else 'OFF'}"]
    if windows:
        if not all((visual_studio, msvc_version, windows_sdk)):
            raise RuntimeError("Dawn D3D12 requires the selected Visual Studio, MSVC version, and Windows SDK")
        command += ["-G", "Visual Studio 17 2022", "-A", "x64",
                    f"-DCMAKE_GENERATOR_INSTANCE={visual_studio}", f"-T=v143,version={msvc_version}",
                    f"-DCMAKE_SYSTEM_VERSION={windows_sdk}", "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"]
    else:
        if clang_base is None:
            raise RuntimeError("Dawn Metal requires the engine's Xcode clang toolchain")
        sdk = subprocess.check_output(["xcrun", "--sdk", "macosx", "--show-sdk-path"], text=True).strip()
        command += ["-G", "Ninja", f"-DCMAKE_C_COMPILER={clang_base / 'bin/clang'}",
                    f"-DCMAKE_CXX_COMPILER={clang_base / 'bin/clang++'}", "-DCMAKE_OSX_ARCHITECTURES=arm64",
                    f"-DCMAKE_OSX_SYSROOT={sdk}", "-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0",
                    "-DCMAKE_CXX_FLAGS=-stdlib=libc++"]
    _run(command, root)
    _run([cmake, "--build", str(output), "--config", "Release", "--target", "mini_electron_dawn", "--parallel", str(jobs)], root)
    archives = [Path(line) for line in (output / "archives-Release.txt").read_text(encoding="utf-8").splitlines() if line]
    if not archives or any(not archive.is_file() for archive in archives):
        raise RuntimeError("Dawn did not produce its complete static archive dependency closure")
    if windows:
        libraries = ";".join(escape(str(archive)) for archive in archives)
        (output / "dawn-libraries.props").write_text(
            '<Project><ItemDefinitionGroup><Link><AdditionalDependencies>' + libraries + ';%(AdditionalDependencies)</AdditionalDependencies></Link></ItemDefinitionGroup></Project>\n', encoding="utf-8")
    else:
        lines = ["mini_electron_dawn_libraries = ["]
        lines += ['  "//' + archive.relative_to(root).as_posix() + '",' for archive in archives]
        (output / "libraries.gni").write_text("\n".join(lines + ["]", ""]), encoding="utf-8")
    return output
