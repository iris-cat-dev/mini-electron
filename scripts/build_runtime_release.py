#!/usr/bin/env python3
# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build"))

from mini_electron.distribution import (  # noqa: E402
    DistributionError,
    create_zip,
    path_is_link,
    release_metadata,
    sha256,
    validate_distribution,
)
from mini_electron.windows_symbols import (  # noqa: E402
    SymbolArchiveError,
    create_windows_symbols_zip,
    validate_windows_symbols,
)


_PACKAGE_FILES = (
    "LICENSE",
    "README.md",
    "package.json",
)
_NPM_SOURCE = Path("src") / "npm"
_TARGETS = (
    ("win32", "x64", "windows_dist"),
    ("darwin", "arm64", "macos_dist"),
)

_CHANNEL_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]*")


def _runtime_channel(platform: object, installer_protocol: object) -> str:
    if not isinstance(platform, dict):
        raise DistributionError("Runtime platform metadata is invalid")
    channel = platform.get("runtimeChannel")
    if channel is None:
        channel = f"runtime-v{installer_protocol}"
    if not isinstance(channel, str) or _CHANNEL_NAME.fullmatch(channel) is None:
        raise DistributionError(f"Runtime channel name is unsafe: {channel!r}")
    return channel



def _validate_distribution(
    directory: Path, platform_name: str, arch: str
) -> dict[str, object]:
    metadata = release_metadata(ROOT, platform_name, arch)
    inventory = validate_distribution(directory, metadata)
    if platform_name == "win32" and any(
        Path(name).suffix.casefold() == ".pdb"
        or Path(name).name.casefold() == "symbols.json"
        for name in inventory["files"]
    ):
        raise DistributionError("Windows runtime distribution must not contain debug symbols")
    required_notices = (
        directory / "LICENSE",
        directory / "LICENSES.chromium.html",
        directory / "LICENSES.leveldb",
    )
    if any(not notice.is_file() or notice.stat().st_size == 0 for notice in required_notices):
        raise DistributionError(
            f"Runtime distribution license notices are incomplete: {directory}"
        )
    if platform_name == "darwin":
        squirrel_notices = directory / "LICENSES.squirrel"
        required_squirrel_notices = (
            "Squirrel.Mac.LICENSE",
            "Mantle.LICENSE.md",
            "ReactiveObjC.LICENSE.md",
            "Sparkle.LICENSE",
        )
        if any(
            not (squirrel_notices / name).is_file()
            or (squirrel_notices / name).stat().st_size == 0
            for name in required_squirrel_notices
        ):
            raise DistributionError(
                f"macOS updater license notices are incomplete: {directory}"
            )
    return metadata


def _copy_npm_package(destination: Path) -> None:
    destination.mkdir()
    for relative in _PACKAGE_FILES:
        source = ROOT / relative
        if not source.is_file():
            raise DistributionError(f"npm package input is missing: {source}")
        shutil.copy2(source, destination / relative)
    source = ROOT / _NPM_SOURCE
    if not source.is_dir():
        raise DistributionError(f"npm package input is missing: {source}")
    shutil.copytree(source, destination / _NPM_SOURCE)


def assemble(args: argparse.Namespace) -> Path:
    output = Path(args.output)
    if not output.is_absolute():
        output = ROOT / output
    if path_is_link(output):
        raise DistributionError(
            f"Release output must not be a symlink or junction: {output}"
        )
    output = output.resolve()
    manifest = json.loads((ROOT / "artifacts.json").read_text(encoding="utf-8"))
    platforms = manifest.get("platforms")
    if not isinstance(platforms, dict) or not platforms:
        raise DistributionError("Runtime artifact manifest must declare platforms")
    supported = {f"{platform}-{arch}" for platform, arch, _ in _TARGETS}
    unsupported = platforms.keys() - supported
    if unsupported:
        raise DistributionError("Unsupported runtime targets: " + ", ".join(sorted(unsupported)))
    declared: list[tuple[str, str, str, str]] = []
    for platform_name, arch, argument in _TARGETS:
        target = f"{platform_name}-{arch}"
        if target not in platforms:
            continue
        declared.append(
            (
                platform_name,
                arch,
                argument,
                _runtime_channel(
                    platforms[target], manifest.get("installerProtocol")
                ),
            )
        )
    selected_channel_keys = {
        channel.casefold()
        for _, _, argument, channel in declared
        if getattr(args, argument) is not None
    }
    if not selected_channel_keys:
        raise DistributionError("A runtime distribution for a declared target is required")

    windows_symbols: Path | None = None
    symbols_value = getattr(args, "windows_symbols", None)
    if symbols_value is not None:
        if getattr(args, "windows_dist", None) is None:
            raise DistributionError("--windows-symbols requires --windows-dist")
        windows_symbols = Path(symbols_value)
        if not windows_symbols.is_absolute():
            windows_symbols = ROOT / windows_symbols
        if path_is_link(windows_symbols):
            raise DistributionError("Windows symbol archive must not be a symlink or junction")
        windows_symbols = windows_symbols.resolve()
        if output == windows_symbols or output in windows_symbols.parents or windows_symbols in output.parents:
            raise DistributionError("Release output and Windows symbol archive must not overlap")
    inputs: list[tuple[Path, dict[str, object], str]] = []
    for platform_name, arch, argument, channel in declared:
        input_value = getattr(args, argument)
        if input_value is None:
            if channel.casefold() in selected_channel_keys:
                raise DistributionError(
                    f"Runtime distribution for target {platform_name}-{arch} sharing "
                    f"selected channel {channel} is required"
                )
            continue
        directory = Path(input_value)
        if not directory.is_absolute():
            directory = ROOT / directory
        directory = directory.resolve()
        if not directory.is_dir():
            raise DistributionError(f"Runtime distribution is missing: {directory}")
        if output == directory or output in directory.parents or directory in output.parents:
            raise DistributionError("Release output and runtime distribution inputs must not overlap")
        inputs.append(
            (
                directory,
                _validate_distribution(directory, platform_name, arch),
                channel,
            )
        )
        if platform_name == "win32":
            if windows_symbols is None:
                raise DistributionError("--windows-symbols is required with --windows-dist")
            metadata = inputs[-1][1]
            validate_windows_symbols(windows_symbols, directory / str(metadata["executable"]))

    output.parent.mkdir(parents=True, exist_ok=True)
    staged = Path(tempfile.mkdtemp(prefix=f".{output.name}.stage-", dir=output.parent))
    try:
        if not getattr(args, "runtime_only", False):
            _copy_npm_package(staged / "npm-package")
        checksums: list[str] = []
        release = staged / f"v{inputs[0][1]['runtimeVersion']}"
        release.mkdir()
        release_manifest = {
            name: manifest[name]
            for name in (
                "schemaVersion", "installerProtocol", "runtimeVersion",
                "electronApiVersion", "nodeVersion", "nodeModuleAbi",
                "chromiumMajorVersion", "downloadBaseUrl",
            )
        }
        release_platforms: dict[str, object] = {}
        release_manifest["platforms"] = release_platforms
        for directory, metadata, _ in inputs:
            archive_name = metadata["archive"]
            if not isinstance(archive_name, str):
                raise DistributionError("Runtime archive metadata is invalid")
            archive = release / archive_name
            if archive.exists():
                raise DistributionError(f"Release archive names collide: {archive.name}")
            create_zip(directory, archive)
            digest = sha256(archive)
            checksums.append(f"{digest}  {archive.name}")
            release_platforms[f"{metadata['platform']}-{metadata['arch']}"] = {
                "archive": archive.name,
                "executable": metadata["executable"],
                "sha256": digest,
            }
            if metadata["platform"] == "win32":
                assert windows_symbols is not None
                symbols_name = (
                    f"mini-electron-v{metadata['runtimeVersion']}-"
                    f"win32-{metadata['arch']}-symbols.zip"
                )
                symbols_archive = release / symbols_name
                if symbols_archive.exists():
                    raise DistributionError(f"Release archive names collide: {symbols_archive.name}")
                create_windows_symbols_zip(
                    windows_symbols,
                    directory / str(metadata["executable"]),
                    symbols_archive,
                )
                checksums.append(f"{sha256(symbols_archive)}  {symbols_archive.name}")
        (release / "SHASUMS256.txt").write_text(
            "\n".join(sorted(checksums)) + "\n", encoding="ascii"
        )
        manifest_text = json.dumps(release_manifest, indent=2) + "\n"
        (release / "artifacts.json").write_text(manifest_text, encoding="utf-8")
        channels = {channel.casefold(): channel for _, _, channel in inputs}
        for channel_name in sorted(channels.values()):
            channel = staged / channel_name
            channel.mkdir()
            (channel / "artifacts.json").write_text(manifest_text, encoding="utf-8")

        backup_holder: Path | None = None
        backup: Path | None = None
        if output.exists():
            backup_holder = Path(
                tempfile.mkdtemp(
                    prefix=f".{output.name}.replace-", dir=output.parent
                )
            )
            backup = backup_holder / "previous"
            output.rename(backup)
        try:
            staged.rename(output)
        except BaseException:
            if backup is not None and backup.exists() and not output.exists():
                backup.rename(output)
            raise
        finally:
            if backup_holder is not None and backup_holder.exists():
                shutil.rmtree(backup_holder)
    except BaseException:
        if staged.exists():
            shutil.rmtree(staged)
        raise
    return output


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Assemble declared runtime artifacts and optionally the mini-electron npm package."
    )
    parser.add_argument("--windows-dist", help="win32-x64 runtime-dist directory")
    parser.add_argument(
        "--windows-symbols",
        help="required with --windows-dist: archived leaf containing symbols.json, EXE and matching PDB",
    )
    parser.add_argument("--macos-dist", help="darwin-arm64 runtime-dist directory")
    parser.add_argument("--output", default="out/runtime-release", help="release output directory")
    parser.add_argument("--runtime-only", action="store_true", help="skip staging the npm package")
    args = parser.parse_args()
    try:
        output = assemble(args)
    except (DistributionError, SymbolArchiveError, OSError, ValueError) as error:
        print(f"release assembly failed: {error}", file=sys.stderr)
        return 1
    print(f"Runtime release: {output}")
    if not args.runtime_only:
        print(f"npm pack input: {output / 'npm-package'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
