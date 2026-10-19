#!/usr/bin/env python3
# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

import argparse
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import build_runtime_release as release
from mini_electron.distribution import (
    DistributionError,
    release_metadata,
    sha256,
    validate_distribution,
    write_version_metadata,
)


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
        for name in release._PACKAGE_FILES:
            if name != "package.json":
                (self.root / name).write_text(name, encoding="utf-8")
        (self.root / "lib").mkdir()
        (self.root / "lib" / "paths.js").write_text("module.exports = {};", encoding="utf-8")
        self.distribution = self.root / "runtime-dist"
        self.distribution.mkdir()
        for name in ("electron.exe", "LICENSE", "LICENSES.chromium.html", "LICENSES.leveldb"):
            (self.distribution / name).write_text(name, encoding="utf-8")
        self.refresh_metadata()
        self.args = argparse.Namespace(
            output=str(self.root / "release"),
            windows_dist=str(self.distribution),
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
        self.assertEqual(
            (immutable / "SHASUMS256.txt").read_text(encoding="ascii"),
            f"{target['sha256']}  {target['archive']}\n",
        )
        npm = output / "npm-package"
        self.assertFalse((npm / "artifacts.json").exists())
        npm_inputs = {str(p.relative_to(npm)): p.read_bytes() for p in npm.rglob("*") if p.is_file()}
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

    def test_runtime_only_needs_no_npm_inputs(self) -> None:
        for name in release._PACKAGE_FILES:
            (self.root / name).unlink()
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


if __name__ == "__main__":
    unittest.main()
