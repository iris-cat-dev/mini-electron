#!/usr/bin/env python3
# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

from pathlib import Path
import hashlib
import json
import struct
import sys
import tempfile
import unittest
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "build"))
from mini_electron.windows_symbols import SymbolArchiveError, archive_windows_symbols  # noqa: E402


def image_fixture(path: Path, guid: uuid.UUID, age: int) -> None:
    data = bytearray(2048)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x86, 1)
    struct.pack_into("<H", data, 0x94, 240)
    struct.pack_into("<H", data, 0x98, 0x20B)
    struct.pack_into("<II", data, 0x98 + 112 + 6 * 8, 0x1000, 28)
    section = 0x98 + 240
    struct.pack_into("<III", data, section + 12, 0x1000, 1536, 512)
    rsds = b"RSDS" + guid.bytes_le + struct.pack("<I", age) + b"C:\\build\\mini-electron.pdb\0"
    struct.pack_into("<IIII", data, 512 + 12, 2, len(rsds), 0x1020, 544)
    data[544:544 + len(rsds)] = rsds
    path.write_bytes(data)


def pdb_fixture(path: Path, guid: uuid.UUID, age: int) -> None:
    data = bytearray(6 * 512)
    data[:32] = b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0"
    struct.pack_into("<6I", data, 32, 512, 1, 6, 16, 0, 3)
    struct.pack_into("<4I", data, 2 * 512, 2, 0, 28, 4)
    struct.pack_into("<I", data, 3 * 512, 2)
    struct.pack_into("<III16s", data, 4 * 512, 20000404, 0, age, guid.bytes_le)
    path.write_bytes(data)


class WindowsSymbolsTest(unittest.TestCase):
    def test_archives_matching_identity_and_preserves_the_exact_symbol_pair(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            guid = uuid.UUID("12345678-1122-3344-5566-778899aabbcc")
            age = 10
            image = root / "mini-electron.exe"
            pdb = root / "mini-electron.pdb"
            image_fixture(image, guid, age)
            pdb_fixture(pdb, guid, age)

            destination = archive_windows_symbols(image, root / "symbols")
            self.assertEqual(destination, root / "symbols" / "mini-electron.pdb" / (guid.hex.upper() + "A"))
            self.assertEqual((destination / image.name).read_bytes(), image.read_bytes())
            self.assertEqual((destination / pdb.name).read_bytes(), pdb.read_bytes())
            self.assertEqual(json.loads((destination / "symbols.json").read_text("utf-8")), {
                "pdbGuid": guid.hex.upper(),
                "pdbAge": age,
                "pdbName": pdb.name,
                "imageName": image.name,
                "imageSha256": hashlib.sha256(image.read_bytes()).hexdigest(),
                "pdbSha256": hashlib.sha256(pdb.read_bytes()).hexdigest(),
            })
            self.assertEqual(archive_windows_symbols(image, root / "symbols"), destination)

    def test_rejects_pdb_from_another_build_even_with_the_same_filename(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            guid = uuid.UUID("12345678-1122-3344-5566-778899aabbcc")
            other = uuid.UUID("87654321-1122-3344-5566-778899aabbcc")
            image_fixture(root / "mini-electron.exe", guid, 1)
            pdb_fixture(root / "mini-electron.pdb", other, 1)
            with self.assertRaisesRegex(SymbolArchiveError, "PDB does not match"):
                archive_windows_symbols(root / "mini-electron.exe", root / "symbols")

    def test_rejects_an_incremental_pdb_age_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            guid = uuid.UUID("12345678-1122-3344-5566-778899aabbcc")
            image_fixture(root / "mini-electron.exe", guid, 2)
            pdb_fixture(root / "mini-electron.pdb", guid, 1)
            with self.assertRaisesRegex(SymbolArchiveError, "PDB does not match"):
                archive_windows_symbols(root / "mini-electron.exe", root / "symbols")


if __name__ == "__main__":
    unittest.main()
