#!/usr/bin/env python3
# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch
import uuid
import zipfile

import build_runtime_release as release
from mini_electron.distribution import (
    DistributionError,
    release_metadata,
    sha256,
    validate_distribution,
    write_version_metadata,
)
from mini_electron.windows_symbols import SymbolArchiveError, archive_windows_symbols
from windows_symbols_test import image_fixture, pdb_fixture


class RuntimeReleaseTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.manifest = {
            "schemaVersion": 2,
            "installerProtocol": 1,
            "runtimeVersion": "9.8.7",
            "electronApiVersion": "41.2.0",
            "nodeVersion": "22.14.0",
            "nodeModuleAbi": 134,
            "chromiumMajorVersion": 134,
            "downloadBaseUrl": "https://example.com/releases/download",
            "platforms": {
                "win32-x64": {
                    "archive": "mini-electron-v9.8.7-win32-x64.zip",
                    "executable": "electron.exe",
                }
            },
        }
        self.write_manifest()
        # npm's supported targets and version deliberately disagree with runtime inputs.
        (self.root / "package.json").write_text(
            json.dumps({"version": "1.0.0", "os": ["darwin"], "cpu": ["arm64"]}),
            encoding="utf-8",
        )
        source = self.root / release._NPM_SOURCE
        (source / "lib").mkdir(parents=True)
        for name in ("cli.js", "electron.d.ts", "index.js", "install.js"):
            (source / name).write_text(name, encoding="utf-8")
        (source / "lib" / "paths.js").write_text("module.exports = {};", encoding="utf-8")
        for name in release._PACKAGE_FILES:
            if name != "package.json":
                (self.root / name).write_text(name, encoding="utf-8")
        self.distribution = self.root / "runtime-dist"
        self.distribution.mkdir()
        for name in ("LICENSE", "LICENSES.chromium.html", "LICENSES.leveldb"):
            (self.distribution / name).write_text(name, encoding="utf-8")
        self.guid = uuid.UUID("12345678-1122-3344-5566-778899aabbcc")
        built = self.root / "built"
        built.mkdir()
        image_fixture(built / "mini-electron.exe", self.guid, 10)
        pdb_fixture(built / "mini-electron.pdb", self.guid, 10)
        self.symbols = archive_windows_symbols(built / "mini-electron.exe", self.root / "symbols")
        shutil.copy2(built / "mini-electron.exe", self.distribution / "electron.exe")
        self.refresh_metadata()
        self.args = argparse.Namespace(
            output=str(self.root / "release"),
            windows_dist=str(self.distribution),
            windows_symbols=str(self.symbols),
            macos_dist=None,
            runtime_only=False,
        )

    def write_manifest(self) -> None:
        (self.root / "artifacts.json").write_text(json.dumps(self.manifest), encoding="utf-8")

    def refresh_metadata(self) -> None:
        write_version_metadata(self.distribution, release_metadata(self.root, "win32", "x64"))

    def assemble(self) -> Path:
        with patch.object(release, "ROOT", self.root):
            return release.assemble(self.args)

    def test_independent_runtime_and_channel_manifest(self) -> None:
        output = self.assemble()
        immutable = output / "v9.8.7"
        remote = json.loads((immutable / "artifacts.json").read_text(encoding="utf-8"))
        self.assertEqual(remote["installerProtocol"], 1)
        self.assertNotIn("packageVersion", remote)
        self.assertEqual(
            (immutable / "artifacts.json").read_bytes(),
            (output / "runtime-v1" / "artifacts.json").read_bytes(),
        )
        target = remote["platforms"]["win32-x64"]
        self.assertEqual(target["sha256"], sha256(immutable / target["archive"]))
        symbols_name = "mini-electron-v9.8.7-win32-x64-symbols.zip"
        self.assertEqual(
            (immutable / "SHASUMS256.txt").read_text(encoding="ascii"),
            "\n".join(sorted((
                f"{target['sha256']}  {target['archive']}",
                f"{sha256(immutable / symbols_name)}  {symbols_name}",
            ))) + "\n",
        )
        self.assertEqual(set(target), {"archive", "executable", "sha256"})
        with zipfile.ZipFile(immutable / target["archive"]) as archive:
            self.assertFalse(any(name.endswith(".pdb") or name == "symbols.json" for name in archive.namelist()))
        with zipfile.ZipFile(immutable / symbols_name) as archive:
            self.assertEqual(set(archive.namelist()), {"mini-electron.pdb", "symbols.json"})
            self.assertEqual(archive.read("mini-electron.pdb"), (self.symbols / "mini-electron.pdb").read_bytes())
            metadata = json.loads(archive.read("symbols.json"))
            self.assertEqual(metadata["imageSha256"], sha256(self.distribution / "electron.exe"))
            self.assertEqual(metadata["pdbSha256"], sha256(self.symbols / "mini-electron.pdb"))
            self.assertEqual(metadata["pdbGuid"], self.guid.hex.upper())
            self.assertEqual(metadata["pdbAge"], 10)
            self.assertEqual(archive.getinfo("mini-electron.pdb").compress_type, zipfile.ZIP_DEFLATED)
        npm = output / "npm-package"
        self.assertFalse((npm / "artifacts.json").exists())
        npm_inputs = {str(p.relative_to(npm)): p.read_bytes() for p in npm.rglob("*") if p.is_file()}
        self.assertFalse(any(Path(name).suffix.casefold() == ".pdb" for name in npm_inputs))
        self.manifest["runtimeVersion"] = "9.8.8"
        self.manifest["platforms"]["win32-x64"]["archive"] = "mini-electron-v9.8.8-win32-x64.zip"
        self.write_manifest()
        self.refresh_metadata()
        self.assemble()
        self.assertTrue((output / "v9.8.8" / "artifacts.json").is_file())
        self.assertEqual(
            npm_inputs,
            {str(p.relative_to(npm)): p.read_bytes() for p in npm.rglob("*") if p.is_file()},
        )
        metadata = validate_distribution(self.distribution)
        self.assertEqual(metadata["runtimeVersion"], "9.8.8")
        self.assertNotIn("packageVersion", metadata)

    def test_windows_channel_does_not_update_legacy_channel(self) -> None:
        self.manifest["platforms"]["win32-x64"]["runtimeChannel"] = (
            "runtime-v1-win32-x64"
        )
        self.manifest["platforms"]["darwin-arm64"] = {
            "archive": "mini-electron-v9.8.7-darwin-arm64.zip",
            "executable": "Mini Electron.app/Contents/MacOS/Mini Electron",
            "runtimeChannel": "runtime-v1",
        }
        self.write_manifest()
        self.args.runtime_only = True

        output = self.assemble()
        immutable_manifest = output / "v9.8.7" / "artifacts.json"
        windows_channel = output / "runtime-v1-win32-x64" / "artifacts.json"
        self.assertEqual(immutable_manifest.read_bytes(), windows_channel.read_bytes())
        self.assertFalse((output / "runtime-v1").exists())
        self.assertFalse((output / "npm-package").exists())
        remote = json.loads(immutable_manifest.read_text(encoding="utf-8"))
        self.assertEqual(set(remote["platforms"]), {"win32-x64"})

    def test_partial_assembly_rejects_omitted_target_on_selected_channel(self) -> None:
        output = self.assemble()
        channel = output / "runtime-v1" / "artifacts.json"
        original = channel.read_bytes()
        self.manifest["platforms"]["darwin-arm64"] = {
            "archive": "mini-electron-v9.8.7-darwin-arm64.zip",
            "executable": "Mini Electron.app/Contents/MacOS/Mini Electron",
        }
        self.write_manifest()

        with self.assertRaisesRegex(
            DistributionError, "sharing selected channel runtime-v1 is required"
        ):
            self.assemble()

        self.assertEqual(channel.read_bytes(), original)

    def test_unsafe_channel_preserves_existing_release(self) -> None:
        self.manifest["platforms"]["win32-x64"]["runtimeChannel"] = (
            "runtime-v1-win32-x64"
        )
        self.write_manifest()
        output = self.assemble()
        channel = output / "runtime-v1-win32-x64" / "artifacts.json"
        original = channel.read_bytes()

        self.manifest["platforms"]["win32-x64"]["runtimeChannel"] = "../escaped"
        self.write_manifest()
        with self.assertRaisesRegex(DistributionError, "channel name is unsafe"):
            self.assemble()

        self.assertEqual(channel.read_bytes(), original)
        self.assertFalse((output.parent / "escaped").exists())

    def test_runtime_only_needs_no_npm_inputs(self) -> None:
        for name in release._PACKAGE_FILES:
            (self.root / name).unlink()
        shutil.rmtree(self.root / release._NPM_SOURCE)
        self.args.runtime_only = True
        output = self.assemble()
        self.assertFalse((output / "npm-package").exists())
        self.assertTrue((output / "runtime-v1" / "artifacts.json").is_file())

    def test_old_identity_is_rejected(self) -> None:
        version = self.distribution / "version.json"
        metadata = json.loads(version.read_text(encoding="utf-8"))
        metadata["packageVersion"] = metadata.pop("installerProtocol")
        version.write_text(json.dumps(metadata), encoding="utf-8")
        with self.assertRaisesRegex(DistributionError, "installerProtocol"):
            validate_distribution(self.distribution)

    def test_invalid_inventory_preserves_existing_release(self) -> None:
        output = self.assemble()
        channel = output / "runtime-v1" / "artifacts.json"
        original = channel.read_bytes()
        (self.distribution / "electron.exe").write_text("changed", encoding="utf-8")
        with self.assertRaises(DistributionError):
            self.assemble()
        self.assertEqual(channel.read_bytes(), original)

    def release_snapshot(self) -> dict[str, bytes]:
        output = Path(self.args.output)
        return {
            str(path.relative_to(output)): path.read_bytes()
            for path in output.rglob("*") if path.is_file()
        }

    def assert_symbol_failure_preserves_release(self, message: str) -> None:
        original = self.release_snapshot()
        with self.assertRaisesRegex((DistributionError, SymbolArchiveError), message):
            self.assemble()
        self.assertEqual(self.release_snapshot(), original)
        self.assertFalse(list(self.root.glob(".release.stage-*")))
        self.assertFalse(list(self.root.glob(".release.replace-*")))

    def test_windows_requires_symbols_and_preserves_existing_output(self) -> None:
        self.assemble()
        self.args.windows_symbols = None
        self.assert_symbol_failure_preserves_release("--windows-symbols is required")

    def test_missing_symbol_directory_preserves_existing_output(self) -> None:
        self.assemble()
        self.args.windows_symbols = str(self.root / "missing-symbols")
        self.assert_symbol_failure_preserves_release("archive is missing or unsafe")

    def test_unsafe_symbol_metadata_paths_preserve_existing_output(self) -> None:
        self.assemble()
        document = self.symbols / "symbols.json"
        original = document.read_bytes()
        for name in ("../other.pdb", "..\\other.pdb", "C:\\other.pdb", "other.pdb:stream"):
            with self.subTest(name=name):
                metadata = json.loads(original)
                metadata["pdbName"] = name
                document.write_text(json.dumps(metadata), encoding="utf-8")
                self.assert_symbol_failure_preserves_release("Invalid Windows symbol filename")

    def test_symbols_archive_cannot_replace_the_runtime_archive(self) -> None:
        self.assemble()
        self.manifest["platforms"]["win32-x64"]["archive"] = (
            "mini-electron-v9.8.7-win32-x64-symbols.zip"
        )
        self.write_manifest()
        self.assert_symbol_failure_preserves_release("Release archive names collide")

    def test_missing_symbol_inputs_preserve_existing_output(self) -> None:
        self.assemble()
        for name in ("symbols.json", "mini-electron.exe", "mini-electron.pdb"):
            with self.subTest(name=name):
                source = self.symbols / name
                original = source.read_bytes()
                source.unlink()
                try:
                    self.assert_symbol_failure_preserves_release("missing or unsafe")
                finally:
                    source.write_bytes(original)

    def test_mismatched_pdb_guid_and_age_preserve_existing_output(self) -> None:
        self.assemble()
        for guid, age in ((uuid.uuid4(), 10), (self.guid, 11)):
            with self.subTest(guid=guid, age=age):
                pdb_fixture(self.symbols / "mini-electron.pdb", guid, age)
                self.assert_symbol_failure_preserves_release("PDB does not match")

    def test_changed_runtime_with_same_rsds_identity_is_rejected(self) -> None:
        self.assemble()
        image = self.distribution / "electron.exe"
        image.write_bytes(image.read_bytes() + b"another build")
        self.refresh_metadata()
        self.assert_symbol_failure_preserves_release("Archived executable does not match")

    def test_metadata_hashes_cannot_mask_changed_symbol_inputs(self) -> None:
        self.assemble()
        document = self.symbols / "symbols.json"
        original = document.read_bytes()
        for key in ("imageSha256", "pdbSha256", "pdbGuid", "pdbAge"):
            with self.subTest(key=key):
                metadata = json.loads(original)
                metadata[key] = 11 if key == "pdbAge" else "0" * 64
                document.write_text(json.dumps(metadata), encoding="utf-8")
                self.assert_symbol_failure_preserves_release("metadata does not match")

    def test_corrupt_archived_files_are_rejected_even_with_matching_metadata(self) -> None:
        self.assemble()
        for name, message in (
            ("mini-electron.exe", "Archived executable does not match"),
            ("mini-electron.pdb", "metadata does not match"),
        ):
            with self.subTest(name=name):
                source = self.symbols / name
                original = source.read_bytes()
                source.write_bytes(original + b"corruption")
                try:
                    self.assert_symbol_failure_preserves_release(message)
                finally:
                    source.write_bytes(original)

    def test_symbols_changed_during_zip_preserve_existing_output(self) -> None:
        self.assemble()
        original_open = zipfile.ZipFile.open
        pdb = self.symbols / "mini-electron.pdb"

        def changing_open(archive, name, mode="r", *args, **kwargs):
            if mode == "w" and isinstance(name, zipfile.ZipInfo) and name.filename.endswith(".pdb"):
                with pdb.open("r+b") as source:
                    source.seek(-1, 2)
                    source.write(b"X")
            return original_open(archive, name, mode, *args, **kwargs)

        with patch.object(zipfile.ZipFile, "open", new=changing_open):
            self.assert_symbol_failure_preserves_release("PDB changed while archiving")

    def test_symbols_never_enter_runtime_zip_even_if_inventory_allows_them(self) -> None:
        self.assemble()
        shutil.copy2(self.symbols / "mini-electron.pdb", self.distribution / "mini-electron.pdb")
        self.refresh_metadata()
        self.assert_symbol_failure_preserves_release("must not contain debug symbols")

    def test_symbol_input_must_not_overlap_output(self) -> None:
        self.assemble()
        self.args.windows_symbols = self.args.output
        self.assert_symbol_failure_preserves_release("must not overlap")

    @unittest.skipIf(os.name == "nt", "Windows cannot represent macOS executable mode bits")
    def test_macos_only_release_needs_no_windows_symbols(self) -> None:
        self.manifest["platforms"]["win32-x64"]["runtimeChannel"] = "runtime-v1-win32-x64"
        self.manifest["platforms"]["darwin-arm64"] = {
            "archive": "mini-electron-v9.8.7-darwin-arm64.zip",
            "executable": "Mini Electron.app/Contents/MacOS/Mini Electron",
        }
        self.write_manifest()
        macos = self.root / "macos-dist"
        executable = macos / "Mini Electron.app/Contents/MacOS/Mini Electron"
        executable.parent.mkdir(parents=True)
        executable.write_bytes(b"macos executable")
        executable.chmod(0o755)
        for name in ("LICENSE", "LICENSES.chromium.html", "LICENSES.leveldb"):
            (macos / name).write_text(name, encoding="utf-8")
        notices = macos / "LICENSES.squirrel"
        notices.mkdir()
        for name in ("Squirrel.Mac.LICENSE", "Mantle.LICENSE.md", "ReactiveObjC.LICENSE.md", "Sparkle.LICENSE"):
            (notices / name).write_text(name, encoding="utf-8")
        write_version_metadata(macos, release_metadata(self.root, "darwin", "arm64"))
        self.args.windows_dist = None
        self.args.windows_symbols = None
        self.args.macos_dist = str(macos)
        self.args.runtime_only = True
        output = self.assemble()
        immutable = output / "v9.8.7"
        self.assertEqual(
            {path.name for path in immutable.iterdir()},
            {"mini-electron-v9.8.7-darwin-arm64.zip", "SHASUMS256.txt", "artifacts.json"},
        )
        self.assertEqual(
            (immutable / "artifacts.json").read_bytes(),
            (output / "runtime-v1" / "artifacts.json").read_bytes(),
        )
        self.assertEqual(len((immutable / "SHASUMS256.txt").read_text("ascii").splitlines()), 1)



if __name__ == "__main__":
    unittest.main()
