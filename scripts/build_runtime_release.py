#!/usr/bin/env python3
# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build"))

from mini_electron.distribution import (  # noqa: E402
    DistributionError,
    create_zip,
    release_metadata,
    sha256,
    validate_distribution,
)


_PACKAGE_FILES = (
    "artifacts.json",
    "cli.js",
    "electron.d.ts",
    "index.js",
    "install.js",
    "LICENSE",
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
    if output.is_symlink() or output.is_junction():
        raise DistributionError(
            f"Release output must not be a symlink or junction: {output}"
        )
    output = output.resolve()
    inputs: list[tuple[Path, dict[str, object]]] = []
    for platform_name, arch, argument in _TARGETS:
        directory = Path(getattr(args, argument))
        if not directory.is_absolute():
            directory = ROOT / directory
        directory = directory.resolve()
        if not directory.is_dir():
            raise DistributionError(f"Runtime distribution is missing: {directory}")
        if output == directory or output in directory.parents or directory in output.parents:
            raise DistributionError("Release output and runtime distribution inputs must not overlap")
        inputs.append((directory, _validate_distribution(directory, platform_name, arch)))

    output.parent.mkdir(parents=True, exist_ok=True)
    staged = Path(tempfile.mkdtemp(prefix=f".{output.name}.stage-", dir=output.parent))
    try:
        npm_package = staged / "npm-package"
        _copy_npm_package(npm_package)
        checksums: list[str] = []
        release = staged / f"v{inputs[0][1]['runtimeVersion']}"
        release.mkdir()
        for directory, metadata in inputs:
            archive_name = metadata["archive"]
            if not isinstance(archive_name, str):
                raise DistributionError("Runtime archive metadata is invalid")
            archive = release / archive_name
            create_zip(directory, archive)
            checksums.append(f"{sha256(archive)}  {archive.name}")
        (release / "SHASUMS256.txt").write_text(
            "\n".join(sorted(checksums)) + "\n", encoding="ascii"
        )

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
        description="Assemble both runtime zips and the publishable mini-electron npm package."
    )
    parser.add_argument("--windows-dist", required=True, help="win32-x64 runtime-dist directory")
    parser.add_argument("--macos-dist", required=True, help="darwin-arm64 runtime-dist directory")
    parser.add_argument("--output", default="out/runtime-release", help="release output directory")
    args = parser.parse_args()
    try:
        output = assemble(args)
    except (DistributionError, OSError, ValueError) as error:
        print(f"release assembly failed: {error}", file=sys.stderr)
        return 1
    print(f"Runtime release: {output}")
    print(f"npm pack input: {output / 'npm-package'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
