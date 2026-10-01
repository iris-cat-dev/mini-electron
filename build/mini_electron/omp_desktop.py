# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

import json
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from typing import Any


_PRODUCT_NAME = "OMP Desktop"
_WORKSPACE_PREFIX = "@omp-desktop/"
_IGNORED_PRODUCTION_PACKAGES = {"electron", "electron-builder", "typescript", "vitest"}
_REQUIRED_DESKTOP_OUTPUTS = (
    Path("dist/main.js"),
    Path("dist/preload.js"),
    Path("dist/daemon/node-entrypoint-runner.js"),
)
_REQUIRED_BACKEND_ENTRIES = (
    Path("node_modules/@omp-desktop/cli/dist/index.js"),
    Path("node_modules/@omp-desktop/server/dist/scripts/supervisor-entrypoint.js"),
    Path(
        "node_modules/@omp-desktop/server/dist/server/server/agent/providers/omp/"
        "background-jobs-extension.js"
    ),
)
_REQUIRED_NATIVE_ENTRIES = (
    Path(
        "node_modules/@napi-rs/keyring-win32-x64-msvc/"
        "keyring.win32-x64-msvc.node"
    ),
    Path("node_modules/node-pty/prebuilds/win32-x64/conpty.node"),
    Path("node_modules/@esbuild/win32-x64/esbuild.exe"),
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


def _run(command: list[str], *, cwd: Path, env: dict[str, str] | None = None) -> None:
    print("+", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, cwd=cwd, env=env, check=True)


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


def _validate_remote_backend(backend: Path) -> dict[str, Any]:
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
        digest = hashlib.sha256(candidate.read_bytes()).hexdigest()
        if digest != expected_hash:
            raise OmpPackagingError(
                f"Remote backend file checksum does not match its manifest: {relative}"
            )
    return manifest


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


def _desktop_external_dependencies(
    source: Path, desktop_package: dict[str, Any]
) -> dict[str, str]:
    dependencies = desktop_package.get("dependencies")
    if not isinstance(dependencies, dict) or not dependencies:
        raise OmpPackagingError("OMP Desktop package.json has no production dependencies")
    root_lock = _read_json(source / "package-lock.json", "OMP Desktop root lockfile")
    locked_packages = root_lock.get("packages")
    if not isinstance(locked_packages, dict):
        raise OmpPackagingError("OMP Desktop root lockfile has no packages")

    external: dict[str, str] = {}
    for name, requested_version in dependencies.items():
        if (
            not isinstance(name, str)
            or not isinstance(requested_version, str)
            or not requested_version
        ):
            raise OmpPackagingError("OMP Desktop has an invalid production dependency")
        if name.startswith(_WORKSPACE_PREFIX):
            continue
        if name in _IGNORED_PRODUCTION_PACKAGES:
            raise OmpPackagingError(
                f"OMP Desktop incorrectly declares build-only package {name} as a dependency"
            )
        locked = locked_packages.get(f"node_modules/{name}")
        locked_version = locked.get("version") if isinstance(locked, dict) else None
        if not isinstance(locked_version, str) or not locked_version:
            raise OmpPackagingError(
                f"OMP Desktop production dependency is not locked: {name}"
            )
        external[name] = locked_version
    return external


def _install_production_dependencies(
    source: Path,
    install_root: Path,
    desktop_package: dict[str, Any],
) -> Path:
    backend = source / "packages" / "desktop" / "remote-backend-dist"
    shutil.copytree(backend, install_root)
    deployment_package = _read_json(install_root / "package.json", "deployment package")
    dependencies = deployment_package.get("dependencies")
    if not isinstance(dependencies, dict):
        raise OmpPackagingError("Remote backend deployment package has no dependencies")
    dependencies.update(_desktop_external_dependencies(source, desktop_package))
    deployment_package["dependencies"] = dict(sorted(dependencies.items()))
    (install_root / "package.json").write_text(
        json.dumps(deployment_package, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    npm = shutil.which("npm.cmd") or shutil.which("npm")
    if not npm:
        raise OmpPackagingError("npm was not found on PATH")
    environment = os.environ.copy()
    environment.update(
        {
            "ELECTRON_SKIP_BINARY_DOWNLOAD": "1",
            "npm_config_arch": "x64",
            "npm_config_platform": "win32",
        }
    )
    common = ["--omit=dev", "--include=optional", "--no-audit", "--no-fund"]
    _run(
        [npm, "install", "--package-lock-only", "--ignore-scripts", *common],
        cwd=install_root,
        env=environment,
    )
    _run([npm, "ci", *common], cwd=install_root, env=environment)

    modules = install_root / "node_modules"
    _require_directory(modules, "installed production node_modules")
    for relative in _REQUIRED_BACKEND_ENTRIES:
        _require_file(install_root / relative, f"installed backend runtime entry {relative}")
    for relative in _REQUIRED_NATIVE_ENTRIES:
        _require_file(install_root / relative, f"installed native runtime entry {relative}")
    for name in _IGNORED_PRODUCTION_PACKAGES:
        if (modules / name).exists():
            raise OmpPackagingError(
                f"Production dependency installation unexpectedly contains {name}"
            )
    for executable_name in ("electron.exe", "node.exe"):
        runtime_binaries = list(modules.rglob(executable_name))
        if runtime_binaries:
            raise OmpPackagingError(
                "Production dependency installation contains an external runtime: "
                + str(runtime_binaries[0])
            )
    return modules


def _remove_path(path: Path) -> None:
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)


def _prune_native_modules(modules: Path) -> None:
    prebuilds = modules / "node-pty" / "prebuilds"
    if prebuilds.is_dir():
        for child in prebuilds.iterdir():
            if child.name != "win32-x64":
                _remove_path(child)
    for debug_symbols in (modules / "node-pty").rglob("*.pdb"):
        if debug_symbols.is_file():
            debug_symbols.unlink()

    esbuild = modules / "@esbuild"
    if esbuild.is_dir():
        for child in esbuild.iterdir():
            if child.name != "win32-x64":
                _remove_path(child)

    image_modules = modules / "@img"
    if image_modules.is_dir():
        for child in image_modules.iterdir():
            if child.name.startswith("sharp-") and not child.name.startswith(
                "sharp-win32-x64"
            ):
                _remove_path(child)

    web_ui = modules / "@omp-desktop" / "server" / "dist" / "server" / "web-ui"
    _remove_path(web_ui)


def _remove_development_metadata(root: Path) -> None:
    for path in root.rglob("*"):
        if path.is_file() and path.name.endswith((".map", ".d.ts", ".d.mts", ".d.cts")):
            path.unlink()


def _create_asar(source: Path, destination: Path, asar_package: Path) -> None:
    node = shutil.which("node.exe") or shutil.which("node")
    if not node:
        raise OmpPackagingError("Node.js was not found on PATH for @electron/asar")
    # @electron/asar is a build-time dependency installed in the OMP checkout,
    # not a runtime dependency copied into the package.
    if not asar_package.is_dir():
        raise OmpPackagingError(
            f"Installed @electron/asar package was not found: {asar_package}"
        )
    script = (
        "const asar=require(process.argv[1]);"
        "asar.createPackageWithOptions(process.argv[2],process.argv[3],"
        "{unpackDir:'{dist,node_modules}'}).catch(e=>{console.error(e);process.exit(1)});"
    )
    _run(
        [node, "-e", script, str(asar_package), str(source), str(destination)],
        cwd=source,
    )


def _write_app_update_config(resources: Path, package_name: str) -> None:
    # Match electron-builder's sanitize-filename behavior for this package's
    # updater cache name, then serialize the checked-in publish configuration.
    invalid = '/?<>\\\\:*|"'
    sanitized_name = "".join(
        character
        for character in package_name
        if character not in invalid
        and ord(character) >= 32
        and not 128 <= ord(character) <= 159
    ).rstrip(" .")
    if not sanitized_name:
        raise OmpPackagingError("OMP Desktop package name cannot form an updater cache name")
    (resources / "app-update.yml").write_text(
        "provider: github\n"
        "owner: iris-cat-dev\n"
        "repo: omp-desktop\n"
        f"updaterCacheDirName: {json.dumps(sanitized_name.lower() + '-updater')}\n",
        encoding="utf-8",
    )


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
    _require_directory(source, "OMP Desktop source checkout")
    _require_file(binary, "compiled mini-electron executable")

    desktop = source / "packages" / "desktop"
    app_dist = source / "packages" / "app" / "dist"
    backend = desktop / "remote-backend-dist"
    desktop_manifest_path = desktop / "package.json"
    root_manifest_path = source / "package.json"
    license_path = source / "LICENSE"
    omp_binary = source / "bin" / "omp-windows-x64.exe"
    ripgrep = (
        source
        / "node_modules"
        / "@vscode"
        / "ripgrep-universal"
        / "bin"
        / "win32-x64"
        / "rg.exe"
    )
    asar_package = source / "node_modules" / "@electron" / "asar" / "package.json"

    for relative in _REQUIRED_DESKTOP_OUTPUTS:
        _require_file(desktop / relative, f"compiled desktop output {relative}")
    for path, label in (
        (app_dist / "index.html", "compiled Electron frontend"),
        (desktop_manifest_path, "desktop package manifest"),
        (root_manifest_path, "OMP Desktop root package manifest"),
        (license_path, "OMP Desktop license"),
        (omp_binary, "Windows x64 OMP executable"),
        (ripgrep, "Windows x64 ripgrep executable"),
        (asar_package, "installed @electron/asar package"),
        (desktop / "assets" / "icon.png", "OMP Desktop PNG icon"),
        (desktop / "assets" / "icon.ico", "OMP Desktop Windows icon"),
        (desktop / "bin" / "omp-desktop.cmd", "OMP Desktop Windows CLI shim"),
    ):
        _require_file(path, label)
    _require_directory(desktop / "assets" / "editor-targets", "editor target icons")
    _validate_remote_backend(backend)
    _validate_cli_shim(desktop / "bin" / "omp-desktop.cmd")

    desktop_package = _read_json(desktop_manifest_path, "desktop package manifest")
    root_package = _read_json(root_manifest_path, "OMP Desktop root package manifest")
    package_name = desktop_package.get("name")
    if not isinstance(package_name, str) or not package_name:
        raise OmpPackagingError("OMP Desktop package name is missing")
    if desktop_package.get("version") != root_package.get("version"):
        raise OmpPackagingError("OMP Desktop package versions do not match")
    version = desktop_package.get("version")
    if not isinstance(version, str) or not version:
        raise OmpPackagingError("OMP Desktop package version is missing")

    app = _resolve_output(root, out_dir, source, requested_output)
    with tempfile.TemporaryDirectory(prefix="omp-desktop-package-", dir=app.parent) as temporary:
        temporary_root = Path(temporary)
        assembled = temporary_root / "assembled"
        resources = assembled / "resources"
        bin_dir = resources / "bin"
        resources.mkdir(parents=True)
        bin_dir.mkdir()

        install_root = temporary_root / "production-install"
        modules = _install_production_dependencies(source, install_root, desktop_package)
        _prune_native_modules(modules)

        app_source = temporary_root / "app"
        shutil.copytree(desktop / "dist", app_source / "dist")
        modules.replace(app_source / "node_modules")
        app_package = {
            "name": package_name,
            "productName": _PRODUCT_NAME,
            "version": version,
            "private": True,
            "license": root_package.get("license", desktop_package.get("license")),
            "main": "dist/main.js",
            "dependencies": desktop_package.get("dependencies", {}),
        }
        (app_source / "package.json").write_text(
            json.dumps(app_package, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        _remove_development_metadata(app_source)
        _create_asar(
            app_source,
            resources / "app.asar",
            source / "node_modules" / "@electron" / "asar",
        )

        runner = resources / "app.asar.unpacked" / "dist" / "daemon" / "node-entrypoint-runner.js"
        _require_file(runner, "unpacked Node entrypoint runner")
        for relative in _REQUIRED_BACKEND_ENTRIES:
            unpacked = resources / "app.asar.unpacked" / relative
            _require_file(unpacked, f"unpacked backend runtime entry {relative}")

        shutil.copy2(binary, assembled / f"{_PRODUCT_NAME}.exe")
        shutil.copytree(app_dist, resources / "app-dist")
        shutil.copytree(backend, resources / "remote-backend")
        shutil.copytree(desktop / "assets" / "editor-targets", resources / "editor-target-icons")
        shutil.copy2(omp_binary, bin_dir / "omp.exe")
        shutil.copy2(ripgrep, bin_dir / "rg.exe")
        shutil.copy2(desktop / "bin" / "omp-desktop.cmd", bin_dir / "omp-desktop.cmd")
        shutil.copy2(desktop / "assets" / "icon.png", resources / "icon.png")
        shutil.copy2(desktop / "assets" / "icon.ico", resources / "icon.ico")
        shutil.copy2(license_path, assembled / "LICENSE.omp-desktop.txt")
        shutil.copy2(root / "LICENSE", assembled / "LICENSE.mini-electron.txt")
        _write_app_update_config(resources, package_name)

        for executable_name in ("electron.exe", "node.exe"):
            if any(assembled.rglob(executable_name)):
                raise OmpPackagingError(
                    f"Packaged application contains an external {executable_name} runtime"
                )
        executable = assembled / f"{_PRODUCT_NAME}.exe"
        _require_file(executable, "packaged mini-electron executable")

        if app.exists():
            shutil.rmtree(app)
        assembled.replace(app)

    executable = app / f"{_PRODUCT_NAME}.exe"
    print(f"[windows] OMP Desktop package: {app}", flush=True)
    return executable
