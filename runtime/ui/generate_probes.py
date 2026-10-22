#!/usr/bin/env python3
# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.
"""Generate the retained Blink instrumentation probes for the local UI profile."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

BLINK = "third_party/blink/renderer"
CORE = f"{BLINK}/core"
SCRIPTS = f"{BLINK}/build/scripts"
PROBE_CONFIG = f"{CORE}/probe/core_probes.json5"
PROBE_PIDL = f"{CORE}/probe/core_probes.pidl"
OVERLAY_FILE = "mini_ui_probe_sources.json"
STATE_FILE = "mini_ui_probe_generator_state.json"
GENERATED_FILES = (
    f"{CORE}/core_probe_sink.h",
    f"{CORE}/core_probes_inl.h",
    f"{CORE}/core_probes_impl.cc",
)

# These mappings are deliberately checked against Blink's real configuration.
# A schema change must be reviewed rather than silently broadening the profile.
RETAINED_OBSERVERS = {
    "AnimationFrameTimingMonitor": frozenset({
        "DidFinishSyncXHR",
        "DidRunJavaScriptDialog",
        "EvaluateScriptBlock",
        "ExecuteScript",
        "InvokeCallback",
        "InvokeEventHandler",
        "RecalculateStyle",
        "UpdateLayout",
        "WillHandlePromise",
    }),
    "LCPScriptObserver": frozenset({
        "CallFunction",
        "ExecuteScript",
    }),
    "PerformanceMonitor": frozenset({
        "CallFunction",
        "DocumentWriteFetchScript",
        "ExecuteScript",
        "RecalculateStyle",
        "UpdateLayout",
        "UserCallback",
        "V8Compile",
    }),
}
RETAINED_PROBES = frozenset().union(*RETAINED_OBSERVERS.values())
REQUIRED_SETTINGS_INCLUDES = (
    f"{CORE}/core_probe_sink.h",
    f"{CORE}/probe/core_probes.h",
)
TEMPLATES = (
    "instrumenting_probes_impl.cc.tmpl",
    "instrumenting_probes_inl.h.tmpl",
    "probe_sink.h.tmpl",
    "macros.tmpl",
)


def json_bytes(value):
    return (json.dumps(value, indent=2, ensure_ascii=False,
                       sort_keys=True) + "\n").encode("utf-8")


def write_changed(path: Path, content: bytes):
    if path.is_file() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def generator_fingerprint(root: Path, dependency_versions):
    inputs = {
        Path(__file__).resolve(),
        root / PROBE_CONFIG,
        root / PROBE_PIDL,
        root / SCRIPTS / "make_instrumenting_probes.py",
        root / SCRIPTS / "blinkbuild/name_style_converter.py",
        *(root / SCRIPTS / "templates" / name for name in TEMPLATES),
    }
    digest = hashlib.sha256()
    for path in sorted(inputs):
        if not path.is_file():
            raise RuntimeError(f"Missing probe generator input: {path}")
        label = path.relative_to(root).as_posix()
        digest.update(label.encode("utf-8") + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    digest.update(json_bytes({"dependencies": dependency_versions}))
    return digest.hexdigest()


def expected_owned_files():
    return frozenset((*GENERATED_FILES, OVERLAY_FILE))


def cached_outputs(out: Path, fingerprint: str):
    state_path = out / STATE_FILE
    if not state_path.is_file():
        return None
    state = json.loads(state_path.read_text(encoding="utf-8"))
    files = state.get("files")
    if (state.get("version") != 1 or state.get("inputs") != fingerprint
            or not isinstance(files, dict)
            or set(files) != expected_owned_files()):
        return None
    for name, expected in files.items():
        path = (out / name).resolve()
        if not path.is_relative_to(out) or not path.is_file():
            return None
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            return None
    return state


def checked_config(root: Path, probe_writer):
    original = probe_writer.load_config(str(root / PROBE_CONFIG))
    observers = original.get("observers")
    settings = original.get("settings")
    if not isinstance(observers, dict) or not isinstance(settings, dict):
        raise RuntimeError(f"Invalid probe configuration in {PROBE_CONFIG}")

    missing_observers = sorted(set(RETAINED_OBSERVERS) - set(observers))
    if missing_observers:
        raise RuntimeError(
            f"Blink probe configuration lost retained observers: {missing_observers}")
    for name, expected in RETAINED_OBSERVERS.items():
        entry = observers[name]
        actual = frozenset(entry.get("probes", ()))
        if actual != expected:
            raise RuntimeError(
                f"Blink observer {name} probe binding changed: "
                f"expected={sorted(expected)}, actual={sorted(actual)}")
        include_path = entry.get("include_path", settings.get("include_path"))
        if not isinstance(include_path, str) or not include_path:
            raise RuntimeError(f"Blink observer {name} has no real include path")

    original_includes = settings.get("includes")
    if not isinstance(original_includes, list):
        raise RuntimeError("Blink probe settings do not contain an include list")
    missing_includes = sorted(set(REQUIRED_SETTINGS_INCLUDES) -
                              set(original_includes))
    if missing_includes:
        raise RuntimeError(
            f"Blink probe configuration lost required includes: {missing_includes}")

    config = copy.deepcopy(original)
    config["observers"] = {
        name: copy.deepcopy(observers[name]) for name in RETAINED_OBSERVERS
    }
    # core_probes.h supplies the retained parameter types. Keeping unrelated
    # PIDL includes here would restore media, speculation and inspector types.
    config["settings"]["includes"] = list(REQUIRED_SETTINGS_INCLUDES)
    return config, frozenset(observers)


def retained_model(root: Path, probe_writer, config):
    source = (root / PROBE_PIDL).read_text(encoding="utf-8")
    files = probe_writer.load_model_from_idl(source)
    if len(files) != 1 or files[0].name != "core_probes":
        raise RuntimeError(
            f"Expected one CoreProbes interface in {PROBE_PIDL}, got "
            f"{[item.name for item in files]}")

    model = files[0]
    all_names = [probe.name for probe in model.declarations]
    counts = {
        name: all_names.count(name) for name in RETAINED_PROBES
    }
    invalid_counts = {
        name: count for name, count in counts.items() if count != 1
    }
    if invalid_counts:
        raise RuntimeError(
            "Blink PIDL must declare each retained probe exactly once: "
            f"{dict(sorted(invalid_counts.items()))}")

    model.declarations = [
        probe for probe in model.declarations if probe.name in RETAINED_PROBES
    ]
    retained_names = frozenset(probe.name for probe in model.declarations)
    if retained_names != RETAINED_PROBES:
        raise RuntimeError(
            "Filtered PIDL probe set differs from the declared UI profile: "
            f"{sorted(retained_names ^ RETAINED_PROBES)}")

    parameter_text = " ".join(
        value
        for probe in model.declarations
        for parameter in probe.params
        for value in (parameter.type, parameter.default_value)
        if value is not None
    )
    needed_declarations = []
    for declaration in model.forward_declarations:
        match = re.match(r"(?:class|struct|using)\s+(\w+)", declaration)
        if match and re.search(rf"\b{re.escape(match.group(1))}\b",
                               parameter_text):
            needed_declarations.append(declaration)
    model.forward_declarations = needed_declarations

    agents = probe_writer.build_observers(config, files)
    if agents != set(RETAINED_OBSERVERS):
        raise RuntimeError(
            "Filtered observer set differs from the declared UI profile: "
            f"{sorted(agents ^ set(RETAINED_OBSERVERS))}")
    for probe in model.declarations:
        expected = {
            observer for observer, probes in RETAINED_OBSERVERS.items()
            if probe.name in probes
        }
        if probe.agents != expected:
            raise RuntimeError(
                f"Filtered dispatch binding for {probe.name} is invalid: "
                f"expected={sorted(expected)}, actual={sorted(probe.agents)}")
    return files, agents


def render_outputs(root: Path, stage: Path, cache: Path, probe_writer):
    config, all_observers = checked_config(root, probe_writer)
    files, agents = retained_model(root, probe_writer, config)
    cache.mkdir(parents=True, exist_ok=True)
    environment = probe_writer.initialize_jinja_env(config, str(cache))
    base_name = "core_probes"
    output_prefix = f"{CORE}/"
    converter = probe_writer.NameStyleConverter
    context = {
        "files": files,
        "agents": agents,
        "config": config,
        "method_name": lambda name: converter(name).to_function_name(),
        "name": converter(base_name).to_upper_camel_case(),
        "header": base_name,
        "input_files": [Path(PROBE_PIDL).name],
        "output_path_in_gen_dir": output_prefix,
    }

    outputs = {}
    context["template_file"] = "/instrumenting_probes_impl.cc.tmpl"
    outputs[f"{CORE}/{base_name}_impl.cc"] = environment.get_template(
        context["template_file"]).render(context).encode("utf-8")

    sink_name = probe_writer.to_singular(base_name) + "_sink.h"
    context["template_file"] = "/probe_sink.h.tmpl"
    context["header_guard"] = converter(
        output_prefix + sink_name).to_header_guard()
    outputs[f"{CORE}/{sink_name}"] = environment.get_template(
        context["template_file"]).render(context).encode("utf-8")

    for file_model in files:
        context["file"] = file_model
        context["template_file"] = "/instrumenting_probes_inl.h.tmpl"
        context["header_guard"] = converter(
            output_prefix + file_model.header_name).to_header_guard()
        outputs[f"{CORE}/{file_model.header_name}"] = environment.get_template(
            context["template_file"]).render(context).encode("utf-8")

    if set(outputs) != set(GENERATED_FILES):
        raise RuntimeError(
            f"Unexpected generated probe files: {sorted(set(outputs) ^ set(GENERATED_FILES))}")
    for relative, content in outputs.items():
        write_changed(stage / relative, content)
    observer_includes = {
        observer: probe_writer.agent_name_to_include(config, observer)
        for observer in RETAINED_OBSERVERS
    }
    validate_generated(outputs, all_observers, observer_includes)


def validate_generated(outputs, all_observers, observer_includes):
    decoded = {name: content.decode("utf-8")
               for name, content in outputs.items()}
    combined = "\n".join(decoded.values())
    removed_observers = all_observers - set(RETAINED_OBSERVERS)
    forbidden = sorted(removed_observers | {
        "AdScriptIdentifier",
        "CachedData",
        "V8Inspector",
        "v8_inspector",
        "WebMediaInspector",
    })
    for token in forbidden:
        if token in combined:
            raise RuntimeError(
                f"Filtered probe output still references removed symbol {token!r}")

    sink = decoded[f"{CORE}/core_probe_sink.h"]
    implementation = decoded[f"{CORE}/core_probes_impl.cc"]
    inline = decoded[f"{CORE}/core_probes_inl.h"]
    for observer in RETAINED_OBSERVERS:
        include = f'#include "{observer_includes[observer]}"'
        if include not in implementation:
            raise RuntimeError(
                f"Generated probe implementation lost observer include {include}")
        for fragment in (f"k{observer}", f"Add{observer}(",
                         f"Remove{observer}(", f"Has{observer}s()"):
            if fragment not in sink:
                raise RuntimeError(
                    f"Generated probe sink lost retained API {fragment!r}")
        if f"{observer}s().ForEachAgent" not in implementation:
            raise RuntimeError(
                f"Generated probe implementation does not dispatch to {observer}")

    for probe in RETAINED_PROBES:
        if (f" {probe}Impl(" not in inline
                and f"class CORE_EXPORT {probe} " not in inline):
            raise RuntimeError(
                f"Generated inline probe declarations lost {probe}")
        if (f"void {probe}Impl(" not in implementation
                and f"{probe}::{probe}(" not in implementation):
            raise RuntimeError(
                f"Generated probe implementation lost {probe}")




def make_overlay(root: Path, out: Path):
    out_relative = out.relative_to(root).as_posix()
    old_implementation = f"gen/{CORE}/core_probes_impl.cc"
    new_implementation = f"{out_relative}/{CORE}/core_probes_impl.cc"
    return {
        "version": 1,
        "add": [],
        "exclude": [],
        "replace": {old_implementation: new_implementation},
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
            f"Probe generator staged unexpected files: "
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
                    f"Refusing to delete modified stale probe output: {path}")
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
        import make_instrumenting_probes as probe_writer
        import jinja2
    except ModuleNotFoundError as error:
        parser.error(
            f"Missing probe-generation dependency {error.name!r}; restore "
            "Chromium's bundled Jinja package")

    dependency_versions = {
        "jinja2": getattr(jinja2, "__version__", "unknown"),
    }
    fingerprint = generator_fingerprint(root, dependency_versions)
    if cached_outputs(out, fingerprint) is not None:
        print(f"UI probes up to date: {(out / OVERLAY_FILE).relative_to(root)}")
        return 0

    with tempfile.TemporaryDirectory(prefix=".ui-probes-", dir=out) as scratch:
        work = Path(scratch)
        stage = work / "publish"
        render_outputs(root, stage, work / "jinja-cache", probe_writer)
        write_changed(stage / OVERLAY_FILE,
                      json_bytes(make_overlay(root, out)))
        publish(out, stage, fingerprint)

    print(f"Generated {len(RETAINED_PROBES)} UI probes for "
          f"{len(RETAINED_OBSERVERS)} native observers")
    return 0


if __name__ == "__main__":
    # Sets and template loops must remain stable on every host.
    if os.environ.get("PYTHONHASHSEED") != "0":
        environment = dict(os.environ, PYTHONHASHSEED="0")
        raise SystemExit(subprocess.call([
            sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]
        ], env=environment))
    raise SystemExit(main())
