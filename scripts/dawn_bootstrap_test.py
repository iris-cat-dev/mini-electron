#!/usr/bin/env python3
# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "build"))
from mini_electron.dawn import _archive, _source_checksum


class DawnBootstrapTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / ".build-tools").mkdir()

    def archive(self, name: str, *, timestamp: int = 1, path: str = "source.cc",
                body: bytes = b"pinned source", mode: int = 0o644,
                target: str = "source.cc", reverse: bool = False) -> Path:
        output = self.root / ".build-tools" / name
        source = tarfile.TarInfo(path)
        source.size = len(body)
        source.mode = mode
        link = tarfile.TarInfo("source-link")
        link.type = tarfile.SYMTYPE
        link.linkname = target
        entries = [(source, io.BytesIO(body)), (link, None)]
        if reverse:
            entries.reverse()
        with tarfile.open(output, "w:gz") as package:
            for member, stream in entries:
                member.mtime = timestamp
                member.uid = timestamp
                member.gid = timestamp
                member.uname = str(timestamp)
                member.gname = str(timestamp)
                package.addfile(member, stream)
        return output

    def test_regenerated_archive_keeps_source_identity(self) -> None:
        first = self.archive("first.tar.gz")
        regenerated = self.archive("regenerated.tar.gz", timestamp=1000, reverse=True)
        self.assertNotEqual(first.read_bytes(), regenerated.read_bytes())
        self.assertEqual(_source_checksum(first), _source_checksum(regenerated))

    def test_changed_source_is_rejected(self) -> None:
        checksum = _source_checksum(self.archive("original.tar.gz"))
        for change in ({"body": b"changed source"}, {"path": "other.cc"},
                       {"mode": 0o755}, {"target": "other.cc"}):
            with self.subTest(change=change):
                self.archive("changed.tar.gz", **change)
                with self.assertRaisesRegex(RuntimeError, "Dawn checksum mismatch"):
                    _archive(self.root, "changed.tar.gz", "https://unused.invalid", checksum)


if __name__ == "__main__":
    unittest.main()
