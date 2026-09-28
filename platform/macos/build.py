#!/usr/bin/env python3
# Copyright 2026 The miniblink132 Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

import argparse
import json
import hashlib
import os
from pathlib import Path
import plistlib
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from typing import Optional

GN_REVISION = "feafd1012a32c05ec6095f69ddc3850afb621f3a"
HEADLESS_TARGET = "miniblink_mac_smoke"
GUI_TARGET = "miniblink_mac_gui_demo"
ELECTRON_TARGET = "miniblink_mac_electron_demo"

BACKEND_BUNDLE_ENTRIES = (
    (
        "node_modules/@omp-desktop/cli/dist/index.js",
        "node_modules/@omp-desktop/cli/dist/index.js",
    ),
    (
        "node_modules/@omp-desktop/server/dist/scripts/supervisor-entrypoint.js",
        "node_modules/@omp-desktop/server/dist/scripts/supervisor-entrypoint.js",
    ),
    (
        "node_modules/@omp-desktop/server/dist/server/server/daemon-worker.js",
        "node_modules/@omp-desktop/server/dist/server/server/daemon-worker.js",
    ),
)
BACKEND_EXTERNAL_PACKAGES = (
    "@esbuild/darwin-arm64",
    "@napi-rs/keyring",
    "@vscode/ripgrep",
    "@vscode/ripgrep-darwin-arm64",
    "esbuild",
    "node-pty",
    "sherpa-onnx-darwin-arm64",
    "sherpa-onnx-node",
    "which",
)
BACKEND_RUNTIME_PACKAGES = (
    "@esbuild/darwin-arm64",
    "@napi-rs/keyring",
    "@napi-rs/keyring-darwin-arm64",
    "@vscode/ripgrep",
    "@vscode/ripgrep-darwin-arm64",
    "esbuild",
    "isexe",
    "node-pty",
    "sherpa-onnx-darwin-arm64",
    "sherpa-onnx-node",
    "which",
)


def run(command: list[str], *, cwd: Path) -> None:
    print("+", " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def find_or_install_gn(root: Path) -> str:
    configured = os.environ.get("GN")
    if configured:
        return configured

    local_gn = root / ".mac-tools" / "gn"
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


def ensure_node_distribution(root: Path, version: str) -> Path:
    tools_dir = root / ".mac-tools"
    distribution = tools_dir / f"node-v{version}-darwin-arm64"
    node = distribution / "bin" / "node"
    npm = distribution / "bin" / "npm"
    if node.is_file() and npm.is_file():
        return distribution

    tools_dir.mkdir(parents=True, exist_ok=True)
    archive_name = f"node-v{version}-darwin-arm64.tar.gz"
    base_url = f"https://nodejs.org/dist/v{version}"
    with tempfile.TemporaryDirectory(dir=tools_dir) as temporary:
        temporary_dir = Path(temporary)
        checksums = temporary_dir / "SHASUMS256.txt"
        archive = temporary_dir / archive_name
        download(f"{base_url}/SHASUMS256.txt", checksums)
        expected = next(
            (
                line.split()[0]
                for line in checksums.read_text().splitlines()
                if line.split()[-1] == archive_name
            ),
            None,
        )
        if not expected:
            raise RuntimeError(f"{archive_name} is missing from Node.js checksums")
        download(f"{base_url}/{archive_name}", archive)
        actual = hashlib.sha256(archive.read_bytes()).hexdigest()
        if actual != expected:
            raise RuntimeError(
                f"Node.js archive checksum mismatch: expected {expected}, got {actual}"
            )
        with tarfile.open(archive, "r:gz") as package:
            package.extractall(temporary_dir)
        extracted = temporary_dir / distribution.name
        if not (extracted / "bin" / "node").is_file():
            raise RuntimeError("Node.js archive did not contain the expected executable")
        if distribution.exists():
            shutil.rmtree(distribution)
        shutil.move(str(extracted), distribution)
    return distribution


def write_executable(path: Path, content: str) -> None:
    path.write_text(content)
    path.chmod(0o755)


def copy_backend_package(modules: Path, optimized_modules: Path, name: str) -> None:
    source = modules / name
    if not source.is_dir():
        raise RuntimeError(f"installed backend is missing runtime package {name}")
    destination = optimized_modules / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, destination, symlinks=True)


def optimize_packaged_backend(root: Path, backend: Path) -> None:
    modules = backend / "node_modules"
    esbuild = modules / "esbuild" / "bin" / "esbuild"
    if not esbuild.is_file():
        raise RuntimeError("installed backend is missing esbuild")

    optimized = backend.with_name(f"{backend.name}-optimized")
    if optimized.exists():
        shutil.rmtree(optimized)
    optimized_modules = optimized / "node_modules"
    optimized_modules.mkdir(parents=True)

    for name in ("package.json", "manifest.json"):
        source = backend / name
        if source.is_file():
            shutil.copy2(source, optimized / name)

    banner = (
        'import { createRequire as __createRequireForBundle } from "node:module"; '
        "const require = __createRequireForBundle(import.meta.url);"
    )
    for source_relative, output_relative in BACKEND_BUNDLE_ENTRIES:
        source = backend / source_relative
        if not source.is_file():
            raise RuntimeError(f"installed backend is missing bundle entry {source_relative}")
        output = optimized / output_relative
        output.parent.mkdir(parents=True, exist_ok=True)
        command = [
            str(esbuild),
            str(source),
            "--bundle",
            "--platform=node",
            "--target=node22",
            "--format=esm",
            "--minify-syntax",
            "--minify-whitespace",
            "--legal-comments=none",
            f"--banner:js={banner}",
            f"--outfile={output}",
        ]
        command.extend(f"--external:{name}" for name in BACKEND_EXTERNAL_PACKAGES)
        run(command, cwd=root)

    for name in BACKEND_RUNTIME_PACKAGES:
        copy_backend_package(modules, optimized_modules, name)

    cli_source = modules / "@omp-desktop" / "cli"
    cli_destination = optimized_modules / "@omp-desktop" / "cli"
    shutil.copy2(cli_source / "package.json", cli_destination / "package.json")
    shutil.copytree(cli_source / "bin", cli_destination / "bin")

    server_source = modules / "@omp-desktop" / "server"
    server_destination = optimized_modules / "@omp-desktop" / "server"
    shutil.copy2(server_source / "package.json", server_destination / "package.json")
    exports = server_destination / "dist" / "server" / "server" / "exports.js"
    exports.write_text("export {};\n")

    worker_directory = (
        server_destination / "dist" / "server" / "server"
    )
    file_assets = (
        (
            server_source
            / "dist/server/server/speech/providers/local/sherpa/assets/silero_vad.onnx",
            worker_directory / "assets/silero_vad.onnx",
        ),
        (
            server_source
            / "dist/server/server/agent/providers/omp/background-jobs-extension.js",
            worker_directory / "background-jobs-extension.js",
        ),
        (
            server_source / "dist/server/terminal/terminal-ts-loader.mjs",
            worker_directory / "terminal-ts-loader.mjs",
        ),
        (
            server_source / "dist/server/terminal/terminal-ts-loader.mjs",
            optimized_modules
            / "@omp-desktop"
            / "terminal"
            / "terminal-ts-loader.mjs",
        ),
        (
            server_source / "dist/scripts/mcp-stdio-socket-bridge-cli.mjs",
            server_destination / "dist/scripts/mcp-stdio-socket-bridge-cli.mjs",
        ),
        (
            server_source / "dist/scripts/github-git-askpass.mjs",
            server_destination / "dist/scripts/github-git-askpass.mjs",
        ),
        (
            server_source / "dist/scripts/github-git-askpass.cmd",
            server_destination / "dist/scripts/github-git-askpass.cmd",
        ),
    )
    for source, destination in file_assets:
        if not source.is_file():
            raise RuntimeError(f"installed backend is missing runtime asset {source}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)

    directory_assets = (
        (
            server_source / "dist/server/terminal/shell-integration",
            worker_directory / "shell-integration",
        ),
        (
            server_source / "dist/server/skills",
            server_destination / "skills",
        ),
    )
    for source, destination in directory_assets:
        if not source.is_dir():
            raise RuntimeError(f"installed backend is missing runtime assets {source}")
        shutil.copytree(source, destination)

    npm_bin = optimized_modules / ".bin"
    npm_bin.mkdir()
    (npm_bin / "omp-desktop").symlink_to("../@omp-desktop/cli/bin/omp-desktop")

    shutil.rmtree(backend)
    optimized.rename(backend)
    print("Optimized packaged backend with bundled CLI and daemon", flush=True)


def package_omp_desktop(
    root: Path,
    out_dir: Path,
    binary: Path,
    source: Path,
    requested_output: Optional[str],
) -> Path:
    app_dist = source / "packages" / "app" / "dist"
    backend_source = (
        source / "packages" / "desktop" / "remote-backend-dist"
    )
    manifest_path = backend_source / "manifest.json"
    omp_binary = source / "bin" / "omp-darwin-arm64"
    icon = source / "packages" / "desktop" / "assets" / "icon.icns"
    required = [
        app_dist / "index.html",
        manifest_path,
        backend_source / "package-lock.json",
        omp_binary,
        icon,
        binary,
    ]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise RuntimeError("OMP Desktop packaging inputs are missing: " + ", ".join(missing))

    manifest = json.loads(manifest_path.read_text())
    node_version = manifest["nodeVersion"]
    node_distribution = ensure_node_distribution(root, node_version)
    package_metadata = json.loads((source / "package.json").read_text())

    app = Path(requested_output) if requested_output else out_dir / "OMP Desktop.app"
    if not app.is_absolute():
        app = root / app
    if app.exists():
        shutil.rmtree(app)
    contents = app / "Contents"
    macos = contents / "MacOS"
    resources = contents / "Resources"
    bin_dir = resources / "bin"
    node_bin = resources / "node" / "bin"
    macos.mkdir(parents=True)
    bin_dir.mkdir(parents=True)
    node_bin.mkdir(parents=True)

    executable = macos / "OMP Desktop"
    shutil.copy2(binary, executable)
    executable.chmod(0o755)
    unstripped_size = executable.stat().st_size
    strip = shutil.which("strip")
    if not strip:
        raise RuntimeError("strip was not found on PATH")
    run([strip, "-x", str(executable)], cwd=root)
    stripped_size = executable.stat().st_size
    print(
        f"Stripped {unstripped_size - stripped_size} bytes from {executable.name}",
        flush=True,
    )
    shutil.copytree(app_dist, resources / "app-dist")
    shutil.copytree(backend_source, resources / "backend")
    shutil.copy2(node_distribution / "bin" / "node", node_bin / "node")
    shutil.copy2(omp_binary, bin_dir / "omp")
    shutil.copy2(icon, resources / "icon.icns")
    (node_bin / "node").chmod(0o755)
    (bin_dir / "omp").chmod(0o755)

    install_environment = os.environ.copy()
    install_environment["PATH"] = (
        str(node_distribution / "bin")
        + os.pathsep
        + install_environment.get("PATH", "")
    )
    npm = node_distribution / "bin" / "npm"
    print(f"+ install packaged backend with Node.js {node_version}", flush=True)
    subprocess.run(
        [
            str(npm),
            "ci",
            "--prefix",
            str(resources / "backend"),
            "--omit=dev",
            "--include=optional",
            "--no-audit",
            "--no-fund",
        ],
        cwd=root,
        env=install_environment,
        check=True,
    )
    optimize_packaged_backend(root, resources / "backend")
    cli_entry = (
        resources
        / "backend"
        / "node_modules"
        / "@omp-desktop"
        / "cli"
        / "dist"
        / "index.js"
    )
    if not cli_entry.is_file():
        raise RuntimeError("installed backend is missing @omp-desktop/cli")


    write_executable(
        bin_dir / "omp-desktop",
        """#!/bin/sh
RESOURCES=$(cd "$(dirname "$0")/.." && pwd)
export PATH="$RESOURCES/bin:$PATH"
exec "$RESOURCES/node/bin/node" \
  "$RESOURCES/backend/node_modules/@omp-desktop/cli/dist/index.js" "$@"
""",
    )
    info = {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleDisplayName": "OMP Desktop",
        "CFBundleExecutable": "OMP Desktop",
        "CFBundleIconFile": "icon",
        "CFBundleIdentifier": "sh.omp.desktop.miniblink",
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": "OMP Desktop",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": package_metadata["version"],
        "CFBundleVersion": package_metadata["version"],
        "CFBundleURLTypes": [
            {
                "CFBundleURLName": "sh.omp.desktop",
                "CFBundleURLSchemes": ["omp-desktop"],
            }
        ],
        "LSApplicationCategoryType": "public.app-category.developer-tools",
        "LSMinimumSystemVersion": "13.0",
        "NSHighResolutionCapable": True,
        "NSHumanReadableCopyright": "OMP Desktop contributors",
    }
    with (contents / "Info.plist").open("wb") as output:
        plistlib.dump(info, output, sort_keys=True)
    (contents / "PkgInfo").write_bytes(b"APPL????")
    run(["codesign", "--force", "--deep", "--sign", "-", str(app)], cwd=root)
    print(f"OMP Desktop package: {app}", flush=True)
    return app

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build the Apple Silicon macOS miniblink targets."
    )
    parser.add_argument("--out", default="out/mac-arm64", help="GN output directory")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--run", action="store_true", help="run the headless smoke executable")
    mode.add_argument("--gui", action="store_true", help="build and run the GUI demo")
    mode.add_argument(
        "--electron", action="store_true", help="build and run the Electron API demo"
    )
    mode.add_argument(
        "--omp-desktop",
        action="store_true",
        help="build a self-contained OMP Desktop macOS application",
    )
    parser.add_argument(
        "--frontend",
        default="platform/macos/resources/demo/index.html",
        help="GUI HTML path or HTTP(S) URL",
    )
    parser.add_argument(
        "--electron-main",
        default="platform/macos/resources/electron-demo/main.js",
        help="Electron-compatible main-process JavaScript",
    )
    parser.add_argument(
        "--omp-source",
        default="../omp-desktop",
        help="OMP Desktop source checkout used for application resources",
    )
    parser.add_argument(
        "--app-output",
        default=None,
        help="output path for the packaged .app",
    )
    parser.add_argument(
        "--interactive",
        action="store_true",
        help="keep the Electron demo open until its last window closes",
    )
    parser.add_argument(
        "--screenshot",
        default=None,
        help="GUI or Electron verification snapshot path",
    )
    args = parser.parse_args()

    if sys.platform != "darwin" or platform.machine() != "arm64":
        parser.error("this bootstrap currently supports Apple Silicon macOS only")

    root = Path(__file__).resolve().parents[2]
    ninja = shutil.which("ninja")
    if not ninja:
        parser.error("ninja was not found on PATH")

    gn = find_or_install_gn(root)
    clang_base, clang_version = xcode_clang()
    out_dir = root / args.out

    screenshot = args.screenshot or (
        "/tmp/miniblink132-electron-demo.png"
        if args.electron
        else "/tmp/miniblink132-gui-demo.png"
    )
    target = (
        ELECTRON_TARGET
        if args.electron or args.omp_desktop
        else GUI_TARGET
        if args.gui
        else HEADLESS_TARGET
    )
    run([gn, "gen", str(out_dir), f"--args={gn_args(clang_base, clang_version)}"], cwd=root)
    run([ninja, "-C", str(out_dir), "-j", "16", target], cwd=root)
    if args.omp_desktop:
        source = Path(args.omp_source)
        if not source.is_absolute():
            source = root / source
        package_omp_desktop(
            root,
            out_dir,
            out_dir / target,
            source.resolve(),
            args.app_output,
        )
        return 0
    if args.electron:
        main_script = Path(args.electron_main)
        if not main_script.is_absolute():
            main_script = root / main_script
        command = [str(out_dir / target), str(main_script), screenshot]
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
        run([str(out_dir / target), frontend, screenshot], cwd=root)
    elif args.run:
        run([str(out_dir / target)], cwd=root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
