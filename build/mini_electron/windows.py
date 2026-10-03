# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import locale
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile

from .node_sources import generate_node_bootstraps
from .distribution import (
    copy_release_notices,
    copy_runtime_javascript,
    publish_distribution,
    release_metadata,
    staging_directory,
)


_MANIFEST = Path("build/mini_electron/windows/manifest.json")
_NASM_VERSION = "2.16.03"
_NASM_URL = (
    "https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/"
    "nasm-2.16.03-win64.zip"
)
_NASM_SHA256 = "3ee4782247bcb874378d02f7eab4e294a84d3d15f3f6ee2de2f47a46aa7226e6"
_VSNASM_REVISION = "17e29635060397a4a4a6bee578fd3ba089939e88"
_VSNASM_FILES = {
    "nasm.props": "ea9b80bea57f260afb174548bf0f0fbc4354646ba63fb80a74df3707bf0d1719",
    "nasm.targets": "c46548b55ce04ec8cab665dfe4f486420c110ca79b131a9dfc939ddf3d27c458",
    "nasm.xml": "954a6dd55d233fd1ccf58dd905ccedfe1762002c0d965e4b17bdd7ac89a10f04",
}
_NGHTTP2_VERSION = "1.61.0"
_NGHTTP2_SHA256 = "c0e660175b9dc429f11d25b9507a834fb752eea9135ab420bb7cb7e9dbcc9654"
_NGHTTP2_SOURCES = (
    "nghttp2_alpn.c",
    "nghttp2_buf.c",
    "nghttp2_callbacks.c",
    "nghttp2_debug.c",
    "nghttp2_extpri.c",
    "nghttp2_frame.c",
    "nghttp2_hd.c",
    "nghttp2_hd_huffman.c",
    "nghttp2_hd_huffman_data.c",
    "nghttp2_helper.c",
    "nghttp2_http.c",
    "nghttp2_map.c",
    "nghttp2_mem.c",
    "nghttp2_option.c",
    "nghttp2_outbound_item.c",
    "nghttp2_pq.c",
    "nghttp2_priority_spec.c",
    "nghttp2_queue.c",
    "nghttp2_ratelim.c",
    "nghttp2_rcbuf.c",
    "nghttp2_session.c",
    "nghttp2_stream.c",
    "nghttp2_submit.c",
    "nghttp2_time.c",
    "nghttp2_version.c",
    "sfparse.c",
)
_DEFAULT_FRONTEND = "examples/browser/index.html"
_DEFAULT_OMP_SOURCE = "../omp-desktop"
_WINDOW_TIMEOUT_SECONDS = 30.0
_CLOSE_TIMEOUT_SECONDS = 10.0


class BuildError(RuntimeError):
    pass


def _normalized_option(value: object) -> str:
    return str(value).replace("\\", "/").rstrip("/")


def _validate_options(args: object) -> None:
    unsupported: list[str] = []
    if bool(getattr(args, "gui", False)):
        unsupported.append("--gui")
    if getattr(args, "app_output", None) is not None and not bool(
        getattr(args, "omp_desktop", False)
    ):
        unsupported.append("--app-output without --omp-desktop")
    if getattr(args, "dist_output", None) is not None and not bool(
        getattr(args, "package_dist", False)
    ):
        unsupported.append("--dist-output without --package-dist")
    if getattr(args, "screenshot", None) is not None:
        unsupported.append("--screenshot")
    frontend = getattr(args, "frontend", _DEFAULT_FRONTEND)
    if _normalized_option(frontend) != _DEFAULT_FRONTEND:
        unsupported.append("--frontend")
    omp_source = getattr(args, "omp_source", _DEFAULT_OMP_SOURCE)
    if (
        _normalized_option(omp_source) != _DEFAULT_OMP_SOURCE
        and not bool(getattr(args, "omp_desktop", False))
    ):
        unsupported.append("--omp-source without --omp-desktop")
    if bool(getattr(args, "interactive", False)) and not (
        bool(getattr(args, "electron", False))
        or bool(getattr(args, "omp_desktop", False))
    ):
        unsupported.append("--interactive without --electron or --omp-desktop")
    if unsupported:
        joined = ", ".join(unsupported)
        raise BuildError(f"Windows does not support these options: {joined}")


def _resolve_inside(base: Path, value: object, label: str) -> Path:
    candidate = Path(str(value))
    if not candidate.is_absolute():
        candidate = base / candidate
    candidate = candidate.resolve()
    try:
        candidate.relative_to(base.resolve())
    except ValueError as error:
        raise BuildError(f"{label} must stay inside {base}: {value}") from error
    return candidate


def _load_manifest(root: Path) -> dict[str, object]:
    path = root / _MANIFEST
    if not path.is_file():
        raise BuildError(f"Windows source manifest is missing: {path}")
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise BuildError(f"Cannot read Windows source manifest {path}: {error}") from error
    if not isinstance(manifest, dict) or manifest.get("version") != 1:
        raise BuildError(f"Unsupported Windows source manifest version in {path}")
    projects = manifest.get("projects")
    if not isinstance(projects, list) or not projects:
        raise BuildError(f"Windows source manifest has no projects: {path}")
    required = (
        "entry_project",
        "built_executable",
        "package_executable",
        "embedded_resources",
        "asar_source",
        "msbuild",
    )
    missing = [key for key in required if key not in manifest]
    if missing:
        raise BuildError(f"Windows source manifest is missing: {', '.join(missing)}")
    return manifest


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _download(url: str, destination: Path) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": "mini-electron-build/1"})
    try:
        with urllib.request.urlopen(request, timeout=120) as response, destination.open("wb") as output:
            shutil.copyfileobj(response, output)
    except Exception as error:
        destination.unlink(missing_ok=True)
        raise BuildError(f"Failed to download required build tool {url}: {error}") from error


def _safe_extract_zip(archive: Path, destination: Path) -> None:
    destination_resolved = destination.resolve()
    with zipfile.ZipFile(archive) as package:
        for member in package.infolist():
            target = (destination / member.filename).resolve()
            try:
                target.relative_to(destination_resolved)
            except ValueError as error:
                raise BuildError(f"Unsafe path in {archive}: {member.filename}") from error
        package.extractall(destination)


def _safe_extract_tar(archive: Path, destination: Path) -> None:
    destination_resolved = destination.resolve()
    with tarfile.open(archive, "r:xz") as package:
        members = package.getmembers()
        for member in members:
            target = (destination / member.name).resolve()
            try:
                target.relative_to(destination_resolved)
            except ValueError as error:
                raise BuildError(f"Unsafe path in {archive}: {member.name}") from error
            if not (member.isfile() or member.isdir()):
                raise BuildError(f"Unsupported entry in {archive}: {member.name}")
        package.extractall(destination, members=members)


def _nasm_version(executable: Path) -> str | None:
    try:
        result = subprocess.run(
            [str(executable), "-v"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding=locale.getpreferredencoding(False),
            errors="replace",
        )
    except OSError:
        return None
    if result.returncode != 0:
        return None
    match = re.search(r"NASM version\s+([0-9.]+)", result.stdout)
    return match.group(1) if match else None


def _ensure_nasm(root: Path) -> Path:
    cached_root = root / ".build-tools" / f"nasm-{_NASM_VERSION}-windows-x64"
    cached_executable = cached_root / f"nasm-{_NASM_VERSION}" / "nasm.exe"
    if cached_executable.is_file() and _nasm_version(cached_executable) == _NASM_VERSION:
        return cached_executable.parent

    system_nasm = shutil.which("nasm.exe") or shutil.which("nasm")
    if system_nasm:
        executable = Path(system_nasm).resolve()
        if _nasm_version(executable) == _NASM_VERSION:
            return executable.parent

    tools = root / ".build-tools"
    tools.mkdir(parents=True, exist_ok=True)
    archive = tools / f"nasm-{_NASM_VERSION}-windows-x64.zip"
    temporary_archive = tools / f".{archive.name}.{os.getpid()}.tmp"
    print(f"[windows] Downloading NASM {_NASM_VERSION}...")
    _download(_NASM_URL, temporary_archive)
    actual_hash = _sha256(temporary_archive)
    if actual_hash != _NASM_SHA256:
        temporary_archive.unlink(missing_ok=True)
        raise BuildError(
            f"NASM archive checksum mismatch: expected {_NASM_SHA256}, got {actual_hash}"
        )
    os.replace(temporary_archive, archive)

    temporary_root = tools / f".{cached_root.name}.{os.getpid()}.tmp"
    if temporary_root.exists():
        shutil.rmtree(temporary_root)
    temporary_root.mkdir()
    try:
        _safe_extract_zip(archive, temporary_root)
        extracted = temporary_root / f"nasm-{_NASM_VERSION}" / "nasm.exe"
        if _nasm_version(extracted) != _NASM_VERSION:
            raise BuildError(f"Downloaded NASM package does not contain NASM {_NASM_VERSION}")
        if cached_root.exists():
            shutil.rmtree(cached_root)
        os.replace(temporary_root, cached_root)
    finally:
        if temporary_root.exists():
            shutil.rmtree(temporary_root)
    return cached_executable.parent


def _ensure_vsnasm(root: Path) -> Path:
    destination = root / ".build-tools" / f"vsnasm-{_VSNASM_REVISION[:8]}"
    if all(
        (destination / name).is_file() and _sha256(destination / name) == expected
        for name, expected in _VSNASM_FILES.items()
    ):
        return destination

    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.parent / f".{destination.name}.{os.getpid()}.tmp"
    if temporary.exists():
        shutil.rmtree(temporary)
    temporary.mkdir()
    print("[windows] Downloading pinned VSNASM build customizations...")
    try:
        for name, expected in _VSNASM_FILES.items():
            url = (
                "https://raw.githubusercontent.com/ShiftMediaProject/VSNASM/"
                f"{_VSNASM_REVISION}/{name}"
            )
            path = temporary / name
            _download(url, path)
            actual = _sha256(path)
            if actual != expected:
                raise BuildError(
                    f"VSNASM {name} checksum mismatch: expected {expected}, got {actual}"
                )
        if destination.exists():
            shutil.rmtree(destination)
        os.replace(temporary, destination)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    return destination


def _nghttp2_source_ready(source: Path) -> bool:
    header = source / "lib" / "includes" / "nghttp2" / "nghttp2.h"
    version_header = header.with_name("nghttp2ver.h")
    if not header.is_file() or not version_header.is_file():
        return False
    if not all((source / "lib" / name).is_file() for name in _NGHTTP2_SOURCES):
        return False
    version_text = version_header.read_text(encoding="utf-8")
    return f'#define NGHTTP2_VERSION "{_NGHTTP2_VERSION}"' in version_text


def _ensure_nghttp2(root: Path) -> Path:
    tools = root / ".build-tools"
    source = tools / f"nghttp2-{_NGHTTP2_VERSION}-source"
    if _nghttp2_source_ready(source):
        return source

    tools.mkdir(parents=True, exist_ok=True)
    archive_name = f"nghttp2-{_NGHTTP2_VERSION}.tar.xz"
    archive = tools / archive_name
    if not archive.is_file() or _sha256(archive) != _NGHTTP2_SHA256:
        archive.unlink(missing_ok=True)
        temporary_archive = tools / f".{archive_name}.{os.getpid()}.tmp"
        temporary_archive.unlink(missing_ok=True)
        url = (
            "https://github.com/nghttp2/nghttp2/releases/download/"
            f"v{_NGHTTP2_VERSION}/{archive_name}"
        )
        print(f"[windows] Downloading nghttp2 {_NGHTTP2_VERSION}...")
        _download(url, temporary_archive)
        actual_hash = _sha256(temporary_archive)
        if actual_hash != _NGHTTP2_SHA256:
            temporary_archive.unlink(missing_ok=True)
            raise BuildError(
                "nghttp2 archive checksum mismatch: "
                f"expected {_NGHTTP2_SHA256}, got {actual_hash}"
            )
        os.replace(temporary_archive, archive)

    temporary = tools / f".{source.name}.{os.getpid()}.tmp"
    if temporary.exists():
        shutil.rmtree(temporary)
    temporary.mkdir()
    try:
        try:
            _safe_extract_tar(archive, temporary)
        except tarfile.TarError as error:
            raise BuildError(f"Failed to extract {archive}: {error}") from error
        extracted = temporary / f"nghttp2-{_NGHTTP2_VERSION}"
        template = (
            extracted / "lib" / "includes" / "nghttp2" / "nghttp2ver.h.in"
        )
        if not template.is_file():
            raise BuildError(f"nghttp2 archive is missing {template.relative_to(extracted)}")
        version_parts = tuple(int(part) for part in _NGHTTP2_VERSION.split("."))
        if len(version_parts) != 3 or any(part < 0 or part > 255 for part in version_parts):
            raise BuildError(f"Invalid nghttp2 version: {_NGHTTP2_VERSION}")
        version_number = "0x" + "".join(f"{part:02x}" for part in version_parts)
        version_header = template.with_suffix("")
        version_text = template.read_text(encoding="utf-8")
        version_text = version_text.replace("@PACKAGE_VERSION@", _NGHTTP2_VERSION)
        version_text = version_text.replace("@PACKAGE_VERSION_NUM@", version_number)
        version_header.write_text(version_text, encoding="utf-8", newline="\n")
        if not _nghttp2_source_ready(extracted):
            raise BuildError("nghttp2 archive is missing required library sources")
        if source.exists():
            shutil.rmtree(source)
        os.replace(extracted, source)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    return source


def _find_msbuild() -> Path:
    candidates: list[Path] = []

    program_files_x86 = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
    vswhere = program_files_x86 / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.is_file():
        result = subprocess.run(
            [
                str(vswhere),
                "-latest",
                "-products",
                "*",
                "-requires",
                "Microsoft.Component.MSBuild",
                "-find",
                r"MSBuild\**\Bin\amd64\MSBuild.exe",
            ],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            encoding=locale.getpreferredencoding(False),
            errors="replace",
        )
        candidates.extend(Path(line.strip()) for line in result.stdout.splitlines() if line.strip())

    program_files = Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
    visual_studio = program_files / "Microsoft Visual Studio"
    if visual_studio.is_dir():
        candidates.extend(visual_studio.glob("2022/*/MSBuild/Current/Bin/amd64/MSBuild.exe"))

    on_path = shutil.which("MSBuild.exe") or shutil.which("MSBuild")
    if on_path:
        path_candidate = Path(on_path)
        candidates.append(path_candidate.parent / "amd64" / "MSBuild.exe")
        candidates.append(path_candidate)

    for candidate in candidates:
        candidate = candidate.resolve()
        if candidate.is_file():
            return candidate
    raise BuildError(
        "MSBuild was not found. Install Visual Studio 2022 with the Desktop development with C++ workload."
    )


def _visual_studio_root(msbuild: Path) -> Path:
    for parent in msbuild.parents:
        if (parent / "VC" / "Tools" / "MSVC").is_dir():
            return parent
    raise BuildError(f"Cannot locate the Visual Studio C++ toolchain for {msbuild}")


def _find_llvm(required_major: int) -> Path:
    candidates: list[Path] = []
    configured = os.environ.get("LLVM_INSTALL_DIR")
    if configured:
        candidates.append(Path(configured) / "bin")
    on_path = shutil.which("clang-cl.exe") or shutil.which("clang-cl")
    if on_path:
        candidates.append(Path(on_path).resolve().parent)
    program_files = Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
    candidates.append(program_files / "LLVM" / "bin")

    for directory in candidates:
        clang = directory / "clang-cl.exe"
        linker = directory / "lld-link.exe"
        librarian = directory / "llvm-lib.exe"
        if not all(path.is_file() for path in (clang, linker, librarian)):
            continue
        result = subprocess.run(
            [str(clang), "--version"],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        match = re.search(r"clang version\s+(\d+)", result.stdout)
        if result.returncode == 0 and match and int(match.group(1)) == required_major:
            return directory.resolve()
    raise BuildError(
        f"LLVM {required_major} with clang-cl, lld-link, and llvm-lib was not found. "
        "Install the matching 64-bit LLVM release or set LLVM_INSTALL_DIR."
    )


def _verify_toolchain(msbuild: Path, sdk_version: str) -> None:
    version = subprocess.run(
        [str(msbuild), "-version", "-nologo"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding=locale.getpreferredencoding(False),
        errors="replace",
    )
    if version.returncode != 0 or not version.stdout.strip().startswith("17."):
        raise BuildError(f"Visual Studio 2022 MSBuild is required; found: {version.stdout.strip()}")

    vs_root = _visual_studio_root(msbuild)
    msvc_roots = sorted((vs_root / "VC" / "Tools" / "MSVC").glob("*"), reverse=True)
    if not any((path / "bin" / "Hostx64" / "x64" / "cl.exe").is_file() for path in msvc_roots):
        raise BuildError(f"The Visual Studio v143 x64 C++ toolchain is missing under {vs_root}")

    program_files_x86 = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
    kits = program_files_x86 / "Windows Kits" / "10"
    required = (
        kits / "Include" / sdk_version / "um" / "Windows.h",
        kits / "Include" / sdk_version / "ucrt" / "stdio.h",
        kits / "Lib" / sdk_version / "um" / "x64" / "User32.Lib",
        kits / "bin" / sdk_version / "x64" / "rc.exe",
    )
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise BuildError(
            f"Windows SDK {sdk_version} is incomplete; missing: " + ", ".join(missing)
        )


def _same_file(first: Path, second: Path) -> bool:
    if not first.is_file() or not second.is_file():
        return False
    if first.stat().st_size != second.stat().st_size:
        return False
    with first.open("rb") as left, second.open("rb") as right:
        while True:
            left_chunk = left.read(1024 * 1024)
            right_chunk = right.read(1024 * 1024)
            if left_chunk != right_chunk:
                return False
            if not left_chunk:
                return True


def _replace_if_changed(temporary: Path, destination: Path) -> None:
    if _same_file(temporary, destination):
        temporary.unlink()
    else:
        os.replace(temporary, destination)


def _generate_embedded_resources(source_dir: Path, output: Path) -> None:
    if not source_dir.is_dir():
        raise BuildError(f"Embedded Electron resource directory is missing: {source_dir}")
    files = sorted(
        (path for path in source_dir.rglob("*") if path.is_file()),
        key=lambda path: path.relative_to(source_dir).as_posix().casefold(),
    )
    if not files:
        raise BuildError(f"No Electron resources found under {source_dir}")

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f".{output.name}.{os.getpid()}.tmp")
    try:
        with temporary.open("w", encoding="utf-8", newline="\n") as header:
            header.write("#pragma once\n#include <cstddef>\n\nnamespace atom {\n")
            header.write("struct EmbeddedResource {\n")
            header.write("    const char* path;\n")
            header.write("    const unsigned char* data;\n")
            header.write("    std::size_t size;\n")
            header.write("};\n")
            header.write("const EmbeddedResource kEmbeddedResources[] = {\n")
            for path in files:
                data = path.read_bytes()
                resource_name = "lib/" + path.relative_to(source_dir).as_posix()
                name_literal = json.dumps(resource_name, ensure_ascii=False)
                header.write(f"{{{name_literal},\n")
                header.write("(const unsigned char*)\n")
                if data:
                    for offset in range(0, len(data), 16):
                        encoded = "".join(f"\\x{byte:02X}" for byte in data[offset : offset + 16])
                        header.write(f'"{encoded}"\n')
                else:
                    header.write('""\n')
                header.write(f", {len(data)}}},\n")
            header.write("}; // kEmbeddedResources\n")
            header.write("} // namespace atom\n")
        _replace_if_changed(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)


def _generate_asar_source_header(output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(f".{output.name}.{os.getpid()}.tmp")
    try:
        temporary.write_text(
            '#pragma once\n\n#include "runtime/electron/common/asar/asar_js.h"\n',
            encoding="utf-8",
            newline="\n",
        )
        _replace_if_changed(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)


def _project_list(root: Path, manifest: dict[str, object]) -> list[tuple[str, Path]]:
    entry = _resolve_inside(root, manifest["entry_project"], "entry_project")
    if not entry.is_file():
        raise BuildError(f"Windows entry project is missing: {entry}")

    result: list[tuple[str, Path]] = []
    seen_names: set[str] = set()
    seen_paths: set[Path] = set()
    for index, value in enumerate(manifest["projects"]):
        if not isinstance(value, dict):
            raise BuildError(f"Invalid project #{index + 1} in {_MANIFEST}")
        name = value.get("name")
        project_value = value.get("path")
        if not isinstance(name, str) or not name or not isinstance(project_value, str):
            raise BuildError(f"Invalid project #{index + 1} in {_MANIFEST}")
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
            raise BuildError(f"Unsafe project name in {_MANIFEST}: {name}")
        project = _resolve_inside(root, project_value, f"project {name}")
        if name in seen_names or project in seen_paths:
            raise BuildError(f"Duplicate Windows project in {_MANIFEST}: {name}")
        if not project.is_file():
            raise BuildError(f"Windows project is missing: {project}")
        seen_names.add(name)
        seen_paths.add(project)
        if project != entry:
            result.append((name, project))

    result.append(("application", entry))
    return result


def _windows_path(path: Path, trailing_separator: bool = False) -> str:
    value = str(path.resolve())
    if trailing_separator and not value.endswith(("\\", "/")):
        value += "\\"
    return value


def _build_projects(
    root: Path,
    output: Path,
    manifest: dict[str, object],
    msbuild: Path,
    llvm_bin: Path,
    nasm_dir: Path,
    vsnasm_dir: Path,
    generated_root: Path,
    nghttp2_root: Path,
    jobs: int,
) -> Path:
    settings = manifest["msbuild"]
    if not isinstance(settings, dict):
        raise BuildError(f"Invalid msbuild settings in {_MANIFEST}")
    configuration = str(settings.get("configuration", "Release"))
    platform = str(settings.get("platform", "x64"))
    sdk = str(settings.get("sdk", "10.0.26100.0"))
    if configuration != "Release" or platform != "x64":
        raise BuildError("The Windows backend supports only Release|x64")

    stage = _resolve_inside(output, "stage", "stage directory")
    objects = _resolve_inside(output, "obj", "object directory")
    stage.mkdir(parents=True, exist_ok=True)
    objects.mkdir(parents=True, exist_ok=True)
    projects = _project_list(root, manifest)
    solution_dir = root / "build"

    environment = os.environ.copy()
    environment["NASMPATH"] = str(nasm_dir)
    environment["CL_MPCount"] = str(jobs)
    environment["MSBUILDDISABLENODEREUSE"] = "1"
    for name, project in projects:
        intermediate = _resolve_inside(objects, name, f"object directory for {name}")
        intermediate.mkdir(parents=True, exist_ok=True)
        command = [
            str(msbuild),
            str(project),
            "-nologo",
            "-m:1",
            "-v:minimal",
            "-t:Build",
            f"-p:Configuration={configuration}",
            f"-p:Platform={platform}",
            "-p:BuildProjectReferences=false",
            f"-p:SolutionDir={_windows_path(solution_dir, trailing_separator=True)}",
            f"-p:OutDir={_windows_path(stage, trailing_separator=True)}",
            f"-p:IntDir={_windows_path(intermediate, trailing_separator=True)}",
            "-p:PlatformToolset=v143",
            f"-p:WindowsTargetPlatformVersion={sdk}",
            "-p:UseMultiToolTask=true",
            f"-p:MultiProcMaxCount={jobs}",
            f"-p:ProcessorNumber={jobs}",
            "-p:EnforceProcessCountAcrossBuilds=true",
            f"-p:CLToolPath={llvm_bin}",
            "-p:CLToolExe=clang-cl.exe",
            f"-p:LinkToolPath={llvm_bin}",
            "-p:LinkToolExe=lld-link.exe",
            f"-p:LibToolPath={llvm_bin}",
            "-p:LibToolExe=llvm-lib.exe",
            f"-p:MiniElectronGeneratedRoot={_windows_path(generated_root)}",
            f"-p:MiniElectronNGHTTP2Root={_windows_path(nghttp2_root)}",
            f"-p:MiniElectronVSNASMDir={_windows_path(vsnasm_dir)}",
        ]
        print(f"[windows] Building {name} ({configuration}|{platform})...")
        try:
            result = subprocess.run(command, cwd=root, env=environment, check=False)
        except OSError as error:
            raise BuildError(f"Failed to start MSBuild for {name}: {error}") from error
        if result.returncode != 0:
            raise BuildError(f"MSBuild failed for {name} with exit code {result.returncode}")

    built = _resolve_inside(stage, manifest["built_executable"], "built executable")
    if not built.is_file() or built.stat().st_size == 0:
        raise BuildError(f"MSBuild succeeded but did not produce {built}")
    return built


def _remove_path(path: Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)


def _sync_app(source: Path, destination: Path, main_file: Path) -> None:
    source = source.resolve()
    destination = destination.resolve()
    if (
        source == destination
        or source in destination.parents
        or destination in source.parents
    ):
        raise BuildError("Electron resource source and package destination overlap")

    source_files = {
        path.relative_to(source): path
        for path in source.rglob("*")
        if path.is_file()
    }
    source_directories = {
        path.relative_to(source)
        for path in source.rglob("*")
        if path.is_dir()
    }
    source_directories.add(Path("."))
    wanted_files = set(source_files)
    wanted_files.add(Path("package.json"))

    destination.mkdir(parents=True, exist_ok=True)
    existing = sorted(destination.rglob("*"), key=lambda path: len(path.parts), reverse=True)
    for path in existing:
        relative = path.relative_to(destination)
        if path.is_dir() and not path.is_symlink():
            if relative not in source_directories:
                _remove_path(path)
        elif relative not in wanted_files:
            _remove_path(path)

    for relative in sorted(source_directories, key=lambda path: len(path.parts)):
        target = destination / relative
        if target.exists() and not target.is_dir():
            _remove_path(target)
        target.mkdir(parents=True, exist_ok=True)

    for relative, source_file in sorted(source_files.items(), key=lambda item: item[0].as_posix()):
        target = destination / relative
        if target.exists() and not target.is_file():
            _remove_path(target)
        target.parent.mkdir(parents=True, exist_ok=True)
        if not _same_file(source_file, target):
            shutil.copy2(source_file, target)

    package_source = source / "package.json"
    package: dict[str, object]
    if package_source.is_file():
        try:
            value = json.loads(package_source.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            raise BuildError(f"Invalid Electron package manifest {package_source}: {error}") from error
        if not isinstance(value, dict):
            raise BuildError(f"Electron package manifest must be a JSON object: {package_source}")
        package = value
    else:
        package = {"name": "mini-electron-app", "version": "1.0.0"}
    package["main"] = main_file.name
    package_bytes = (json.dumps(package, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    package_destination = destination / "package.json"
    if not package_destination.is_file() or package_destination.read_bytes() != package_bytes:
        package_destination.write_bytes(package_bytes)


def _package(root: Path, args: object, output: Path, built: Path, manifest: dict[str, object]) -> Path:
    main_value = getattr(args, "electron_main", "examples/electron/main.js")
    main_file = Path(str(main_value))
    if not main_file.is_absolute():
        main_file = root / main_file
    main_file = main_file.resolve()
    if not main_file.is_file():
        raise BuildError(f"Electron main script is missing: {main_file}")

    app_source = main_file.parent
    app_destination = _resolve_inside(
        output, "resources/app", "Electron package resources"
    )
    _sync_app(app_source, app_destination, main_file)

    package_name = manifest["package_executable"]
    if not isinstance(package_name, str) or not package_name:
        raise BuildError(f"Invalid package_executable in {_MANIFEST}")
    executable = _resolve_inside(output, package_name, "packaged executable")
    if executable.is_dir() and not executable.is_symlink():
        raise BuildError(f"Packaged executable path is a directory: {executable}")
    if executable.is_symlink():
        executable.unlink()
    try:
        if not _same_file(built, executable):
            shutil.copy2(built, executable)
    except OSError as error:
        raise BuildError(f"Cannot install packaged executable {executable}: {error}") from error
    return executable


def _package_runtime_distribution(
    root: Path, args: object, output: Path, built: Path
) -> tuple[Path, Path]:
    metadata = release_metadata(root, "win32", "x64")
    requested = getattr(args, "dist_output", None)
    destination = Path(str(requested)) if requested else output / "runtime-dist"
    if not destination.is_absolute():
        destination = root / destination
    if destination.is_symlink() or destination.is_junction():
        raise BuildError(
            f"--dist-output must not be a symlink or junction: {destination}"
        )
    destination = destination.resolve()
    if destination == root or destination == output or destination in output.parents:
        raise BuildError("--dist-output must not replace the repository or build output")
    staged = staging_directory(destination)
    try:
        executable = staged / "electron.exe"
        shutil.copy2(built, executable)
        copy_release_notices(root, staged)
        copy_runtime_javascript(root, staged / "resources")
        copy_release_notices(root, staged / "resources")
        published, archive, _ = publish_distribution(staged, destination, metadata)
    except BaseException:
        if staged.exists():
            shutil.rmtree(staged)
        raise
    print(f"[windows] Runtime distribution: {published}")
    print(f"[windows] Runtime archive: {archive}")
    return published, archive



def _find_visible_window(process_id: int, timeout: float) -> tuple[int, str] | None:
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    enum_windows = user32.EnumWindows
    enum_windows.argtypes = [callback_type, wintypes.LPARAM]
    enum_windows.restype = wintypes.BOOL
    get_process = user32.GetWindowThreadProcessId
    get_process.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    get_process.restype = wintypes.DWORD
    is_visible = user32.IsWindowVisible
    is_visible.argtypes = [wintypes.HWND]
    is_visible.restype = wintypes.BOOL
    get_length = user32.GetWindowTextLengthW
    get_length.argtypes = [wintypes.HWND]
    get_length.restype = ctypes.c_int
    get_text = user32.GetWindowTextW
    get_text.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    get_text.restype = ctypes.c_int

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found: list[tuple[int, str]] = []

        @callback_type
        def callback(window: int, unused: int) -> bool:
            del unused
            owner = wintypes.DWORD()
            get_process(window, ctypes.byref(owner))
            if owner.value != process_id or not is_visible(window):
                return True
            length = get_length(window)
            buffer = ctypes.create_unicode_buffer(length + 1)
            get_text(window, buffer, len(buffer))
            found.append((int(window), buffer.value))
            return False

        ctypes.set_last_error(0)
        if not enum_windows(callback, 0):
            error = ctypes.get_last_error()
            if not found and error:
                raise BuildError(f"EnumWindows failed with Windows error {error}")
        if found:
            return found[0]
        time.sleep(0.1)
    return None


def _stop_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        process.terminate()
    except OSError:
        return
    try:
        process.wait(timeout=_CLOSE_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def _run(executable: Path, interactive: bool) -> int:
    try:
        process = subprocess.Popen([str(executable)], cwd=executable.parent)
    except OSError as error:
        raise BuildError(f"Failed to launch {executable}: {error}") from error

    if interactive:
        print(f"[windows] Running {executable}; close the application to continue.")
        try:
            return process.wait()
        except KeyboardInterrupt:
            _stop_process(process)
            return 130

    try:
        window = _find_visible_window(process.pid, _WINDOW_TIMEOUT_SECONDS)
    except (BuildError, OSError, KeyboardInterrupt):
        _stop_process(process)
        raise
    if window is None:
        return_code = process.poll()
        _stop_process(process)
        if return_code is None:
            raise BuildError(
                f"{executable.name} did not create a visible window within "
                f"{_WINDOW_TIMEOUT_SECONDS:g}s"
            )
        raise BuildError(
            f"{executable.name} exited with code {return_code} "
            "before creating a visible window"
        )

    handle, title = window
    print(f"[windows] Visible window: {title or '<untitled>'}")
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    post_message = user32.PostMessageW
    post_message.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    post_message.restype = wintypes.BOOL
    if not post_message(wintypes.HWND(handle), 0x0010, 0, 0):
        _stop_process(process)
        raise BuildError(f"Failed to send WM_CLOSE to {executable.name}")
    try:
        return_code = process.wait(timeout=_CLOSE_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired as error:
        _stop_process(process)
        raise BuildError(f"{executable.name} did not exit after WM_CLOSE") from error
    if return_code != 0:
        raise BuildError(f"{executable.name} exited with code {return_code}")
    return 0


def build(root: Path, args: argparse.Namespace) -> int:
    try:
        if sys.platform != "win32":
            raise BuildError("The Windows backend can only run on Windows")
        root = root.resolve()
        _validate_options(args)
        jobs = int(getattr(args, "jobs", 0))
        if jobs < 1:
            raise BuildError("--jobs must be at least 1")
        output = (root / args.out).resolve()
        if output == root:
            raise BuildError("--out must not be the repository root")
        output.mkdir(parents=True, exist_ok=True)
        generate_node_bootstraps(root)

        manifest = _load_manifest(root)
        settings = manifest["msbuild"]
        if not isinstance(settings, dict):
            raise BuildError(f"Invalid msbuild settings in {_MANIFEST}")
        sdk_version = str(settings.get("sdk", "10.0.26100.0"))
        llvm_major = int(settings.get("llvm_major", 22))

        resources = manifest["embedded_resources"]
        if not isinstance(resources, dict):
            raise BuildError(f"Invalid embedded_resources in {_MANIFEST}")
        resource_source = resources.get("source_dir")
        resource_output = resources.get("output")
        if (
            not isinstance(resource_source, str)
            or not resource_source
            or not isinstance(resource_output, str)
            or not resource_output
        ):
            raise BuildError(f"Invalid embedded_resources paths in {_MANIFEST}")
        source_dir = _resolve_inside(root, resource_source, "embedded resource source")
        generated_header = _resolve_inside(
            output, resource_output, "embedded resource output"
        )
        generated_root = _resolve_inside(
            output,
            resources.get("include_root", "generated"),
            "generated include root",
        )
        try:
            generated_header.relative_to(generated_root)
        except ValueError as error:
            raise BuildError(
                "embedded_resources.output must be under "
                "embedded_resources.include_root"
            ) from error
        _generate_embedded_resources(source_dir, generated_header)

        asar_source = manifest["asar_source"]
        if not isinstance(asar_source, dict):
            raise BuildError(f"Invalid asar_source in {_MANIFEST}")
        asar_output = asar_source.get("output")
        if not isinstance(asar_output, str) or not asar_output:
            raise BuildError(f"Invalid asar_source.output in {_MANIFEST}")
        asar_header = _resolve_inside(output, asar_output, "ASAR source header")
        try:
            asar_header.relative_to(generated_root)
        except ValueError as error:
            raise BuildError(
                "asar_source.output must be under embedded_resources.include_root"
            ) from error
        _generate_asar_source_header(asar_header)

        msbuild = _find_msbuild()
        _verify_toolchain(msbuild, sdk_version)
        llvm_bin = _find_llvm(llvm_major)
        nasm_dir = _ensure_nasm(root)
        nghttp2_root = _ensure_nghttp2(root)
        vsnasm_dir = _ensure_vsnasm(root)
        built = _build_projects(
            root,
            output,
            manifest,
            msbuild,
            llvm_bin,
            nasm_dir,
            vsnasm_dir,
            generated_root,
            nghttp2_root,
            jobs,
        )
        if bool(getattr(args, "package_dist", False)):
            _package_runtime_distribution(root, args, output, built)
            return 0

        if bool(getattr(args, "omp_desktop", False)):
            from .omp_desktop import package_windows_omp_desktop

            source = Path(str(getattr(args, "omp_source", _DEFAULT_OMP_SOURCE)))
            if not source.is_absolute():
                source = root / source
            executable = package_windows_omp_desktop(
                root,
                output,
                built,
                source,
                getattr(args, "app_output", None),
            )
            if bool(getattr(args, "interactive", False)):
                return _run(executable, True)
            return 0

        executable = _package(root, args, output, built, manifest)
        print(f"[windows] Packaged {executable}")

        if bool(getattr(args, "electron", False)) or bool(getattr(args, "run", False)):
            interactive = bool(getattr(args, "electron", False)) and bool(
                getattr(args, "interactive", False)
            )
            return _run(executable, interactive)
        return 0
    except (BuildError, OSError, RuntimeError, ValueError, TypeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
