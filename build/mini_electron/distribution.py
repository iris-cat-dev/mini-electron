# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

import hashlib
import html
import json
import os
import re
import stat
from pathlib import Path
import shutil
import tempfile
import zipfile


class DistributionError(RuntimeError):
    pass

_RESERVED_WINDOWS_NAME = re.compile(
    r"^(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?$", re.IGNORECASE
)
_CHROMIUM_LICENSE = """// Copyright 2015 The Chromium Authors
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * Neither the name of Google LLC nor the names of its contributors may be
//   used to endorse or promote products derived from this software without
//   specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
"""



def _safe_relative_name(relative: str) -> tuple[str, ...]:
    if (
        not relative
        or relative.startswith("/")
        or relative.startswith("//")
        or "\\" in relative
        or "\0" in relative
    ):
        raise DistributionError(f"Unsafe runtime distribution path: {relative!r}")
    parts = tuple(relative.split("/"))
    if any(
        not part
        or part in (".", "..")
        or ":" in part
        or part.endswith((".", " "))
        or _RESERVED_WINDOWS_NAME.match(part)
        for part in parts
    ):
        raise DistributionError(f"Unsafe runtime distribution path: {relative!r}")
    return parts


def _resolved_link_target(relative: str, target: str) -> str:
    if (
        not target
        or target.startswith("/")
        or target.startswith("//")
        or "\\" in target
        or "\0" in target
        or (len(target) >= 2 and target[0].isalpha() and target[1] == ":")
    ):
        raise DistributionError(
            f"Unsafe runtime symlink target for {relative}: {target!r}"
        )
    resolved = list(_safe_relative_name(relative)[:-1])
    for part in target.split("/"):
        if not part or part == "." or ":" in part:
            raise DistributionError(
                f"Unsafe runtime symlink target for {relative}: {target!r}"
            )
        if part == "..":
            if not resolved:
                raise DistributionError(
                    f"Runtime symlink target escapes the distribution: {relative} -> {target}"
                )
            resolved.pop()
        else:
            resolved.append(part)
    if not resolved:
        raise DistributionError(
            f"Unsafe runtime symlink target for {relative}: {target!r}"
        )
    return "/".join(resolved)


def _distribution_entries(
    directory: Path,
) -> list[tuple[Path, str, str, os.stat_result]]:
    entries: list[tuple[Path, str, str, os.stat_result]] = []
    folded_names: set[str] = set()
    for current, directory_names, file_names in os.walk(
        directory, topdown=True, followlinks=False
    ):
        current_path = Path(current)
        traversable: list[str] = []
        for name in sorted(directory_names):
            path = current_path / name
            relative = path.relative_to(directory).as_posix()
            _safe_relative_name(relative)
            status = path.lstat()
            if stat.S_ISLNK(status.st_mode):
                kind = "link"
            elif stat.S_ISDIR(status.st_mode):
                kind = "directory"
                traversable.append(name)
            else:
                raise DistributionError(
                    f"Runtime distribution contains a special file: {path}"
                )
            folded = relative.casefold()
            if folded in folded_names:
                raise DistributionError(
                    f"Runtime distribution contains duplicate paths: {relative}"
                )
            folded_names.add(folded)
            entries.append((path, relative, kind, status))
        directory_names[:] = traversable
        for name in sorted(file_names):
            path = current_path / name
            relative = path.relative_to(directory).as_posix()
            _safe_relative_name(relative)
            status = path.lstat()
            if stat.S_ISLNK(status.st_mode):
                kind = "link"
            elif stat.S_ISREG(status.st_mode):
                kind = "file"
            else:
                raise DistributionError(
                    f"Runtime distribution contains a special file: {path}"
                )
            folded = relative.casefold()
            if folded in folded_names:
                raise DistributionError(
                    f"Runtime distribution contains duplicate paths: {relative}"
                )
            folded_names.add(folded)
            entries.append((path, relative, kind, status))
    return sorted(entries, key=lambda entry: entry[1])


def _validated_links(
    directory: Path, entries: list[tuple[Path, str, str, os.stat_result]]
) -> dict[str, str]:
    root = directory.resolve(strict=True)
    links: dict[str, str] = {}
    for path, relative, kind, _ in entries:
        if kind != "link":
            continue
        target = os.readlink(path)
        _resolved_link_target(relative, target)
        try:
            resolved = path.resolve(strict=True)
            resolved.relative_to(root)
        except (OSError, RuntimeError, ValueError) as error:
            raise DistributionError(
                f"Runtime symlink is dangling, cyclic, or escapes the distribution: "
                f"{relative} -> {target}"
            ) from error
        links[relative] = target
    return links


def _read_json(path: Path, label: str) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise DistributionError(f"Invalid {label} {path}: {error}") from error
    if not isinstance(value, dict):
        raise DistributionError(f"{label} must be a JSON object: {path}")
    return value


def release_metadata(root: Path, platform_name: str, arch: str) -> dict[str, object]:
    package = _read_json(root / "package.json", "npm package manifest")
    artifacts = _read_json(root / "artifacts.json", "runtime artifact manifest")
    package_version = package.get("version")
    if package_version != artifacts.get("packageVersion"):
        raise DistributionError("package.json and artifacts.json package versions do not match")
    key = f"{platform_name}-{arch}"
    platforms = artifacts.get("platforms")
    if not isinstance(platforms, dict) or key not in platforms:
        raise DistributionError(f"Runtime artifact metadata does not define {key}")
    target = platforms[key]
    if not isinstance(target, dict):
        raise DistributionError(f"Runtime artifact metadata for {key} is invalid")
    executable = target.get("executable")
    archive = target.get("archive")
    if not isinstance(executable, str) or not executable or not isinstance(archive, str) or not archive:
        raise DistributionError(f"Runtime artifact paths for {key} are invalid")
    required = (
        "runtimeVersion",
        "electronApiVersion",
        "nodeVersion",
        "nodeModuleAbi",
        "chromiumMajorVersion",
    )
    missing = [name for name in required if name not in artifacts]
    if missing:
        raise DistributionError("Runtime artifact metadata is missing: " + ", ".join(missing))
    return {
        "packageVersion": package_version,
        **{name: artifacts[name] for name in required},
        "platform": platform_name,
        "arch": arch,
        "executable": executable,
        "archive": archive,
    }


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _metadata_fields(block: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    current: str | None = None
    for line in block.splitlines():
        match = re.match(r"^([A-Za-z][A-Za-z ]+):\s*(.*)$", line)
        if match:
            current = match.group(1)
            value = match.group(2).strip()
            fields[current] = (
                f"{fields[current]}, {value}" if current in fields else value
            )
        elif current is not None and line[:1].isspace() and line.strip():
            fields[current] = f"{fields[current]} {line.strip()}"
    return fields


def _generate_chromium_notices(root: Path, destination: Path) -> None:
    entries: list[tuple[str, str, str, str]] = [
        (
            "The Chromium Project",
            "https://www.chromium.org/",
            "BSD-3-Clause",
            _CHROMIUM_LICENSE,
        )
    ]
    ignored_roots = {".build-tools", ".git", "gen", "out"}
    for metadata_path in sorted(root.rglob("README.chromium")):
        relative_metadata = metadata_path.relative_to(root)
        if relative_metadata.parts[0] in ignored_roots:
            continue
        text = metadata_path.read_text(encoding="utf-8")
        for block in re.split(r"\n\s*\n", text):
            fields = _metadata_fields(block)
            license_value = fields.get("License File", "")
            if (
                not fields.get("Name")
                or not license_value
                or license_value == "NOT_SHIPPED"
                or fields.get("Shipped", "yes").strip().lower() == "no"
            ):
                continue
            license_texts: list[str] = []
            for value in (item.strip() for item in license_value.split(",")):
                if not value:
                    continue
                license_path = (
                    root / value.removeprefix("//")
                    if value.startswith("//")
                    else metadata_path.parent / value
                )
                if not license_path.is_file():
                    raise DistributionError(
                        f"Chromium license metadata references a missing file: "
                        f"{metadata_path}: {value}"
                    )
                license_texts.append(license_path.read_text(encoding="utf-8"))
            if license_texts:
                entries.append(
                    (
                        fields["Name"],
                        fields.get("URL", ""),
                        fields.get("License", ""),
                        "\n\n".join(license_texts),
                    )
                )
    if len(entries) < 2:
        raise DistributionError(
            "Chromium third-party license metadata produced no shipped notices"
        )

    supplemental = (
        (
            "Chrome DevTools frontend",
            "https://chromium.googlesource.com/devtools/devtools-frontend/",
            "BSD-3-Clause",
            root / "third_party" / "devtools-frontend" / "LICENSE",
        ),
        (
            "SQLite",
            "https://www.sqlite.org/",
            "Public Domain",
            root / "third_party" / "sqlite" / "LICENSE",
        ),
        (
            "LevelDB",
            "https://github.com/google/leveldb/",
            "BSD-3-Clause",
            root / "third_party" / "leveldb" / "LICENSE",
        ),
    )
    for name, url, license_name, license_path in supplemental:
        if not license_path.is_file():
            raise DistributionError(
                f"Required third-party license is missing: {license_path}"
            )
        entries.append(
            (
                name,
                url,
                license_name,
                license_path.read_text(encoding="utf-8"),
            )
        )

    sections = []
    for name, url, license_name, license_text in entries:
        title = html.escape(name)
        if url:
            title = f'<a href="{html.escape(url, quote=True)}">{title}</a>'
        sections.append(
            f"<section><h2>{title}</h2>"
            f"<p>{html.escape(license_name)}</p>"
            f"<pre>{html.escape(license_text)}</pre></section>"
        )
    document = (
        "<!doctype html>\n<meta charset=\"utf-8\">\n"
        "<title>Open-source licenses</title>\n"
        "<h1>Chromium and third-party software notices</h1>\n"
        + "\n".join(sections)
        + "\n"
    )
    destination.write_text(document, encoding="utf-8")


def copy_release_notices(root: Path, destination: Path) -> None:
    license_path = root / "LICENSE"
    leveldb_license = root / "third_party" / "leveldb" / "LICENSE"
    if not license_path.is_file():
        raise DistributionError(f"Project license is missing: {license_path}")
    if not leveldb_license.is_file():
        raise DistributionError(f"LevelDB license is missing: {leveldb_license}")
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copy2(license_path, destination / "LICENSE")
    shutil.copy2(leveldb_license, destination / "LICENSES.leveldb")
    _generate_chromium_notices(root, destination / "LICENSES.chromium.html")


def copy_runtime_javascript(root: Path, resources: Path) -> None:
    source = root / "runtime" / "electron" / "lib"
    required = (
        source / "browser" / "init.js",
        source / "browser" / "electron.js",
        source / "common" / "init.js",
    )
    if not source.is_dir() or any(not entry.is_file() for entry in required):
        raise DistributionError(f"Electron runtime JavaScript is incomplete: {source}")
    target = resources / "mini-electron" / "lib"
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, target, symlinks=False)


def copy_devtools_frontend(root: Path, resources: Path) -> None:
    devtools_source = (
        root / "third_party" / "devtools-frontend" / "generated" / "front_end"
    )
    required_devtools_entries = (
        devtools_source / "devtools_app.html",
        devtools_source / "entrypoints" / "devtools_app" / "devtools_app.js",
        devtools_source / "entrypoints" / "inspector_main" / "inspector_main.js",
    )
    if not devtools_source.is_dir() or any(
        not entry.is_file() for entry in required_devtools_entries
    ):
        raise DistributionError(
            "Pinned generated DevTools frontend assets are missing: "
            f"{devtools_source}"
        )
    devtools_target = resources / "devtools-frontend" / "front_end"
    devtools_target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(devtools_source, devtools_target, symlinks=False)


def write_version_metadata(directory: Path, metadata: dict[str, object]) -> None:
    executable = metadata["executable"]
    if not isinstance(executable, str):
        raise DistributionError("Runtime executable metadata is invalid")
    executable_parts = _safe_relative_name(executable)
    executable_path = directory.joinpath(*executable_parts)
    if (
        executable_path.is_symlink()
        or not executable_path.is_file()
        or executable_path.stat().st_size == 0
    ):
        raise DistributionError(f"Runtime executable is missing or empty: {executable_path}")
    version_path = directory / "version.json"
    if version_path.is_symlink() or (version_path.exists() and not version_path.is_file()):
        raise DistributionError(f"Runtime version metadata path is unsafe: {version_path}")

    entries = _distribution_entries(directory)
    links = _validated_links(directory, entries)
    document = {
        name: metadata[name]
        for name in (
            "packageVersion",
            "runtimeVersion",
            "electronApiVersion",
            "nodeVersion",
            "nodeModuleAbi",
            "chromiumMajorVersion",
            "platform",
            "arch",
        )
    }
    document["files"] = {
        relative: sha256(path)
        for path, relative, kind, _ in entries
        if kind == "file" and relative != "version.json"
    }
    document["links"] = links
    version_path.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def validate_distribution(
    directory: Path, expected_metadata: dict[str, object] | None = None
) -> dict[str, object]:
    version_path = directory / "version.json"
    if version_path.is_symlink() or not version_path.is_file():
        raise DistributionError(
            f"Runtime version metadata is missing or unsafe: {version_path}"
        )
    document = _read_json(version_path, "runtime version metadata")
    identity_fields = (
        "packageVersion",
        "runtimeVersion",
        "electronApiVersion",
        "nodeVersion",
        "nodeModuleAbi",
        "chromiumMajorVersion",
        "platform",
        "arch",
    )
    if expected_metadata is not None:
        for name in identity_fields:
            if document.get(name) != expected_metadata.get(name):
                raise DistributionError(
                    f"{directory} metadata {name} does not match the release: "
                    f"{document.get(name)!r} != {expected_metadata.get(name)!r}"
                )

    declared_files = document.get("files")
    declared_links = document.get("links")
    if not isinstance(declared_files, dict) or not isinstance(declared_links, dict):
        raise DistributionError(
            f"Runtime metadata has no exact file/link manifest: {version_path}"
        )

    folded_declared: dict[str, tuple[str, str]] = {}
    for kind, records in (("file", declared_files), ("symlink", declared_links)):
        for relative, value in records.items():
            if not isinstance(relative, str):
                raise DistributionError(
                    f"Runtime metadata contains an invalid {kind} path"
                )
            _safe_relative_name(relative)
            folded = relative.casefold()
            if folded in folded_declared:
                raise DistributionError(
                    f"Runtime metadata contains duplicate paths: {relative}"
                )
            folded_declared[folded] = (relative, kind)
            if kind == "file":
                if not isinstance(value, str) or not re.fullmatch(
                    r"[a-f0-9]{64}", value
                ):
                    raise DistributionError(
                        f"Runtime metadata contains an invalid file hash: {relative}"
                    )
            elif not isinstance(value, str):
                raise DistributionError(
                    f"Runtime metadata contains an invalid symlink target: {relative}"
                )
            else:
                _resolved_link_target(relative, value)

    entries = _distribution_entries(directory)
    actual_files = {
        relative: path
        for path, relative, kind, _ in entries
        if kind == "file" and relative != "version.json"
    }
    actual_links = _validated_links(directory, entries)
    if set(actual_files) != set(declared_files):
        raise DistributionError(
            "Runtime distribution files do not match version metadata"
        )
    if actual_links != declared_links:
        raise DistributionError(
            "Runtime distribution symlinks do not match version metadata"
        )
    if document.get("platform") != "darwin" and actual_links:
        raise DistributionError(
            f"Runtime symlinks are not allowed for {document.get('platform')}"
        )

    for relative, path in actual_files.items():
        if sha256(path) != declared_files[relative]:
            raise DistributionError(
                f"Runtime file SHA256 does not match metadata: {path}"
            )

    if expected_metadata is not None:
        executable_name = expected_metadata.get("executable")
        if not isinstance(executable_name, str):
            raise DistributionError("Runtime executable metadata is invalid")
        executable = directory.joinpath(*_safe_relative_name(executable_name))
        if (
            executable.is_symlink()
            or not executable.is_file()
            or executable.stat().st_size == 0
            or executable_name not in declared_files
        ):
            raise DistributionError(
                f"Runtime executable is missing, unsafe, or unlisted: {executable}"
            )
        if document.get("platform") == "darwin" and (
            executable.stat().st_mode & 0o111
        ) == 0:
            raise DistributionError(f"Runtime executable is not executable: {executable}")
    return document


def _zip_timestamp() -> tuple[int, int, int, int, int, int]:
    # Fixed timestamps make identical release inputs produce identical archives.
    return (2026, 1, 1, 0, 0, 0)


def create_zip(source: Path, destination: Path) -> None:
    metadata = validate_distribution(source)
    entries = _distribution_entries(source)
    expected_hashes = dict(metadata["files"])
    expected_hashes["version.json"] = sha256(source / "version.json")
    declared_links = metadata["links"]

    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{destination.name}.", suffix=".tmp", dir=destination.parent
    )
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        with zipfile.ZipFile(
            temporary, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
        ) as archive:
            for path, relative, kind, status in entries:
                info = zipfile.ZipInfo(
                    relative + ("/" if kind == "directory" else ""),
                    _zip_timestamp(),
                )
                info.create_system = 3
                if kind == "directory":
                    info.external_attr = ((status.st_mode & 0o777) | 0o040000) << 16
                    archive.writestr(info, b"")
                elif kind == "link":
                    info.external_attr = ((status.st_mode & 0o777) | 0o120000) << 16
                    target = os.readlink(path)
                    if declared_links.get(relative) != target:
                        raise DistributionError(
                            f"Runtime symlink changed while archiving: {relative}"
                        )
                    archive.writestr(info, target.encode("utf-8"))
                else:
                    info.compress_type = zipfile.ZIP_DEFLATED
                    info._compresslevel = 9
                    info.external_attr = ((status.st_mode & 0o777) | 0o100000) << 16
                    digest = hashlib.sha256()
                    with path.open("rb") as input_file, archive.open(info, "w") as output_file:
                        for chunk in iter(lambda: input_file.read(1024 * 1024), b""):
                            digest.update(chunk)
                            output_file.write(chunk)
                    if digest.hexdigest() != expected_hashes.get(relative):
                        raise DistributionError(
                            f"Runtime file changed while archiving: {relative}"
                        )
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def publish_distribution(
    staged: Path, destination: Path, metadata: dict[str, object]
) -> tuple[Path, Path, str]:
    if destination.is_symlink() or destination.is_junction():
        raise DistributionError(
            f"Runtime distribution output must not be a symlink or junction: "
            f"{destination}"
        )
    destination = destination.resolve()
    if destination.exists() and not destination.is_dir():
        raise DistributionError(
            f"Runtime distribution output is not a directory: {destination}"
        )
    destination.parent.mkdir(parents=True, exist_ok=True)
    write_version_metadata(staged, metadata)
    archive_name = metadata["archive"]
    if not isinstance(archive_name, str):
        raise DistributionError("Runtime archive metadata is invalid")
    archive = destination.parent / archive_name
    create_zip(staged, archive)
    archive_hash = sha256(archive)
    sidecar = archive.with_name(archive.name + ".sha256")
    sidecar_descriptor, sidecar_temporary_name = tempfile.mkstemp(
        prefix=f".{sidecar.name}.", suffix=".tmp", dir=sidecar.parent
    )
    os.close(sidecar_descriptor)
    sidecar_temporary = Path(sidecar_temporary_name)
    try:
        sidecar_temporary.write_text(
            f"{archive_hash}  {archive.name}\n", encoding="ascii"
        )
        os.replace(sidecar_temporary, sidecar)
    finally:
        sidecar_temporary.unlink(missing_ok=True)


    backup_holder: Path | None = None
    backup: Path | None = None
    if destination.exists():
        backup_holder = Path(
            tempfile.mkdtemp(
                prefix=f".{destination.name}.replace-", dir=destination.parent
            )
        )
        backup = backup_holder / "previous"
        destination.rename(backup)
    try:
        staged.rename(destination)
    except BaseException:
        if backup is not None and backup.exists() and not destination.exists():
            backup.rename(destination)
        raise
    finally:
        if backup_holder is not None and backup_holder.exists():
            shutil.rmtree(backup_holder)
    return destination, archive, archive_hash


def staging_directory(destination: Path) -> Path:
    destination.parent.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix=f".{destination.name}.stage-", dir=destination.parent))
