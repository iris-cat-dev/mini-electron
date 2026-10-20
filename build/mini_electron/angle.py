# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

import hashlib
from pathlib import Path
import shutil
import tarfile
import tempfile
import urllib.request


# ANGLE revision shipped with the Chromium 132 API used by this engine.
ANGLE_REVISION = "ce13a00a2b049a1ef5e0e70a3d333ce70838ef7b"
ANGLE_SHA256 = "adb47645ecaf467001d6bd31faa5706235468fdef4e3f6569279849fe662178d"


def _configure_angle(root: Path, source: Path) -> None:
    for name, relative in (
        ("angle_spirv.gn", "src/common/spirv/BUILD.gn"),
        ("angle_metal_wrapper.py", "src/libANGLE/renderer/metal/shaders/metal_wrapper.py"),
    ):
        override = root / "build/mini_electron" / name
        destination = source / relative
        if destination.read_bytes() != override.read_bytes():
            shutil.copyfile(override, destination)
    # The embedded libraries do not use ANGLE's separate feature-query utility,
    # whose GN target otherwise resolves an unused JSON dependency.
    build_file = source / "BUILD.gn"
    text = build_file.read_text(encoding="utf-8")
    original = "if (angle_standalone || build_with_chromium || build_angle_end2end_tests_aosp) {"
    configured = "if (angle_build_tests && (angle_standalone || build_with_chromium || build_angle_end2end_tests_aosp)) {"
    if original in text:
        build_file.write_text(text.replace(original, configured, 1), encoding="utf-8")
    elif configured not in text:
        raise RuntimeError("The pinned ANGLE feature utility build definition changed")


def ensure_angle(root: Path) -> Path:
    source = root / "third_party" / "angle"
    marker = source / ".mini-electron-revision"
    if (
        marker.is_file()
        and marker.read_text(encoding="utf-8").strip() == ANGLE_REVISION
        and (source / "include/EGL/egl.h").is_file()
        and (source / "src/libANGLE/Context.cpp").is_file()
    ):
        _configure_angle(root, source)
        return source
    if source.exists() and any(source.iterdir()):
        raise RuntimeError(
            f"ANGLE source at {source} is not the pinned revision {ANGLE_REVISION}; "
            "move that directory aside before rebuilding"
        )

    cache = root / ".build-tools"
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / f"angle-{ANGLE_REVISION}.tar.gz"
    if not archive.is_file():
        url = f"https://codeload.github.com/google/angle/tar.gz/{ANGLE_REVISION}"
        print(f"+ download {url}", flush=True)
        temporary_archive = None
        try:
            with tempfile.NamedTemporaryFile(dir=cache, delete=False) as output:
                temporary_archive = Path(output.name)
                with urllib.request.urlopen(url) as response:
                    shutil.copyfileobj(response, output)
            temporary_archive.replace(archive)
        finally:
            if temporary_archive is not None:
                temporary_archive.unlink(missing_ok=True)
    actual = hashlib.sha256(archive.read_bytes()).hexdigest()
    if actual != ANGLE_SHA256:
        raise RuntimeError(
            f"ANGLE archive checksum mismatch: expected {ANGLE_SHA256}, got {actual}; "
            f"remove {archive} before retrying"
        )
    with tempfile.TemporaryDirectory(dir=source.parent) as temporary:
        directory = Path(temporary)
        with tarfile.open(archive, "r:gz") as package:
            # The checksum pins every archive member. Use the extraction filter
            # where available without raising the project's Python baseline.
            options = {"filter": "data"} if hasattr(tarfile, "data_filter") else {}
            package.extractall(directory, **options)
        unpacked = directory / f"angle-{ANGLE_REVISION}"
        (unpacked / ".mini-electron-revision").write_text(
            ANGLE_REVISION + "\n", encoding="utf-8"
        )
        if source.exists():
            source.rmdir()  # Only an empty directory is accepted above.
        unpacked.replace(source)
    _configure_angle(root, source)
    return source
