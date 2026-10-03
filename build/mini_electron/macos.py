# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

import argparse
import json
import hashlib
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request
from typing import Optional
from .distribution import (
    copy_devtools_frontend,
    copy_release_notices,
    copy_runtime_javascript,
    publish_distribution,
    release_metadata,
    staging_directory,
)

GN_REVISION = "feafd1012a32c05ec6095f69ddc3850afb621f3a"
HEADLESS_TARGET = "mini_electron_smoke"
HEADLESS_BINARY = "mini-electron-smoke"
GUI_TARGET = "mini_electron_browser"
GUI_BINARY = "mini-electron-browser"
ELECTRON_TARGET = "mini_electron"
ELECTRON_BINARY = "mini-electron"
NGHTTP2_VERSION = "1.61.0"
NGHTTP2_SHA256 = "c0e660175b9dc429f11d25b9507a834fb752eea9135ab420bb7cb7e9dbcc9654"


ELECTRON_BUILDER_VERSION = "26.8.1"


SQUIRREL_FRAMEWORKS = ("Squirrel", "Mantle", "ReactiveObjC")
SQUIRREL_LICENSES = (
    ("Squirrel.Mac.LICENSE", "third_party/Squirrel.Mac-main/LICENSE"),
    ("Mantle.LICENSE.md", "third_party/Squirrel.Mac-main/vendor/Mantle/LICENSE.md"),
    (
        "ReactiveObjC.LICENSE.md",
        "third_party/Squirrel.Mac-main/vendor/ReactiveObjC/LICENSE.md",
    ),
    ("Sparkle.LICENSE", "third_party/Squirrel.Mac-main/vendor/Sparkle/LICENSE"),
)


def copy_squirrel_frameworks(out_dir: Path, destination: Path) -> None:
    for framework_name in SQUIRREL_FRAMEWORKS:
        source = out_dir / f"{framework_name}.framework"
        if not source.is_dir() or source.is_symlink():
            raise RuntimeError(f"Built {framework_name} framework is missing: {source}")
        expected_links = {
            "Versions/Current": "A",
            framework_name: f"Versions/Current/{framework_name}",
            "Headers": "Versions/Current/Headers",
            "Resources": "Versions/Current/Resources",
        }
        for relative, expected_target in expected_links.items():
            link = source / relative
            if not link.is_symlink() or os.readlink(link) != expected_target:
                raise RuntimeError(
                    f"Built {framework_name} framework has an invalid versioned "
                    f"layout at {link}"
                )
        framework_binary = source / "Versions" / "A" / framework_name
        if not framework_binary.is_file() or framework_binary.stat().st_size == 0:
            raise RuntimeError(
                f"Built {framework_name} framework binary is missing or empty: "
                f"{framework_binary}"
            )
        if framework_name == "Squirrel":
            shipit = source / "Versions" / "A" / "Resources" / "ShipIt"
            if (
                not shipit.is_file()
                or shipit.stat().st_size == 0
                or (shipit.stat().st_mode & 0o111) == 0
            ):
                raise RuntimeError(
                    f"Built Squirrel framework is missing executable ShipIt: {shipit}"
                )
        shutil.copytree(
            source,
            destination / source.name,
            symlinks=True,
        )


def copy_squirrel_notices(root: Path, destination: Path) -> None:
    notices = destination / "LICENSES.squirrel"
    notices.mkdir(exist_ok=True)
    for output_name, relative_source in SQUIRREL_LICENSES:
        source = root / relative_source
        if not source.is_file():
            raise RuntimeError(f"Squirrel runtime license is missing: {source}")
        shutil.copy2(source, notices / output_name)


def run(
    command: list[str], *, cwd: Path, env: Optional[dict[str, str]] = None
) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, env=env, check=True)


def find_or_install_gn(root: Path) -> str:
    configured = os.environ.get("GN")
    if configured:
        return configured

    local_gn = root / ".build-tools" / "gn"
    if local_gn.is_file():
        return str(local_gn)

    path_gn = shutil.which("gn")
    if path_gn:
        return path_gn

    cipd = os.environ.get("CIPD") or shutil.which("cipd")
    if not cipd:
        raise RuntimeError(
            "GN was not found. Install depot_tools, put cipd on PATH, or set GN."
        )

    local_gn.parent.mkdir(parents=True, exist_ok=True)
    run(
        [
            cipd,
            "install",
            "gn/gn/mac-arm64",
            f"git_revision:{GN_REVISION}",
            "-root",
            str(local_gn.parent),
        ],
        cwd=root,
    )
    return str(local_gn)


def xcode_clang() -> tuple[Path, str]:
    clang = Path(
        subprocess.check_output(["xcrun", "--find", "clang"], text=True).strip()
    ).resolve()
    resource_dir = Path(
        subprocess.check_output(
            [str(clang), "-print-resource-dir"], text=True
        ).strip()
    )
    return clang.parent.parent, resource_dir.name


def gn_args(clang_base: Path, clang_version: str) -> str:
    values = {
        "target_os": "mac",
        "target_cpu": "arm64",
        "is_debug": False,
        "is_component_build": False,
        "clang_base_path": str(clang_base),
        "clang_version_override": clang_version,
        "clang_use_chrome_plugins": False,
        "use_custom_libcxx": False,
        "use_custom_libcxx_for_host": False,
        "use_lld": False,
        "use_system_xcode": True,
        "enable_rust": False,
        "enable_base_tracing": False,
        "use_siso": False,
        "use_reclient": False,
        "v8_enable_fuzztest": False,
        "v8_enable_i18n_support": True,
        "v8_enable_javascript_promise_hooks": True,
        "v8_enable_sandbox": False,
        "v8_use_external_startup_data": False,
        "icu_use_data_file": False,
        "symbol_level": 0,
        "treat_warnings_as_errors": False,
        "fatal_linker_warnings": False,
    }
    return " ".join(
        f"{name}={json.dumps(value) if isinstance(value, str) else str(value).lower()}"
        for name, value in values.items()
    )


def download(url: str, destination: Path) -> None:
    print(f"+ download {url}", flush=True)
    with urllib.request.urlopen(url) as response, destination.open("wb") as output:
        shutil.copyfileobj(response, output)


def ensure_nghttp2(root: Path, clang_base: Path) -> Path:
    tools_dir = root / ".build-tools"
    installation = tools_dir / f"nghttp2-{NGHTTP2_VERSION}-darwin-arm64"
    header = installation / "include" / "nghttp2" / "nghttp2.h"
    library = installation / "lib" / "libnghttp2.a"
    if header.is_file() and library.is_file():
        return installation

    tools_dir.mkdir(parents=True, exist_ok=True)
    archive_name = f"nghttp2-{NGHTTP2_VERSION}.tar.xz"
    url = (
        "https://github.com/nghttp2/nghttp2/releases/download/"
        f"v{NGHTTP2_VERSION}/{archive_name}"
    )
    with tempfile.TemporaryDirectory(dir=tools_dir) as temporary:
        temporary_dir = Path(temporary)
        archive = temporary_dir / archive_name
        download(url, archive)
        actual = hashlib.sha256(archive.read_bytes()).hexdigest()
        if actual != NGHTTP2_SHA256:
            raise RuntimeError(
                f"nghttp2 archive checksum mismatch: expected {NGHTTP2_SHA256}, "
                f"got {actual}"
            )
        with tarfile.open(archive, "r:xz") as package:
            package.extractall(temporary_dir)
        source = temporary_dir / f"nghttp2-{NGHTTP2_VERSION}"
        staged = temporary_dir / "install"
        build_environment = os.environ.copy()
        developer_dir = clang_base.parents[2]
        build_environment["DEVELOPER_DIR"] = str(developer_dir)
        sdk = subprocess.check_output(
            ["xcrun", "--sdk", "macosx", "--show-sdk-path"],
            env=build_environment,
            text=True,
        ).strip()
        build_environment["CC"] = str(clang_base / "bin" / "clang")
        build_environment["CXX"] = str(clang_base / "bin" / "clang++")
        build_environment["CFLAGS"] = (
            f"-isysroot {sdk} -mmacosx-version-min=13.0"
        )
        build_environment["LDFLAGS"] = (
            f"-isysroot {sdk} -mmacosx-version-min=13.0"
        )
        run(
            [
                str(source / "configure"),
                f"--prefix={staged}",
                "--enable-lib-only",
                "--disable-shared",
                "--enable-static",
            ],
            cwd=source,
            env=build_environment,
        )
        run(["make", "-j", "16"], cwd=source, env=build_environment)
        run(["make", "install"], cwd=source, env=build_environment)
        if not (staged / "include/nghttp2/nghttp2.h").is_file() or not (
            staged / "lib/libnghttp2.a"
        ).is_file():
            raise RuntimeError("nghttp2 build did not produce the static library")
        if installation.exists():
            shutil.rmtree(installation)
        shutil.move(str(staged), installation)
    return installation


def _require_file(path: Path, label: str) -> None:
    if not path.is_file() or path.stat().st_size == 0:
        raise RuntimeError(f"{label} is missing or empty: {path}")


def _read_json(path: Path, label: str) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"Cannot read {label} {path}: {error}") from error
    if not isinstance(value, dict):
        raise RuntimeError(f"{label} must contain a JSON object: {path}")
    return value


def _validate_framework_layout(frameworks: Path) -> None:
    for framework_name in SQUIRREL_FRAMEWORKS:
        framework = frameworks / f"{framework_name}.framework"
        expected_links = {
            "Versions/Current": "A",
            framework_name: f"Versions/Current/{framework_name}",
            "Headers": "Versions/Current/Headers",
            "Resources": "Versions/Current/Resources",
        }
        for relative, target in expected_links.items():
            link = framework / relative
            if not link.is_symlink() or os.readlink(link) != target:
                raise RuntimeError(
                    f"Packaged {framework_name} framework has an invalid link: {link}"
                )
        _require_file(
            framework / "Versions" / "A" / framework_name,
            f"packaged {framework_name} framework binary",
        )
    shipit = (
        frameworks
        / "Squirrel.framework"
        / "Versions"
        / "A"
        / "Resources"
        / "ShipIt"
    )
    _require_file(shipit, "packaged Squirrel ShipIt")
    if (shipit.stat().st_mode & 0o111) == 0:
        raise RuntimeError(f"Packaged Squirrel ShipIt is not executable: {shipit}")


def _stage_electron_bundle(
    root: Path,
    out_dir: Path,
    binary: Path,
    app: Path,
    metadata: dict[str, object],
) -> None:
    if app.exists() or app.is_symlink():
        raise RuntimeError(f"Runtime application staging path already exists: {app}")
    contents = app / "Contents"
    macos = contents / "MacOS"
    resources = contents / "Resources"
    frameworks = contents / "Frameworks"
    macos.mkdir(parents=True)
    resources.mkdir()
    frameworks.mkdir()

    executable = macos / "Electron"
    shutil.copy2(binary, executable)
    executable.chmod(0o755)
    strip = shutil.which("strip")
    if not strip:
        raise RuntimeError("strip was not found on PATH")
    run([strip, "-x", str(executable)], cwd=root)

    info = {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": "Electron",
        "CFBundleExecutable": "Electron",
        "CFBundleIdentifier": "dev.mini-electron.runtime",
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": "Electron",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": str(metadata["runtimeVersion"]),
        "CFBundleVersion": str(metadata["runtimeVersion"]),
        "LSMinimumSystemVersion": "13.0",
        "NSHighResolutionCapable": True,
    }
    with (contents / "Info.plist").open("wb") as output:
        plistlib.dump(info, output, sort_keys=True)
    (contents / "PkgInfo").write_bytes(b"APPL????")

    helper_names = (
        "Electron Helper",
        "Electron Helper (GPU)",
        "Electron Helper (Plugin)",
        "Electron Helper (Renderer)",
    )
    for helper_name in helper_names:
        helper = frameworks / f"{helper_name}.app" / "Contents"
        helper_macos = helper / "MacOS"
        helper_macos.mkdir(parents=True)
        helper_executable = helper_macos / helper_name
        os.link(executable, helper_executable)
        helper_executable.chmod(0o755)
        helper_info = {
            "CFBundleDevelopmentRegion": "en",
            "CFBundleDisplayName": helper_name,
            "CFBundleExecutable": helper_name,
            "CFBundleIdentifier": (
                "dev.mini-electron.runtime."
                + helper_name.removeprefix("Electron ")
                .lower()
                .replace(" ", "-")
                .replace("(", "")
                .replace(")", "")
            ),
            "CFBundleInfoDictionaryVersion": "6.0",
            "CFBundleName": helper_name,
            "CFBundlePackageType": "APPL",
            "CFBundleShortVersionString": str(metadata["runtimeVersion"]),
            "CFBundleVersion": str(metadata["runtimeVersion"]),
            "LSBackgroundOnly": True,
            "LSMinimumSystemVersion": "13.0",
        }
        with (helper / "Info.plist").open("wb") as output:
            plistlib.dump(helper_info, output, sort_keys=True)
        (helper / "PkgInfo").write_bytes(b"APPL????")

    copy_squirrel_frameworks(out_dir, frameworks)
    copy_runtime_javascript(root, resources)
    copy_devtools_frontend(root, resources)
    copy_release_notices(root, resources)
    copy_squirrel_notices(root, resources)
    _validate_framework_layout(frameworks)


def _validate_remote_backend(backend: Path) -> None:
    manifest = _read_json(backend / "manifest.json", "remote backend manifest")
    files = manifest.get("files")
    if manifest.get("schemaVersion") != 1 or not isinstance(files, dict) or not files:
        raise RuntimeError(f"Remote backend manifest is invalid: {backend / 'manifest.json'}")
    backend_root = backend.resolve()
    for relative, expected_hash in files.items():
        if (
            not isinstance(relative, str)
            or not relative
            or not isinstance(expected_hash, str)
            or not re.fullmatch(r"[a-f0-9]{64}", expected_hash)
        ):
            raise RuntimeError(f"Remote backend manifest entry is invalid: {relative!r}")
        candidate = (backend / relative).resolve()
        try:
            candidate.relative_to(backend_root)
        except ValueError as error:
            raise RuntimeError(
                f"Remote backend manifest path escapes its directory: {relative}"
            ) from error
        _require_file(candidate, f"remote backend file {relative}")
        digest = hashlib.sha256()
        with candidate.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        if digest.hexdigest() != expected_hash:
            raise RuntimeError(
                f"Remote backend file checksum does not match its manifest: {relative}"
            )


def _resolve_omp_output(
    root: Path,
    out_dir: Path,
    source: Path,
    requested_output: Optional[str],
) -> Path:
    candidate = Path(requested_output) if requested_output else out_dir / "OMP Desktop.app"
    if not candidate.is_absolute():
        candidate = root / candidate
    if candidate.is_symlink() or candidate.is_junction():
        raise RuntimeError(
            f"--app-output must not be a symlink or junction: {candidate}"
        )
    destination = candidate.resolve()
    root = root.resolve()
    out_dir = out_dir.resolve()
    source = source.resolve()
    if destination in (root, out_dir) or destination in root.parents or destination in out_dir.parents:
        raise RuntimeError(
            f"--app-output must name an application bundle, not {destination}"
        )
    if (
        destination == source
        or destination in source.parents
        or source in destination.parents
    ):
        raise RuntimeError("--app-output must not overlap the OMP Desktop source")
    if destination.exists() and not destination.is_dir():
        raise RuntimeError(f"--app-output must be a directory path: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    return destination


def _validate_omp_asar(node: str, source: Path, archive: Path) -> None:
    script = r"""
const asar = require('@electron/asar');
const archive = process.argv[1];
const manifest = JSON.parse(asar.extractFile(archive, 'package.json').toString('utf8'));
if (manifest.main !== 'dist/main.js') throw new Error(`unexpected main entry: ${manifest.main}`);
for (const entry of [
  'dist/main.js',
  'dist/preload.js',
  'node_modules/@omp-desktop/cli/dist/run.js',
  'node_modules/@omp-desktop/server/package.json',
]) {
  if (asar.extractFile(archive, entry).length === 0) throw new Error(`missing ${entry}`);
}
"""
    result = subprocess.run(
        [node, "-e", script, str(archive)],
        cwd=source,
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(f"Packaged OMP Desktop app.asar is invalid: {detail}")


def _validate_omp_app(app: Path) -> None:
    contents = app / "Contents"
    resources = contents / "Resources"
    required = (
        contents / "MacOS" / "OMP Desktop",
        resources / "app.asar",
        resources / "app.asar.unpacked" / "dist" / "daemon" / "node-entrypoint-runner.js",
        resources / "app-dist" / "index.html",
        resources / "remote-backend" / "manifest.json",
        resources
        / "app.asar.unpacked"
        / "node_modules"
        / "@omp-desktop"
        / "server"
        / "dist"
        / "server"
        / "server"
        / "agent"
        / "providers"
        / "omp"
        / "background-jobs-extension.js",
        resources / "bin" / "omp",
        resources / "bin" / "omp-desktop",
        resources / "mini-electron" / "lib" / "browser" / "init.js",
        resources / "mini-electron" / "lib" / "browser" / "electron.js",
        resources / "bin" / "rg",
        resources / "devtools-frontend" / "front_end" / "devtools_app.html",
        resources / "LICENSE",
        resources / "LICENSES.chromium.html",
        resources / "LICENSES.leveldb",
        resources / "LICENSES.squirrel" / "Squirrel.Mac.LICENSE",
        resources / "LICENSES.squirrel" / "Mantle.LICENSE.md",
        resources / "LICENSES.squirrel" / "ReactiveObjC.LICENSE.md",
        resources / "LICENSES.squirrel" / "Sparkle.LICENSE",
    )
    for path in required:
        _require_file(path, "packaged OMP Desktop entry")
    forbidden = (
        contents / "MacOS" / "Electron",
        contents / "MacOS" / "node",
        contents / "Frameworks" / "Electron Framework.framework",
    )
    if any(path.exists() for path in forbidden):
        raise RuntimeError("OMP Desktop retained a stock Electron or Node runtime")
    _validate_remote_backend(resources / "remote-backend")
    _validate_framework_layout(contents / "Frameworks")


def package_omp_desktop(
    root: Path,
    out_dir: Path,
    binary: Path,
    source: Path,
    requested_output: Optional[str],
) -> Path:
    root = root.resolve()
    out_dir = out_dir.resolve()
    source = source.resolve()
    binary = binary.resolve()
    desktop = source / "packages" / "desktop"
    app_dist = source / "packages" / "app" / "dist"
    backend = desktop / "remote-backend-dist"
    desktop_manifest = desktop / "package.json"
    builder_config = desktop / "electron-builder.yml"
    builder_cli = source / "node_modules" / "electron-builder" / "cli.js"
    required = (
        binary,
        desktop / "dist" / "main.js",
        desktop / "dist" / "preload.js",
        desktop / "dist" / "daemon" / "node-entrypoint-runner.js",
        app_dist / "index.html",
        backend / "manifest.json",
        backend / "package.json",
        backend / "package-lock.json",
        source / "bin" / "omp-darwin-arm64",
        desktop / "bin" / "omp-desktop",
        desktop / "assets" / "icon-macos.png",
        desktop_manifest,
        builder_config,
        builder_cli,
    )
    for path in required:
        _require_file(path, "OMP Desktop packaging input")
    _validate_remote_backend(backend)

    desktop_package = _read_json(desktop_manifest, "desktop package manifest")
    root_package = _read_json(source / "package.json", "OMP Desktop package manifest")
    if desktop_package.get("version") != root_package.get("version"):
        raise RuntimeError("OMP Desktop package versions do not match")
    if desktop_package.get("main") != "dist/main.js":
        raise RuntimeError("OMP Desktop main entry is not dist/main.js")
    builder_package = _read_json(
        source / "node_modules" / "electron-builder" / "package.json",
        "electron-builder package manifest",
    )
    if builder_package.get("version") != ELECTRON_BUILDER_VERSION:
        raise RuntimeError(
            f"OMP Desktop must use electron-builder {ELECTRON_BUILDER_VERSION}"
        )
    mini_package = _read_json(root / "package.json", "mini-electron package manifest")
    mini_runtime = mini_package.get("miniElectron")
    electron_version = (
        mini_runtime.get("electronApiVersion")
        if isinstance(mini_runtime, dict)
        else None
    )
    development = desktop_package.get("devDependencies")
    if (
        not isinstance(electron_version, str)
        or not isinstance(development, dict)
        or development.get("electron") != electron_version
    ):
        raise RuntimeError("OMP Desktop Electron API version does not match mini-electron")

    node = shutil.which("node")
    if not node:
        raise RuntimeError("Node.js was not found on PATH")
    destination = _resolve_omp_output(root, out_dir, source, requested_output)
    metadata = release_metadata(root, "darwin", "arm64")
    with tempfile.TemporaryDirectory(
        prefix=".omp-desktop-package-", dir=destination.parent
    ) as temporary:
        temporary_root = Path(temporary)
        electron_dist = temporary_root / "electron-dist"
        _stage_electron_bundle(
            root, out_dir, binary, electron_dist / "Electron.app", metadata
        )
        copy_release_notices(root, electron_dist)
        copy_squirrel_notices(root, electron_dist)
        builder_output = temporary_root / "builder-output"
        environment = os.environ.copy()
        environment["OMP_DESKTOP_BUNDLE_OMP"] = "1"
        environment["CSC_IDENTITY_AUTO_DISCOVERY"] = "false"
        run(
            [
                node,
                str(builder_cli),
                "--config",
                str(builder_config),
                "--mac",
                "dir",
                "--arm64",
                "--publish",
                "never",
                f"--config.electronDist={electron_dist}",
                f"--config.electronVersion={electron_version}",
                f"--config.directories.output={builder_output}",
            ],
            cwd=desktop,
            env=environment,
        )
        candidates = sorted(builder_output.glob("mac*/OMP Desktop.app"))
        if len(candidates) != 1:
            raise RuntimeError(
                f"electron-builder produced {len(candidates)} OMP Desktop app bundles"
            )
        packaged = candidates[0]
        resources = packaged / "Contents" / "Resources"
        copy_release_notices(root, resources)
        copy_squirrel_notices(root, resources)
        _validate_omp_app(packaged)
        _validate_omp_asar(node, source, resources / "app.asar")
        run(["codesign", "--force", "--deep", "--sign", "-", str(packaged)], cwd=root)

        previous = temporary_root / "previous-app"
        if destination.exists():
            destination.rename(previous)
        try:
            packaged.rename(destination)
        except BaseException:
            if previous.exists() and not destination.exists():
                previous.rename(destination)
            raise
    print(f"OMP Desktop package: {destination}", flush=True)
    return destination


def package_runtime_distribution(
    root: Path, out_dir: Path, binary: Path, requested_output: Optional[str]
) -> tuple[Path, Path]:
    metadata = release_metadata(root, "darwin", "arm64")
    destination = (
        Path(requested_output) if requested_output else out_dir / "runtime-dist"
    )
    if not destination.is_absolute():
        destination = root / destination
    if destination.is_symlink() or destination.is_junction():
        raise RuntimeError(
            f"--dist-output must not be a symlink or junction: {destination}"
        )
    destination = destination.resolve()
    if destination == root or destination == out_dir or destination in out_dir.parents:
        raise RuntimeError("--dist-output must not replace the repository or build output")
    if not binary.is_file() or binary.stat().st_size == 0:
        raise RuntimeError(f"Runtime binary is missing or empty: {binary}")

    staged = staging_directory(destination)
    try:
        app = staged / "Electron.app"
        _stage_electron_bundle(root, out_dir, binary, app, metadata)
        copy_release_notices(root, staged)
        copy_squirrel_notices(root, staged)
        run(["codesign", "--force", "--deep", "--sign", "-", str(app)], cwd=root)
        published, archive, _ = publish_distribution(staged, destination, metadata)
    except BaseException:
        if staged.exists():
            shutil.rmtree(staged)
        raise
    print(f"[macos] Runtime distribution: {published}", flush=True)
    print(f"[macos] Runtime archive: {archive}", flush=True)
    return published, archive


def build(root: Path, args: argparse.Namespace) -> int:
    ninja = shutil.which("ninja")
    if not ninja:
        raise RuntimeError("ninja was not found on PATH")
    if args.dist_output and not args.package_dist:
        raise RuntimeError("--dist-output requires --package-dist")


    gn = find_or_install_gn(root)
    clang_base, clang_version = xcode_clang()
    out_dir = root / args.out

    screenshot = args.screenshot or (
        "/tmp/mini-electron-demo.png"
        if args.electron
        else "/tmp/mini-electron-browser.png"
    )
    target = (
        ELECTRON_TARGET
        if args.electron or args.omp_desktop or args.package_dist
        else GUI_TARGET
        if args.gui
        else HEADLESS_TARGET
    )
    binary_name = (
        ELECTRON_BINARY
        if target == ELECTRON_TARGET
        else GUI_BINARY
        if target == GUI_TARGET
        else HEADLESS_BINARY
    )
    if target == ELECTRON_TARGET:
        ensure_nghttp2(root, clang_base)
        from .node_sources import generate_node_bootstraps

        generate_node_bootstraps(root)
    run([gn, "gen", str(out_dir), f"--args={gn_args(clang_base, clang_version)}"], cwd=root)
    run([ninja, "-C", str(out_dir), "-j", str(args.jobs), target], cwd=root)
    if args.package_dist:
        package_runtime_distribution(
            root,
            out_dir,
            out_dir / binary_name,
            args.dist_output,
        )
        return 0
    if args.omp_desktop:
        source = Path(args.omp_source)
        if not source.is_absolute():
            source = root / source
        package_omp_desktop(
            root,
            out_dir,
            out_dir / binary_name,
            source.resolve(),
            args.app_output,
        )
        return 0
    if args.electron:
        main_script = Path(args.electron_main)
        if not main_script.is_absolute():
            main_script = root / main_script
        command = [str(out_dir / binary_name), str(main_script), screenshot]
        if args.interactive:
            command.append("--interactive")
        run(command, cwd=root)
        return 0
    if args.gui:
        frontend = args.frontend
        if not frontend.startswith(("https://", "http://")):
            frontend_path = Path(frontend)
            if not frontend_path.is_absolute():
                frontend_path = root / frontend_path
            frontend = str(frontend_path)
        run([str(out_dir / binary_name), frontend, screenshot], cwd=root)
    elif args.run:
        run([str(out_dir / binary_name)], cwd=root)
    return 0
