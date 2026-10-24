# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

import json
import mmap
from pathlib import Path, PureWindowsPath
import shutil
import struct
import tempfile
import uuid

from .distribution import path_is_link, sha256


class SymbolArchiveError(RuntimeError):
    pass


def _unpack(format: str, data: object, offset: int) -> tuple:
    if offset < 0:
        raise SymbolArchiveError("Negative binary offset in Windows symbol input")
    try:
        return struct.unpack_from(format, data, offset)
    except (struct.error, ValueError) as error:
        raise SymbolArchiveError("Truncated Windows symbol input") from error


def _image_identity(image: Path) -> dict[str, object]:
    with image.open("rb") as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if data[:2] != b"MZ":
            raise SymbolArchiveError(f"Not a PE executable: {image}")
        pe, = _unpack("<I", data, 0x3C)
        if data[pe:pe + 4] != b"PE\0\0":
            raise SymbolArchiveError(f"Invalid PE header: {image}")
        sections, = _unpack("<H", data, pe + 6)
        optional_size, = _unpack("<H", data, pe + 20)
        optional = pe + 24
        magic, = _unpack("<H", data, optional)
        if magic not in (0x10B, 0x20B):
            raise SymbolArchiveError(f"Unsupported PE optional header: {image}")
        directory = optional + (96 if magic == 0x10B else 112)
        debug_rva, debug_size = _unpack("<II", data, directory + 6 * 8)

        def file_offset(rva: int, size: int) -> int:
            for index in range(sections):
                section = optional + optional_size + index * 40
                address, raw_size, raw = _unpack("<III", data, section + 12)
                if address <= rva and rva - address <= raw_size and size <= raw_size - (rva - address):
                    result = raw + rva - address
                    if result + size <= len(data):
                        return result
            raise SymbolArchiveError(f"Invalid PE debug directory: {image}")

        debug = file_offset(debug_rva, debug_size)
        for offset in range(debug, debug + debug_size, 28):
            kind, size, _, raw = _unpack("<IIII", data, offset + 12)
            if kind != 2 or size < 25 or raw + size > len(data) or data[raw:raw + 4] != b"RSDS":
                continue
            age, = _unpack("<I", data, raw + 20)
            name = PureWindowsPath(bytes(data[raw + 24:raw + size]).split(b"\0", 1)[0].decode("utf-8")).name
            if not name or name in (".", "..") or ":" in name:
                raise SymbolArchiveError(f"Invalid PDB filename: {image}")
            return {"pdbGuid": uuid.UUID(bytes_le=bytes(data[raw + 4:raw + 20])).hex.upper(),
                    "pdbAge": age, "pdbName": name}
    raise SymbolArchiveError(f"Executable has no RSDS symbol identity: {image}")


def _pdb_identity(pdb: Path) -> tuple[str, int]:
    with pdb.open("rb") as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if data[:32] != b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0":
            raise SymbolArchiveError(f"Unsupported PDB container: {pdb}")
        block_size, _, _, directory_size, _, block_map = _unpack("<6I", data, 32)
        if block_size < 512 or block_size > 65536 or block_size & (block_size - 1):
            raise SymbolArchiveError(f"Invalid PDB block size: {pdb}")
        count = (directory_size + block_size - 1) // block_size
        if directory_size < 12 or directory_size > len(data):
            raise SymbolArchiveError(f"Invalid PDB stream directory size: {pdb}")
        blocks = _unpack(f"<{count}I", data, block_map * block_size)
        directory = bytearray()
        for block in blocks:
            start = block * block_size
            if start + block_size > len(data):
                raise SymbolArchiveError(f"Truncated PDB stream directory: {pdb}")
            directory.extend(data[start:start + block_size])
        streams, = _unpack("<I", directory, 0)
        if streams < 2 or streams > directory_size // 4 - 1:
            raise SymbolArchiveError(f"Invalid PDB stream count: {pdb}")
        stream0, stream1 = _unpack("<II", directory, 4)
        if stream1 < 28 or stream1 == 0xFFFFFFFF:
            raise SymbolArchiveError(f"PDB has no identity stream: {pdb}")
        stream0_blocks = 0 if stream0 == 0xFFFFFFFF else (stream0 + block_size - 1) // block_size
        info_block, = _unpack("<I", directory, 4 + streams * 4 + stream0_blocks * 4)
        offset = info_block * block_size
        _, _, age, guid = _unpack("<III16s", data, offset)
        return uuid.UUID(bytes_le=guid).hex.upper(), age


def archive_windows_symbols(image: Path, root: Path) -> Path:
    identity = _image_identity(image)
    pdb = image.with_suffix(".pdb")
    if _pdb_identity(pdb) != (identity["pdbGuid"], identity["pdbAge"]):
        raise SymbolArchiveError(f"PDB does not match the executable: {pdb}")
    if path_is_link(root):
        raise SymbolArchiveError(f"Symbol archive must not be a link: {root}")
    destination = root / str(identity["pdbName"]) / (str(identity["pdbGuid"]) + format(int(identity["pdbAge"]), "X"))
    metadata = {**identity, "imageName": image.name,
                "imageSha256": sha256(image), "pdbSha256": sha256(pdb)}
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        if path_is_link(destination) or json.loads((destination / "symbols.json").read_text("utf-8")) != metadata:
            raise SymbolArchiveError(f"Existing symbol archive does not match: {destination}")
        return destination
    temporary = Path(tempfile.mkdtemp(prefix=".symbols-", dir=destination.parent))
    try:
        shutil.copy2(image, temporary / image.name)
        shutil.copy2(pdb, temporary / str(identity["pdbName"]))
        (temporary / "symbols.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
        temporary.rename(destination)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    return destination
