# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from typing import Any

from .distribution import copy_release_notices


_PRODUCT_NAME = "OMP Desktop"
_ARCH = "x64"
_ELECTRON_BUILDER_VERSION = "26.8.1"
_REQUIRED_DESKTOP_OUTPUTS = (
    Path("dist/main.js"),
    Path("dist/preload.js"),
    Path("dist/daemon/node-entrypoint-runner.js"),
)
_REQUIRED_PACKAGED_ENTRIES = (
    Path("resources/app.asar"),
    Path("resources/app.asar.unpacked/dist/daemon/node-entrypoint-runner.js"),
    Path(
        "resources/app.asar.unpacked/node_modules/@omp-desktop/server/dist/server/"
        "server/agent/providers/omp/background-jobs-extension.js"
    ),
    Path(
        "resources/app.asar.unpacked/node_modules/@napi-rs/"
        "keyring-win32-x64-msvc/keyring.win32-x64-msvc.node"
    ),
    Path(
        "resources/app.asar.unpacked/node_modules/node-pty/"
        "prebuilds/win32-x64/conpty.node"
    ),
    Path("resources/app-dist/index.html"),
    Path("resources/remote-backend/manifest.json"),
    Path("resources/bin/rg.exe"),
    Path("resources/bin/omp-desktop.cmd"),
    Path("resources/icon.png"),
    Path("resources/icon.ico"),
    Path("resources/mini-electron/lib/browser/init.js"),
    Path("resources/LICENSE"),
    Path("resources/LICENSES.chromium.html"),
    Path("resources/LICENSES.leveldb"),
)


class OmpPackagingError(RuntimeError):
    pass


def _read_json(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise OmpPackagingError(f"Cannot read {label} {path}: {error}") from error
    if not isinstance(value, dict):
        raise OmpPackagingError(f"{label} must contain a JSON object: {path}")
    return value


def _require_file(path: Path, label: str) -> None:
    if not path.is_file() or path.stat().st_size == 0:
        raise OmpPackagingError(f"{label} is missing or empty: {path}")


def _require_directory(path: Path, label: str) -> None:
    if not path.is_dir():
        raise OmpPackagingError(f"{label} is missing: {path}")


def _run(command: list[str], *, cwd: Path, env: dict[str, str]) -> None:
    print("+", subprocess.list2cmdline(command), flush=True)
    try:
        subprocess.run(command, cwd=cwd, env=env, check=True)
    except subprocess.CalledProcessError as error:
        raise OmpPackagingError(
            f"electron-builder failed with exit code {error.returncode}"
        ) from error


def _resolve_output(
    root: Path,
    out_dir: Path,
    source: Path,
    requested_output: str | None,
) -> Path:
    if requested_output:
        candidate = Path(requested_output)
        if not candidate.is_absolute():
            candidate = root / candidate
    else:
        candidate = out_dir / _PRODUCT_NAME
    if candidate.is_symlink() or candidate.is_junction():
        raise OmpPackagingError(
            f"--app-output must not be a symlink or junction: {candidate}"
        )
    destination = candidate.resolve()

    root = root.resolve()
    out_dir = out_dir.resolve()
    source = source.resolve()
    if destination in (root, out_dir) or destination in root.parents:
        raise OmpPackagingError(
            f"--app-output must name a package directory, not {destination}"
        )
    if (
        destination == source
        or destination in source.parents
        or source in destination.parents
    ):
        raise OmpPackagingError(
            f"--app-output must not overlap the OMP Desktop source checkout: {destination}"
        )
    if destination.exists() and not destination.is_dir():
        raise OmpPackagingError(
            f"--app-output must be a directory path: {destination}"
        )
    destination.parent.mkdir(parents=True, exist_ok=True)
    return destination


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _validate_remote_backend(backend: Path) -> None:
    manifest_path = backend / "manifest.json"
    package_path = backend / "package.json"
    lock_path = backend / "package-lock.json"
    packages = backend / "packages"
    for path, label in (
        (manifest_path, "remote backend manifest"),
        (package_path, "remote backend package manifest"),
        (lock_path, "remote backend lockfile"),
    ):
        _require_file(path, label)
    _require_directory(packages, "remote backend deployment tarballs")

    manifest = _read_json(manifest_path, "remote backend manifest")
    if manifest.get("schemaVersion") != 1:
        raise OmpPackagingError(
            f"Unsupported remote backend manifest schema in {manifest_path}"
        )
    files = manifest.get("files")
    if not isinstance(files, dict) or not files:
        raise OmpPackagingError(f"Remote backend manifest has no files: {manifest_path}")
    backend_root = backend.resolve()
    for relative, expected_hash in files.items():
        if (
            not isinstance(relative, str)
            or not relative
            or not isinstance(expected_hash, str)
            or len(expected_hash) != 64
        ):
            raise OmpPackagingError(f"Invalid remote backend manifest entry: {relative!r}")
        candidate = (backend / relative).resolve()
        try:
            candidate.relative_to(backend_root)
        except ValueError as error:
            raise OmpPackagingError(
                f"Remote backend manifest path escapes its directory: {relative}"
            ) from error
        _require_file(candidate, f"remote backend file {relative}")
        if _sha256(candidate) != expected_hash:
            raise OmpPackagingError(
                f"Remote backend file checksum does not match its manifest: {relative}"
            )


def _validate_cli_shim(path: Path) -> None:
    contents = path.read_text(encoding="utf-8").lower()
    required = (
        'set "electron_run_as_node=1"',
        '"%app_executable%"',
        "app.asar.unpacked\\dist\\daemon\\node-entrypoint-runner.js",
        "app.asar\\node_modules\\@omp-desktop\\cli\\dist\\index.js",
    )
    if any(value not in contents for value in required):
        raise OmpPackagingError(
            f"OMP Desktop CLI shim does not use mini-electron's Node mode: {path}"
        )


def _runtime_versions(root: Path, source: Path, desktop_package: dict[str, Any]) -> str:
    mini_package = _read_json(root / "package.json", "mini-electron package manifest")
    runtime = mini_package.get("miniElectron")
    if not isinstance(runtime, dict):
        raise OmpPackagingError("mini-electron package metadata has no miniElectron object")
    electron_version = runtime.get("electronApiVersion")
    if not isinstance(electron_version, str) or not electron_version:
        raise OmpPackagingError("mini-electron electronApiVersion is missing")

    development = desktop_package.get("devDependencies")
    configured_electron = (
        development.get("electron") if isinstance(development, dict) else None
    )
    if configured_electron != electron_version:
        raise OmpPackagingError(
            "OMP Desktop Electron version does not match mini-electron's API version: "
            f"{configured_electron!r} != {electron_version!r}"
        )

    builder_package = _read_json(
        source / "node_modules" / "electron-builder" / "package.json",
        "electron-builder package manifest",
    )
    if builder_package.get("version") != _ELECTRON_BUILDER_VERSION:
        raise OmpPackagingError(
            "OMP Desktop must use electron-builder "
            f"{_ELECTRON_BUILDER_VERSION}, found {builder_package.get('version')!r}"
        )
    return electron_version


def _prepare_electron_distribution(root: Path, binary: Path, destination: Path) -> None:
    resources = destination / "resources"
    locales = destination / "locales"
    runtime_source = root / "runtime" / "electron" / "lib"
    _require_directory(runtime_source, "mini-electron runtime JavaScript")
    destination.mkdir()
    resources.mkdir()
    locales.mkdir()
    # electron-builder prunes configured languages by scanning this directory
    # after copying the custom distribution, and filesystem copies omit empty
    # directories. Keep it present without pretending to ship Chromium packs.
    (locales / ".mini-electron").write_text("", encoding="utf-8")
    shutil.copy2(binary, destination / "electron.exe")
    shutil.copytree(runtime_source, resources / "mini-electron" / "lib")
    copy_release_notices(root, destination)
    copy_release_notices(root, resources)


def _channel_name(version: str) -> str:
    if "-" not in version:
        return "latest"
    prerelease = version.split("-", 1)[1]
    channel = prerelease.split(".", 1)[0]
    if not channel:
        raise OmpPackagingError(f"Invalid prerelease version: {version}")
    return channel


def _require_magic(path: Path, magic: bytes, label: str) -> None:
    _require_file(path, label)
    with path.open("rb") as stream:
        if stream.read(len(magic)) != magic:
            raise OmpPackagingError(f"{label} has an invalid file signature: {path}")


def _validate_builder_output(output: Path, version: str, bundle_omp: bool) -> Path:
    unpacked = output / "win-unpacked"
    executable = unpacked / f"{_PRODUCT_NAME}.exe"
    _require_magic(executable, b"MZ", "packaged mini-electron executable")
    for relative in _REQUIRED_PACKAGED_ENTRIES:
        _require_file(unpacked / relative, f"packaged runtime entry {relative}")
    omp_binary = unpacked / "resources" / "bin" / "omp.exe"
    if bundle_omp:
        _require_file(omp_binary, "bundled OMP executable")
    elif omp_binary.exists():
        raise OmpPackagingError(f"No-OMP package retained OMP executable: {omp_binary}")
    for forbidden in ("electron.exe", "node.exe"):
        if (unpacked / forbidden).exists():
            raise OmpPackagingError(
                f"Packaged application retained an external runtime: {unpacked / forbidden}"
            )
    if (unpacked / "resources" / "devtools-frontend").exists():
        raise OmpPackagingError(
            "DevTools-disabled Windows package retained frontend assets"
        )

    variant = "" if bundle_omp else "No-OMP-"
    artifact_stem = f"OMP-Desktop-{variant}Setup-{version}-{_ARCH}"
    installer = output / f"{artifact_stem}.exe"
    archive = output / f"{artifact_stem}.zip"
    blockmap = output / f"{artifact_stem}.exe.blockmap"
    channel_metadata = output / f"{_channel_name(version)}.yml"
    _require_magic(installer, b"MZ", "NSIS installer")
    _require_magic(archive, b"PK", "Windows zip artifact")
    _require_file(blockmap, "NSIS blockmap")
    _require_file(channel_metadata, "electron-updater channel metadata")

    metadata = channel_metadata.read_text(encoding="utf-8")
    if installer.name not in metadata or "sha512:" not in metadata:
        raise OmpPackagingError(
            f"Update metadata does not describe the NSIS installer: {channel_metadata}"
        )
    app_update = unpacked / "resources" / "app-update.yml"
    _require_file(app_update, "packaged electron-updater configuration")
    updater_config = app_update.read_text(encoding="utf-8")
    for expected in (
        "provider: github",
        "owner: iris-cat-dev",
        "repo: omp-desktop",
    ):
        if expected not in updater_config:
            raise OmpPackagingError(
                f"Packaged updater configuration is missing {expected!r}: {app_update}"
            )
    return executable


def package_windows_omp_desktop(
    root: Path,
    out_dir: Path,
    binary: Path,
    source: Path,
    requested_output: str | None,
) -> Path:
    root = root.resolve()
    out_dir = out_dir.resolve()
    source = source.resolve()
    binary = binary.resolve()
    _require_directory(source, "OMP Desktop source checkout")
    _require_file(binary, "compiled mini-electron executable")

    desktop = source / "packages" / "desktop"
    app_dist = source / "packages" / "app" / "dist"
    backend = desktop / "remote-backend-dist"
    desktop_manifest_path = desktop / "package.json"
    bundle_omp = os.environ.get("OMP_DESKTOP_BUNDLE_OMP") != "0"
    builder_config = desktop / (
        "electron-builder.yml" if bundle_omp else "electron-builder-no-omp.cjs"
    )
    builder_cli = source / "node_modules" / "electron-builder" / "cli.js"
    omp_binary = source / "bin" / "omp-windows-x64.exe"
    keyring = (
        source
        / "node_modules"
        / "@napi-rs"
        / "keyring-win32-x64-msvc"
        / "keyring.win32-x64-msvc.node"
    )

    for relative in _REQUIRED_DESKTOP_OUTPUTS:
        _require_file(desktop / relative, f"compiled desktop output {relative}")
    for path, label in (
        (app_dist / "index.html", "compiled Electron frontend"),
        (
            source / "packages" / "cli" / "dist" / "output" / "index.js",
            "compiled OMP Desktop CLI output",
        ),
        (desktop_manifest_path, "desktop package manifest"),
        (builder_config, "electron-builder configuration"),
        (builder_cli, f"electron-builder {_ELECTRON_BUILDER_VERSION} CLI"),
        (keyring, "Windows x64 native keyring binding"),
        (desktop / "assets" / "icon.png", "OMP Desktop PNG icon"),
        (desktop / "assets" / "icon.ico", "OMP Desktop Windows icon"),
        (desktop / "bin" / "omp-desktop.cmd", "OMP Desktop Windows CLI shim"),
    ):
        _require_file(path, label)
    if bundle_omp:
        _require_file(omp_binary, "Windows x64 OMP executable")
    _validate_remote_backend(backend)
    _validate_cli_shim(desktop / "bin" / "omp-desktop.cmd")

    desktop_package = _read_json(desktop_manifest_path, "desktop package manifest")
    version = desktop_package.get("version")
    if not isinstance(version, str) or not version:
        raise OmpPackagingError("OMP Desktop package version is missing")
    root_package = _read_json(source / "package.json", "OMP Desktop root package manifest")
    if version != root_package.get("version"):
        raise OmpPackagingError("OMP Desktop package versions do not match")
    electron_version = _runtime_versions(root, source, desktop_package)

    node = shutil.which("node.exe") or shutil.which("node")
    if not node:
        raise OmpPackagingError("Node.js was not found on PATH")

    if not bundle_omp and requested_output is None:
        requested_output = str(out_dir / "OMP Desktop No OMP")
    destination = _resolve_output(root, out_dir, source, requested_output)
    with tempfile.TemporaryDirectory(
        prefix="omp-desktop-electron-builder-", dir=destination.parent
    ) as temporary:
        temporary_root = Path(temporary)
        electron_dist = temporary_root / "mini-electron-dist"
        builder_output = temporary_root / "release"
        _prepare_electron_distribution(root, binary, electron_dist)

        environment = os.environ.copy()
        environment["OMP_DESKTOP_BUNDLE_OMP"] = "1" if bundle_omp else "0"
        environment.setdefault("CSC_IDENTITY_AUTO_DISCOVERY", "false")
        command = [
            node,
            str(builder_cli),
            "--config",
            str(builder_config),
            "--win",
            "--x64",
            "--publish",
            "never",
            f"--config.electronDist={electron_dist}",
            f"--config.electronVersion={electron_version}",
            f"--config.directories.output={builder_output}",
        ]
        _run(command, cwd=desktop, env=environment)
        _validate_builder_output(builder_output, version, bundle_omp)

        previous_output = temporary_root / "previous-release"
        if destination.exists():
            destination.replace(previous_output)
        try:
            builder_output.replace(destination)
        except OSError:
            if previous_output.exists():
                previous_output.replace(destination)
            raise

    executable = destination / "win-unpacked" / f"{_PRODUCT_NAME}.exe"
    print(f"[windows] OMP Desktop artifacts: {destination}", flush=True)
    print(f"[windows] OMP Desktop package: {executable}", flush=True)
    return executable
