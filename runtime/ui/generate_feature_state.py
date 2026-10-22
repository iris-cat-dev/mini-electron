#!/usr/bin/env python3
# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.
"""Generate Blink's runtime-feature override context for the local UI profile."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

BLINK = "third_party/blink/renderer"
SCRIPTS = f"{BLINK}/build/scripts"
RUNTIME_FEATURES = f"{BLINK}/platform/runtime_enabled_features.json5"
OUTPUT_DIR = f"{BLINK}/platform/runtime_feature_state"
BASENAME = "runtime_feature_state_override_context"
OVERLAY_FILE = "mini_ui_feature_state_sources.json"
STATE_FILE = "mini_ui_feature_state_generator_state.json"
GENERATED_FILES = (
    f"{OUTPUT_DIR}/{BASENAME}.h",
    f"{OUTPUT_DIR}/{BASENAME}.cc",
)
WRITERS = (
    "make_runtime_feature_state_override_context.py",
    "make_runtime_feature_state_contexts_headers.py",
    "make_runtime_feature_state_contexts_impl.py",
    "make_runtime_features.py",
)
SUPPORT_FILES = (
    "json5_generator.py",
    "make_runtime_features_utilities.py",
    "template_expander.py",
    "blinkbuild/name_style_converter.py",
)
TEMPLATES = (
    "runtime_feature_state_override_context.h.tmpl",
    "runtime_feature_state_override_context.cc.tmpl",
    "runtime_feature_state_context.h.tmpl",
    "runtime_feature_state_context.cc.tmpl",
    "runtime_feature_state_read_context.h.tmpl",
    "runtime_enabled_features.h.tmpl",
    "runtime_enabled_features.cc.tmpl",
)


def json_bytes(value):
    return (json.dumps(value, indent=2, ensure_ascii=False,
                       sort_keys=True) + "\n").encode("utf-8")


def write_changed(path: Path, content: bytes):
    if path.is_file() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def expected_owned_files():
    return frozenset((*GENERATED_FILES, OVERLAY_FILE))


def generator_fingerprint(root: Path, dependency_versions):
    inputs = {
        Path(__file__).resolve(),
        root / RUNTIME_FEATURES,
        *(root / SCRIPTS / name for name in WRITERS),
        *(root / SCRIPTS / name for name in SUPPORT_FILES),
        *(root / SCRIPTS / "templates" / name for name in TEMPLATES),
    }
    digest = hashlib.sha256()
    for path in sorted(inputs):
        if not path.is_file():
            raise RuntimeError(f"Missing runtime-feature generator input: {path}")
        label = (path.relative_to(root).as_posix()
                 if path.is_relative_to(root) else path.name)
        digest.update(label.encode("utf-8") + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    digest.update(json_bytes({"dependencies": dependency_versions}))
    return digest.hexdigest()


def cached_outputs(out: Path, fingerprint: str):
    state_path = out / STATE_FILE
    if not state_path.is_file():
        return False
    state = json.loads(state_path.read_text(encoding="utf-8"))
    files = state.get("files")
    if (state.get("version") != 1 or state.get("inputs") != fingerprint
            or not isinstance(files, dict)
            or set(files) != expected_owned_files()):
        return False
    for name, expected_hash in files.items():
        path = (out / name).resolve()
        if (not path.is_relative_to(out) or not path.is_file()
                or hashlib.sha256(path.read_bytes()).hexdigest() != expected_hash):
            return False
    return True


def render_outputs(root: Path, stage: Path, writer_module):
    output_dir = stage / OUTPUT_DIR
    writer = writer_module.RunTimeFeatureStateOverrideContextWriter(
        [str(root / RUNTIME_FEATURES)], str(output_dir))

    # Temporary staging paths must not leak into provenance or include guards.
    writer._input_files = [RUNTIME_FEATURES]
    writer._relative_output_dir = OUTPUT_DIR + "/"
    writer._header_guard = writer.make_header_guard(
        f"{OUTPUT_DIR}/{BASENAME}.h")

    expected_names = {f"{BASENAME}.h", f"{BASENAME}.cc"}
    if set(writer._outputs) != expected_names:
        raise RuntimeError(
            "Override-context writer output set changed: "
            f"{sorted(set(writer._outputs) ^ expected_names)}")
    for filename, generator in writer._outputs.items():
        content = generator()
        if not isinstance(content, str):
            raise RuntimeError(f"Writer returned non-text output for {filename}")
        write_changed(output_dir / filename, content.encode("utf-8"))


def make_overlay(root: Path, out: Path):
    out_relative = out.relative_to(root).as_posix()
    old_source = f"gen/{OUTPUT_DIR}/{BASENAME}.cc"
    new_source = f"{out_relative}/{OUTPUT_DIR}/{BASENAME}.cc"
    return {
        "version": 1,
        "add": [],
        "exclude": [],
        "replace": {old_source: new_source},
        "include_dirs": [out_relative],
        "defines": [],
    }


def publish(out: Path, stage: Path, fingerprint: str):
    files = {
        path.relative_to(stage).as_posix(): path.read_bytes()
        for path in sorted(stage.rglob("*")) if path.is_file()
    }
    if set(files) != expected_owned_files():
        raise RuntimeError(
            "Runtime-feature generator staged unexpected files: "
            f"{sorted(set(files) ^ expected_owned_files())}")

    state_path = out / STATE_FILE
    if state_path.is_file():
        previous = json.loads(state_path.read_text(encoding="utf-8"))
        for name, old_hash in previous.get("files", {}).items():
            path = (out / name).resolve()
            if name in files or not path.is_relative_to(out) or not path.is_file():
                continue
            if hashlib.sha256(path.read_bytes()).hexdigest() != old_hash:
                raise RuntimeError(
                    f"Refusing to delete modified stale feature-state output: {path}")
            path.unlink()

    for name, content in files.items():
        write_changed(out / name, content)
    state = {
        "version": 1,
        "inputs": fingerprint,
        "files": {
            name: hashlib.sha256(content).hexdigest()
            for name, content in files.items()
        },
    }
    write_changed(state_path, json_bytes(state))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    options = parser.parse_args()

    root = options.root.resolve()
    out = options.out.resolve()
    if out == root or not out.is_relative_to(root):
        parser.error("--out must be a child directory inside --root")
    out.mkdir(parents=True, exist_ok=True)

    sys.path.insert(0, str(root / SCRIPTS))
    try:
        import jinja2
        import json5
        import make_runtime_feature_state_override_context as writer_module
    except ModuleNotFoundError as error:
        parser.error(
            f"Missing runtime-feature generation dependency {error.name!r}; "
            "restore Chromium's bundled Python inputs")

    dependency_versions = {
        "jinja2": getattr(jinja2, "__version__", "unknown"),
        "json5": getattr(json5, "__version__", "unknown"),
    }
    fingerprint = generator_fingerprint(root, dependency_versions)
    if cached_outputs(out, fingerprint):
        print(f"UI feature state up to date: "
              f"{(out / OVERLAY_FILE).relative_to(root)}")
        return 0

    with tempfile.TemporaryDirectory(prefix=".ui-feature-state-", dir=out) as scratch:
        stage = Path(scratch) / "publish"
        render_outputs(root, stage, writer_module)
        write_changed(stage / OVERLAY_FILE, json_bytes(make_overlay(root, out)))
        publish(out, stage, fingerprint)

    print("Generated Blink UI runtime-feature override context")
    return 0


if __name__ == "__main__":
    # Keep parser dictionaries and template iteration deterministic on every host.
    if os.environ.get("PYTHONHASHSEED") != "0":
        environment = dict(os.environ, PYTHONHASHSEED="0")
        raise SystemExit(subprocess.call([
            sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]
        ], env=environment))
    raise SystemExit(main())
