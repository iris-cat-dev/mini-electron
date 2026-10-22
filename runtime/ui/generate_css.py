#!/usr/bin/env python3
# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.
"""Generate a coherent Blink CSS database for the local-only UI profile."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

BLINK = "third_party/blink/renderer"
CORE = f"{BLINK}/core"
SCRIPTS = f"{BLINK}/build/scripts"
CSS_PROPERTIES = f"{CORE}/css/css_properties.json5"
FIELD_ALIASES = f"{CORE}/css/computed_style_field_aliases.json5"
RUNTIME_FEATURES = f"{BLINK}/platform/runtime_enabled_features.json5"
EXTRA_FIELDS = f"{CORE}/style/computed_style_extra_fields.json5"
AT_RULE_NAMES = f"{CORE}/css/parser/at_rule_names.json5"
VALUE_KEYWORDS = f"{CORE}/css/css_value_keywords.json5"
PROPERTY_RANKING = f"{CORE}/css/css_properties_ranking.json5"
GROUP_CONFIG = f"{CORE}/css/css_group_config.json5"
PROPERTY_METHODS = f"{CORE}/css/properties/css_property_methods.json5"
SOURCE_CLOSURE = "out/ui-windows-x64/source-closure.json"
STATE_FILE = "mini_ui_css_generator_state.json"
OVERLAY_FILE = "mini_ui_css_sources.json"
PROFILE_FILE = "mini_ui_css_profile.json"

REMOVED_PROPERTIES = frozenset({
    "view-transition-capture-mode",
    "view-transition-class",
    "view-transition-group",
    "view-transition-name",
})
REMOVED_DESCRIPTORS = frozenset({"navigation", "types"})
REMOVED_EXTRA_FIELDS = frozenset({"ElementIsViewTransitionParticipant", "PaintImages"})

# These are sentinels, not an allowlist. They make an accidental broad profile
# reduction fail at generation time.
REQUIRED_ORDINARY_ANIMATION_PROPERTIES = frozenset({
    "animation",
    "animation-name",
    "transition",
    "transition-delay",
    "transition-duration",
    "transition-property",
    "transition-timing-function",
})

FORBIDDEN_GENERATED_TOKENS = (
    "view-transition-capture-mode",
    "view-transition-class",
    "view-transition-group",
    "view-transition-name",
    "ViewTransitionCaptureMode",
    "ViewTransitionClass",
    "ViewTransitionGroup",
    "ViewTransitionName",
    "ElementIsViewTransitionParticipant",
    "element_is_view_transition_participant",
    "PaintImagesInternal",
    "ConvertViewTransitionCaptureMode",
    "ConvertViewTransitionClass",
    "ConvertViewTransitionGroup",
    "ConvertViewTransitionName",
    "StyleViewTransitionCaptureMode",
    "StyleViewTransitionGroup",
    "StyleViewTransitionName",
)


def json_bytes(value):
    return (json.dumps(value, indent=2, ensure_ascii=False,
                       sort_keys=True) + "\n").encode("utf-8")


def write_changed(path: Path, content: bytes):
    if path.is_file() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def digest_file(path: Path):
    return hashlib.sha256(path.read_bytes()).digest()


def resolve_executable(value: str | None, root: Path):
    candidates = []
    if value:
        candidates.append(value)
    if os.environ.get("GPERF"):
        candidates.append(os.environ["GPERF"])
    candidates.extend([
        str(root / "third_party/gperf/bin/gperf.exe"),
        str(root / "third_party/gperf/bin/gperf"),
        "gperf.exe" if os.name == "nt" else "gperf",
    ])
    for candidate in candidates:
        path = Path(candidate)
        if not path.is_absolute():
            rooted = root / path
            if rooted.is_file():
                return rooted.resolve()
        if path.is_file():
            return path.resolve()
        found = shutil.which(candidate)
        if found:
            return Path(found).resolve()
    return None


def load_source_graph(root: Path, selection: Path | None):
    path = selection.resolve() if selection is not None else root / SOURCE_CLOSURE
    if not path.is_file():
        raise RuntimeError(
            f"Missing selected UI source closure: {path}. "
            "Run the UI source selection before CSS generation.")
    with path.open("r", encoding="utf-8") as source:
        document = json.load(source)
    if document.get("version") != 1:
        raise RuntimeError(f"Unsupported source closure version in {path}")
    selected = {
        entry["path"].replace("\\", "/")
        for entry in document.get("sources", [])
        if isinstance(entry, dict) and isinstance(entry.get("path"), str)
    }
    relevant = sorted(
        item for item in selected
        if item.startswith(f"gen/{CORE}/") and item.endswith(".cc"))
    return selected, relevant


def make_filtered_database(root: Path, destination: Path, json5_module):
    source = root / CSS_PROPERTIES
    document = json5_module.loads(source.read_text(encoding="utf-8"))
    entries = document.get("data")
    if not isinstance(entries, list):
        raise RuntimeError(f"Expected a property list in {source}")

    found = set()
    retained = []
    retained_names = set()
    for entry in entries:
        name = entry if isinstance(entry, str) else entry.get("name")
        if name in REMOVED_PROPERTIES or name in REMOVED_DESCRIPTORS:
            if name in REMOVED_DESCRIPTORS and (
                    not isinstance(entry, dict)
                    or entry.get("is_descriptor") is not True
                    or entry.get("is_property") is not False
                    or entry.get("runtime_flag") != "ViewTransitionOnNavigation"):
                raise RuntimeError(
                    f"Refusing to remove non-ViewTransition descriptor {name!r}")
            found.add(name)
            continue
        retained.append(entry)
        if isinstance(name, str):
            retained_names.add(name)

    expected = REMOVED_PROPERTIES | REMOVED_DESCRIPTORS
    if found != expected:
        missing = sorted(expected - found)
        extra = sorted(found - expected)
        raise RuntimeError(
            "The UI CSS profile must remove exactly its declared properties "
            f"and descriptors; missing={missing}, unexpected={extra}")
    missing_ordinary = REQUIRED_ORDINARY_ANIMATION_PROPERTIES - retained_names
    if missing_ordinary:
        raise RuntimeError(
            "The CSS input no longer contains required ordinary animation/"
            f"transition properties: {sorted(missing_ordinary)}")

    document["data"] = retained
    write_changed(destination, json_bytes(document))
    return len(entries), len(retained)


def make_filtered_named_database(root: Path, input_path: str, destination: Path,
                                json5_module, removed_names):
    source = root / input_path
    document = json5_module.loads(source.read_text(encoding="utf-8"))
    entries = document.get("data")
    if not isinstance(entries, list):
        raise RuntimeError(f"Expected a named-entry list in {source}")
    names = [entry if isinstance(entry, str) else entry.get("name") for entry in entries]
    removed = [name for name in names if name in removed_names]
    if len(removed) != len(removed_names) or set(removed) != removed_names:
        raise RuntimeError(f"ViewTransition entries changed in {source}: {removed}")
    document["data"] = [
        entry for entry, name in zip(entries, names) if name not in removed_names
    ]
    write_changed(destination, json_bytes(document))
    return len(entries), len(document["data"])


def canonical_inputs(extra=()):
    return [CSS_PROPERTIES, FIELD_ALIASES, RUNTIME_FEATURES, *extra]


def physical_inputs(root: Path, filtered: Path, extra=(), filtered_extra=None):
    return [
        str(filtered),
        str(root / FIELD_ALIASES),
        str(root / RUNTIME_FEATURES),
        *(str(filtered_extra if path == EXTRA_FIELDS and filtered_extra is not None
              else root / path) for path in extra),
    ]


def discover_generated_path(root: Path, filename: str):
    generated_core = root / "gen" / CORE
    matches = sorted(path for path in generated_core.rglob(filename)
                     if path.is_file())
    if len(matches) != 1:
        relative_matches = [
            path.relative_to(root).as_posix() for path in matches
        ]
        raise RuntimeError(
            f"Expected one existing generated output named {filename!r}, "
            f"found {relative_matches}")
    return matches[0]


def configure_writer(writer, canonical, relative_parent, gperf_path):
    # Constructors must read the staged database, but generated provenance and
    # include guards must never contain a random temporary directory.
    writer._input_files = list(canonical)
    writer._relative_output_dir = relative_parent.rstrip("/") + "/"
    writer.set_gperf_path(str(gperf_path))


def render_writers(root: Path, stage: Path, filtered: Path, gperf_path,
                   filtered_extra, filtered_descriptors):
    from core.css.make_css_property_names import CSSPropertyNamesWriter
    from core.css.make_css_value_id_mappings import CSSValueIDMappingsWriter
    from core.css.make_cssom_types import CSSOMTypesWriter
    from core.css.make_style_shorthands import StylePropertyShorthandWriter
    from core.css.parser.make_atrule_names import AtRuleNamesWriter
    from core.css.properties.make_css_property_instances import \
        CSSPropertyInstancesWriter
    from core.css.properties.make_css_property_subclasses import \
        CSSPropertiesWriter
    from core.css.properties.make_property_bitsets import PropertyBitsetsWriter
    from core.style.make_computed_style_base import ComputedStyleBaseWriter
    from core.style.make_computed_style_initial_values import \
        ComputedStyleInitialValuesWriter

    common_physical = physical_inputs(root, filtered)
    common_canonical = canonical_inputs()
    specifications = [
        (AtRuleNamesWriter, [str(filtered_descriptors)], [AT_RULE_NAMES]),
        (CSSPropertyNamesWriter, common_physical, common_canonical),
        (CSSPropertyInstancesWriter, common_physical, common_canonical),
        (CSSPropertiesWriter,
         physical_inputs(root, filtered, (PROPERTY_METHODS,)),
         canonical_inputs((PROPERTY_METHODS,))),
        (ComputedStyleInitialValuesWriter,
         physical_inputs(root, filtered, (EXTRA_FIELDS,), filtered_extra),
         canonical_inputs((EXTRA_FIELDS,))),
        (ComputedStyleBaseWriter,
         physical_inputs(root, filtered, (
             EXTRA_FIELDS, VALUE_KEYWORDS, PROPERTY_RANKING, GROUP_CONFIG), filtered_extra),
         canonical_inputs((
             EXTRA_FIELDS, VALUE_KEYWORDS, PROPERTY_RANKING, GROUP_CONFIG))),
        (CSSValueIDMappingsWriter,
         physical_inputs(root, filtered, (VALUE_KEYWORDS,)),
         canonical_inputs((VALUE_KEYWORDS,))),
        (StylePropertyShorthandWriter, common_physical, common_canonical),
        (CSSOMTypesWriter, common_physical, common_canonical),
        (PropertyBitsetsWriter, common_physical, common_canonical),
    ]

    build_file = (root / CORE / "BUILD.gn").read_text(encoding="utf-8")
    rendered = {}
    for writer_class, inputs, provenance in specifications:
        writer = writer_class(inputs, str(stage))
        for filename in writer._outputs:
            existing = discover_generated_path(root, filename)
            relative = existing.relative_to(root / "gen").as_posix()
            if relative in rendered:
                raise RuntimeError(f"Two CSS writers produce {relative}")
            core_relative = existing.relative_to(root / "gen" / CORE).as_posix()
            declaration = f'$blink_core_output_dir/{core_relative}'
            if declaration not in build_file:
                raise RuntimeError(
                    f"Generated output {relative} is not declared by {CORE}/BUILD.gn")
            rendered[relative] = (writer, filename)

        parents = {
            (root / "gen" / relative).parent.relative_to(root / "gen").as_posix()
            for relative, pair in rendered.items() if pair[0] is writer
        }
        if len(parents) != 1:
            raise RuntimeError(
                f"Writer {writer_class.__name__} spans output directories: "
                f"{sorted(parents)}")
        configure_writer(writer, provenance, next(iter(parents)), gperf_path)

    for relative, (writer, filename) in sorted(rendered.items()):
        content = writer._outputs[filename]().encode("utf-8")
        write_changed(stage / relative, content)
    return sorted(rendered)


def validate_generated(stage: Path, outputs):
    cpp_outputs = [path for path in outputs if path.endswith((".cc", ".h"))]
    for relative in cpp_outputs:
        text = (stage / relative).read_text(encoding="utf-8")
        for token in FORBIDDEN_GENERATED_TOKENS:
            if token in text:
                raise RuntimeError(
                    f"Filtered CSS output {relative} still references {token!r}")

    names_header = stage / CORE / "css/css_property_names.h"
    header = names_header.read_text(encoding="utf-8")
    for name in REQUIRED_ORDINARY_ANIMATION_PROPERTIES:
        enum_name = "".join(part.capitalize() for part in name.split("-"))
        if f"k{enum_name}" not in header:
            raise RuntimeError(
                f"Filtered CSS property enum lost required property {name!r}")
    for descriptor in REMOVED_DESCRIPTORS:
        enum_name = "".join(part.capitalize() for part in descriptor.split("-"))
        if f"k{enum_name}" in header:
            raise RuntimeError(
                f"Filtered CSS property enum retained VT descriptor {descriptor!r}")


def make_overlay(root: Path, out: Path, generated, selected_sources):
    out_relative = out.relative_to(root).as_posix()
    replacements = {}
    additions = []
    for relative in generated:
        if not relative.endswith(".cc"):
            continue
        original = f"gen/{relative}"
        replacement = f"{out_relative}/{relative}"
        if original in selected_sources:
            replacements[original] = replacement
        else:
            additions.append(replacement)
    return {
        "version": 1,
        "add": sorted(additions),
        "exclude": [],
        "replace": dict(sorted(replacements.items())),
        "include_dirs": [out_relative],
        "defines": [],
    }


def generator_fingerprint(root: Path, gperf_path: Path,
                          relevant_source_graph, dependency_versions):
    inputs = {
        Path(__file__).resolve(),
        root / f"{CORE}/BUILD.gn",
        root / f"{SCRIPTS}/scripts.gni",
        root / CSS_PROPERTIES,
        root / FIELD_ALIASES,
        root / RUNTIME_FEATURES,
        root / EXTRA_FIELDS,
        root / VALUE_KEYWORDS,
        root / PROPERTY_RANKING,
        root / GROUP_CONFIG,
        root / PROPERTY_METHODS,
        root / AT_RULE_NAMES,
    }
    scripts = root / SCRIPTS
    inputs.update(scripts.rglob("*.py"))
    inputs.update(scripts.rglob("*.tmpl"))

    digest = hashlib.sha256()
    for path in sorted(inputs):
        if not path.is_file():
            raise RuntimeError(f"Missing CSS generator input: {path}")
        try:
            label = path.relative_to(root).as_posix()
        except ValueError:
            label = path.name
        digest.update(label.encode("utf-8") + b"\0")
        digest.update(digest_file(path))
    digest.update(b"gperf\0" + digest_file(gperf_path))
    digest.update(json_bytes({
        "dependencies": dependency_versions,
        "selected_generated_sources": relevant_source_graph,
    }))
    return digest.hexdigest()


def cached_outputs(out: Path, fingerprint: str):
    state_path = out / STATE_FILE
    if not state_path.is_file():
        return None
    state = json.loads(state_path.read_text(encoding="utf-8"))
    if state.get("version") != 1 or state.get("inputs") != fingerprint:
        return None
    for name, expected in state.get("files", {}).items():
        path = (out / name).resolve()
        if not path.is_relative_to(out) or not path.is_file():
            return None
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            return None
    return state


def publish(out: Path, stage: Path, fingerprint: str):
    files = {
        path.relative_to(stage).as_posix(): path.read_bytes()
        for path in sorted(stage.rglob("*")) if path.is_file()
    }
    state_path = out / STATE_FILE
    if state_path.is_file():
        previous = json.loads(state_path.read_text(encoding="utf-8"))
        for name, old_hash in previous.get("files", {}).items():
            path = (out / name).resolve()
            if name in files or not path.is_relative_to(out) or not path.is_file():
                continue
            if hashlib.sha256(path.read_bytes()).hexdigest() != old_hash:
                raise RuntimeError(
                    f"Refusing to delete modified stale CSS output: {path}")
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
    parser.add_argument("--source-closure", type=Path,
                        help="Selected source manifest supplied before final graph generation")
    parser.add_argument(
        "--gperf",
        help="GNU gperf executable; defaults to Chromium's third_party tool "
             "or gperf on PATH")
    options = parser.parse_args()

    root = options.root.resolve()
    out = options.out.resolve()
    if out == root or not out.is_relative_to(root):
        parser.error("--out must be a child directory inside --root")
    out.mkdir(parents=True, exist_ok=True)

    sys.path.insert(0, str(root / SCRIPTS))
    try:
        import json5
        import jinja2
    except ModuleNotFoundError as error:
        parser.error(
            f"Missing CSS-generation dependency {error.name!r}; install the "
            f"json5 and jinja2 packages for {sys.executable}")

    gperf_path = resolve_executable(options.gperf, root)
    if gperf_path is None:
        parser.error(
            "GNU gperf is required. Restore Chromium's "
            "third_party/gperf/bin/gperf.exe, put gperf on PATH, set GPERF, "
            "or pass --gperf. This generator never downloads build tools.")

    selected_sources, relevant_graph = load_source_graph(root, options.source_closure)
    dependency_versions = {
        "jinja2": getattr(jinja2, "__version__", "unknown"),
        "json5": getattr(json5, "__version__", "unknown"),
    }
    fingerprint = generator_fingerprint(
        root, gperf_path, relevant_graph, dependency_versions)
    if cached_outputs(out, fingerprint) is not None:
        print(f"UI CSS database up to date: "
              f"{(out / OVERLAY_FILE).relative_to(root)}")
        return 0

    with tempfile.TemporaryDirectory(prefix=".ui-css-", dir=out) as scratch:
        work = Path(scratch)
        stage = work / "publish"
        filtered = work / "inputs/css_properties.json5"
        original_count, retained_count = make_filtered_database(
            root, filtered, json5)
        filtered_extra = work / "inputs/computed_style_extra_fields.json5"
        original_extra_count, retained_extra_count = make_filtered_named_database(
            root, EXTRA_FIELDS, filtered_extra, json5, REMOVED_EXTRA_FIELDS)
        filtered_descriptors = work / "inputs/at_rule_names.json5"
        original_descriptor_count, retained_descriptor_count = make_filtered_named_database(
            root, AT_RULE_NAMES, filtered_descriptors, json5, REMOVED_DESCRIPTORS)
        generated = render_writers(
            root, stage, filtered, gperf_path, filtered_extra, filtered_descriptors)
        validate_generated(stage, generated)

        overlay = make_overlay(root, out, generated, selected_sources)
        write_changed(stage / OVERLAY_FILE, json_bytes(overlay))
        profile = {
            "version": 1,
            "input": CSS_PROPERTIES,
            "original_property_count": original_count,
            "retained_property_count": retained_count,
            "removed_properties": sorted(REMOVED_PROPERTIES),
            "removed_descriptors": sorted(REMOVED_DESCRIPTORS),
            "removed_computed_extra_fields": sorted(REMOVED_EXTRA_FIELDS),
            "original_computed_extra_field_count": original_extra_count,
            "retained_computed_extra_field_count": retained_extra_count,
            "descriptor_input": AT_RULE_NAMES,
            "original_descriptor_count": original_descriptor_count,
            "retained_descriptor_count": retained_descriptor_count,
            "retained_animation_transition_sentinels":
                sorted(REQUIRED_ORDINARY_ANIMATION_PROPERTIES),
            "generated_files": generated,
            "replaced_translation_units": sorted(overlay["replace"]),
            "added_translation_units": overlay["add"],
        }
        write_changed(stage / PROFILE_FILE, json_bytes(profile))
        publish(out, stage, fingerprint)

    print(f"Generated {len(generated)} coherent UI CSS/style files; "
          f"removed {len(REMOVED_PROPERTIES)} compositor-only properties, "
          f"{len(REMOVED_DESCRIPTORS)} descriptors and "
          f"{len(REMOVED_EXTRA_FIELDS)} computed-style field")
    return 0


if __name__ == "__main__":
    if os.environ.get("PYTHONHASHSEED") != "0":
        environment = dict(os.environ, PYTHONHASHSEED="0")
        raise SystemExit(subprocess.call([
            sys.executable, str(Path(__file__).resolve()), *sys.argv[1:]
        ], env=environment))
    raise SystemExit(main())
