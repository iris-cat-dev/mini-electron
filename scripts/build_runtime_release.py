#!/usr/bin/env python3
# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

import argparse
import json
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


_PACKAGE_FILES = (
    "cli.js",
    "electron.d.ts",
    "index.js",
    "install.js",
    "LICENSE",
    "README.md",
    "package.json",
)
_TARGETS = (
    ("win32", "x64", "windows_dist"),
    ("darwin", "arm64", "macos_dist"),
)


def _validate_distribution(
    directory: Path, platform_name: str, arch: str
) -> dict[str, object]:
    metadata = release_metadata(ROOT, platform_name, arch)
    validate_distribution(directory, metadata)
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
    shutil.copytree(ROOT / "lib", destination / "lib")


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
    inputs: list[tuple[Path, dict[str, object]]] = []
    for platform_name, arch, argument in _TARGETS:
        if f"{platform_name}-{arch}" not in platforms:
            continue
        input_value = getattr(args, argument)
        if input_value is None:
            raise DistributionError(
                f"Runtime distribution for published target {platform_name}-{arch} is required"
            )
        directory = Path(input_value)
        if not directory.is_absolute():
            directory = ROOT / directory
        directory = directory.resolve()
        if not directory.is_dir():
            raise DistributionError(f"Runtime distribution is missing: {directory}")
        if output == directory or output in directory.parents or directory in output.parents:
            raise DistributionError("Release output and runtime distribution inputs must not overlap")
        inputs.append((directory, _validate_distribution(directory, platform_name, arch)))
    if not inputs:
        raise DistributionError("The runtime artifact manifest does not declare a supported target")

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
        for directory, metadata in inputs:
            archive_name = metadata["archive"]
            if not isinstance(archive_name, str):
                raise DistributionError("Runtime archive metadata is invalid")
            archive = release / archive_name
            create_zip(directory, archive)
            digest = sha256(archive)
            checksums.append(f"{digest}  {archive.name}")
            release_platforms[f"{metadata['platform']}-{metadata['arch']}"] = {
                "archive": archive.name,
                "executable": metadata["executable"],
                "sha256": digest,
            }
        (release / "SHASUMS256.txt").write_text(
            "\n".join(sorted(checksums)) + "\n", encoding="ascii"
        )
        manifest_text = json.dumps(release_manifest, indent=2) + "\n"
        (release / "artifacts.json").write_text(manifest_text, encoding="utf-8")
        channel = staged / f"runtime-v{release_manifest['installerProtocol']}"
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
    parser.add_argument("--macos-dist", help="darwin-arm64 runtime-dist directory")
    parser.add_argument("--output", default="out/runtime-release", help="release output directory")
    parser.add_argument("--runtime-only", action="store_true", help="skip staging the npm package")
    args = parser.parse_args()
    try:
        output = assemble(args)
    except (DistributionError, OSError, ValueError) as error:
        print(f"release assembly failed: {error}", file=sys.stderr)
        return 1
    print(f"Runtime release: {output}")
    if not args.runtime_only:
        print(f"npm pack input: {output / 'npm-package'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
