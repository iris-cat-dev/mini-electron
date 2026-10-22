#!/usr/bin/env python3
# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.
"""Regenerate the retained Blink IDL graph and native registration factories."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

BLINK = "third_party/blink/renderer"
BINDINGS = f"{BLINK}/bindings"
COMPONENTS = ("core", "modules")
EXTRA_REMOVED_IDL = {
    f"{BLINK}/core/html/html_permission_element.idl",
    f"{BLINK}/modules/canvas/canvas2d/canvas_2d_gpu_transfer.idl",
    f"{BLINK}/core/fetch/attribution_reporting.idl",
    f"{BLINK}/core/html/html_attributionsrc_element_utils.idl",
    f"{BLINK}/core/css/css_view_transition_rule.idl",
}
EXTRA_REMOVED_MEMBERS = {
    "Document": ("cookie",),
    "DocumentOrShadowRoot": ("pictureInPictureElement",),
    "Window": ("onpageswap", "onpagereveal"),
    "HTMLObjectElement": ("",),
    "HTMLEmbedElement": ("",),
    "RequestInit": ("attributionReporting",),
    "XMLHttpRequest": ("setAttributionReporting",),
    "PerformanceResourceTiming": (
        "workerRouterEvaluationStart", "workerCacheLookupStart",
        "workerMatchedSourceType", "workerFinalSourceType",
    ),
}
REMOVED_SETTINGS = {
    "autoplayPolicy", "textTrackKindUserPreference", "v8CacheOptions",
    "cookieEnabled", "localStorageEnabled",
}
SETTINGS_INPUT = f"{BLINK}/core/frame/settings.json5"
POLICY_INPUTS = (
    f"{BLINK}/core/permissions_policy/document_policy_features.json5",
    f"{BLINK}/core/permissions_policy/permissions_policy_features.json5",
    f"{BLINK}/platform/runtime_enabled_features.json5",
)
BINDING_TASKS = (
    "async_iterator", "callback_function", "callback_interface", "dictionary",
    "enumeration", "interface", "namespace", "observable_array",
    "sync_iterator", "typedef", "union",
)
FACTORIES = (
    f"{BLINK}/core/html_element_factory.cc",
    f"{BLINK}/core/event_factory.cc",
    f"{BLINK}/core/html_element_type_helpers.cc",
)
LOCATION_PROPERTIES = {"FILENAME", "LINENO", "POSITION", "ERRORS", "WARNINGS"}


def json_bytes(value):
    return (json.dumps(value, indent=2, ensure_ascii=False, sort_keys=True) + "\n").encode("utf-8")


def write_changed(path: Path, content: bytes):
    if path.is_file() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def load_policy(root: Path):
    name = "mini_ui_binding_source_policy"
    spec = importlib.util.spec_from_file_location(name, root / "build/mini_ui/sources.py")
    if spec is None or spec.loader is None:
        raise RuntimeError("Cannot load the UI implementation dependency policy")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def source_path(node):
    path = node.GetProperty("FILENAME").replace("\\", "/")
    position = path.find("third_party/blink/")
    if position < 0:
        raise ValueError(f"Unexpected IDL source path: {path}")
    return path[position:]


def replace_children(node, children):
    node._children = children
    for child in children:
        child._parent = node


def prune_runtime_annotations(node):
    # V8's fast-call ABI and browser privacy reports do not change Web IDL behavior.
    children = [child for child in node.GetChildren()
                if not (child.GetClass() == "ExtAttribute"
                        and child.GetName() in {"NoAllocDirectCall", "HighEntropy"})]
    replace_children(node, children)
    for child in children:
        prune_runtime_annotations(child)


def retain_exposure(node):
    attributes = node.GetOneOf("ExtAttributes")
    if attributes is None:
        return True
    for attribute in attributes.GetChildren():
        if attribute.GetName() != "Exposed":
            continue
        value = attribute.GetProperty("VALUE")
        values = value if isinstance(value, list) else [value]
        if "*" in values:
            return True
        if "Window" not in values:
            return False
        attribute.SetProperty("VALUE", "Window")
    return True


def prune_type(node, removed):
    if node.GetClass() == "Typeref" and node.GetName() in removed:
        return None
    previous = list(node.GetChildren())
    children = []
    for child in previous:
        retained = prune_type(child, removed)
        if retained is None:
            if node.GetClass() != "UnionType":
                return None
        else:
            children.append(retained)
    if previous and not children:
        return None
    replace_children(node, children)
    if (node.GetClass() == "Type" and len(children) == 1
            and children[0].GetClass() == "UnionType"
            and len(children[0].GetChildren()) == 1):
        remaining = children[0].GetChildren()[0]
        for key, value in remaining.GetProperties().items():
            if key in LOCATION_PROPERTIES:
                continue
            if key == "NULLABLE":
                value = bool(value or node.GetProperty(key))
            node.SetProperty(key, value)
        replace_children(node, list(remaining.GetChildren()))
    return node


def prune_groups(groups, policy):
    def retained_file(file_node):
        path = source_path(file_node)
        return path not in EXTRA_REMOVED_IDL and policy.forbidden(path) is None

    all_names = set()
    retained_names = set()
    all_paths = set()
    for group in groups.values():
        for file_node in group:
            all_paths.add(source_path(file_node))
            for node in file_node.GetChildren():
                if node.GetClass() == "Includes" or node.GetProperty("PARTIAL"):
                    continue
                all_names.add(node.GetName())
                if retained_file(file_node) and retain_exposure(node):
                    retained_names.add(node.GetName())
    removed = all_names - retained_names
    for group in groups.values():
        group._nodes = [node for node in group if retained_file(node)]
        for node in group:
            prune_runtime_annotations(node)

    while True:
        newly_removed = set()
        for group in groups.values():
            for file_node in group:
                definitions = []
                for node in file_node.GetChildren():
                    name = node.GetName()
                    if node.GetClass() == "Includes":
                        if name in retained_names and node.GetProperty("REFERENCE") in retained_names:
                            definitions.append(node)
                        continue
                    if name in removed or not retain_exposure(node):
                        continue
                    if node.GetProperty("PARTIAL") and name not in retained_names:
                        continue
                    inherited = node.GetOneOf("Inherit")
                    if inherited is not None and inherited.GetName() in removed:
                        newly_removed.add(name)
                        continue
                    if node.GetClass() in ("Interface", "Namespace", "Dictionary"):
                        members = []
                        removed_members = EXTRA_REMOVED_MEMBERS.get(name, ())
                        for member in node.GetChildren():
                            if member.GetClass() in ("ExtAttributes", "Inherit"):
                                members.append(member)
                            elif (member.GetName() not in removed_members
                                  and prune_type(member, removed) is not None):
                                members.append(member)
                        replace_children(node, members)
                        definitions.append(node)
                    elif prune_type(node, removed) is not None:
                        definitions.append(node)
                    else:
                        newly_removed.add(name)
                replace_children(file_node, definitions)
        if not newly_removed:
            break
        removed.update(newly_removed)
        retained_names.difference_update(newly_removed)

    kept_paths = {source_path(node) for group in groups.values()
                  for node in group if node.GetChildren()}
    return removed, sorted(all_paths - kept_paths)


def generator_input_hash(root: Path):
    inputs = {
        Path(__file__).resolve(),
        root / "build/mini_ui/sources.py",
        root / "tools/idl_parser/idl_node.py",
        root / f"{BLINK}/platform/runtime_enabled_features.json5",
        root / f"{BLINK}/core/html/html_tag_names.json5",
        root / f"{BLINK}/core/html/html_attribute_names.json5",
        root / f"{BLINK}/core/events/event_interface_aliases.json5",
        root / f"gen/{BLINK}/core/event_interface_names.json5",
        root / SETTINGS_INPUT,
        *(root / path for path in POLICY_INPUTS),
    }
    for component in COMPONENTS:
        inputs.add(root / f"gen/{BINDINGS}/web_idl_in_{component}.pickle")
    for directory in (root / BINDINGS / "scripts", root / BLINK / "build/scripts"):
        inputs.update(directory.rglob("*.py"))
        inputs.update(directory.rglob("*.tmpl"))
    digest = hashlib.sha256()
    for path in sorted(inputs):
        relative = path.relative_to(root).as_posix()
        digest.update(relative.encode("utf-8") + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def cached_outputs(out: Path, fingerprint: str):
    state_path = out / "generator-state.json"
    if not state_path.is_file():
        return None
    state = json.loads(state_path.read_text(encoding="utf-8"))
    if state.get("version") != 1 or state.get("inputs") != fingerprint:
        return None
    for name, digest in state.get("files", {}).items():
        path = (out / name).resolve()
        if not path.is_relative_to(out) or not path.is_file():
            return None
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            return None
    return state


def generate_factories(root: Path, stage: Path, removed):
    from make_element_factory import MakeElementFactoryWriter
    from make_event_factory import EventFactoryWriter
    from make_element_type_helpers import MakeElementTypeHelpersWriter

    html_inputs = [f"{BLINK}/core/html/html_tag_names.json5",
                   f"{BLINK}/core/html/html_attribute_names.json5"]
    writer = MakeElementFactoryWriter(
        [str(root / name) for name in html_inputs], str(stage / BLINK / "core"))
    writer._input_files = html_inputs
    writer._template_context["input_files"] = html_inputs
    writer._template_context["tags"] = [
        tag for tag in writer._template_context["tags"]
        if tag["interface"] not in removed
    ]
    write_changed(stage / FACTORIES[0], writer.generate_factory_implementation().encode("utf-8"))
    write_changed(stage / BLINK / "core/html_element_factory.h",
                  writer.generate_factory_header().encode("utf-8"))

    helpers = MakeElementTypeHelpersWriter(
        [str(root / html_inputs[0])], str(stage / BLINK / "core"))
    helpers._input_files = [html_inputs[0]]
    helpers._template_context["input_files"] = [html_inputs[0]]
    helpers._template_context["tags"] = [
        tag for tag in helpers._template_context["tags"]
        if tag["interface"] not in removed and tag["js_interface"] not in removed
    ]
    helpers._template_context["elements"] = {
        tag["js_interface"] for tag in helpers._template_context["tags"]
    }
    for filename, generate in helpers._outputs.items():
        write_changed(stage / BLINK / "core" / filename, generate().encode("utf-8"))

    event_inputs = [f"{BLINK}/core/events/event_interface_aliases.json5",
                    f"gen/{BLINK}/core/event_interface_names.json5"]
    writer = EventFactoryWriter(
        [str(root / name) for name in event_inputs], str(stage / BLINK / "core"))
    writer._input_files = event_inputs
    writer.json5_file.name_dictionaries[:] = [
        entry for entry in writer.json5_file.name_dictionaries
        if entry["name"].original not in removed
    ]
    write_changed(stage / FACTORIES[1], writer.generate_implementation().encode("utf-8"))


def generate_core_configuration(root: Path, stage: Path, policy):
    from make_settings import MakeSettingsWriter
    from make_policy_helper import PermissionsPolicyFeatureWriter

    settings = MakeSettingsWriter(
        [str(root / SETTINGS_INPUT)], str(stage / BLINK / "core"))
    settings._input_files = [SETTINGS_INPUT]
    settings._relative_output_dir = f"{BLINK}/core/"
    removed_settings = []
    retained_settings = []
    for entry in settings.json5_file.name_dictionaries:
        name = entry["name"].original
        if name in REMOVED_SETTINGS or any(
                policy.forbidden(path, header=True) for path in entry["include_paths"]):
            removed_settings.append(name)
        else:
            retained_settings.append(entry)
    settings.json5_file.name_dictionaries[:] = retained_settings
    outputs = []
    for filename, generate in settings._outputs.items():
        path = f"{BLINK}/core/{filename}"
        write_changed(stage / path, generate().encode("utf-8"))
        outputs.append(path)

    permissions = PermissionsPolicyFeatureWriter(
        [str(root / path) for path in POLICY_INPUTS],
        str(stage / BLINK / "core/permissions_policy"))
    permissions._input_files = list(POLICY_INPUTS)
    for filename, generate in permissions._outputs.items():
        path = f"{BLINK}/core/permissions_policy/{filename}"
        write_changed(stage / path, generate().encode("utf-8"))
        outputs.append(path)
    return outputs, sorted(removed_settings)


def publish(root: Path, out: Path, stage: Path, fingerprint: str):
    files = {path.relative_to(stage).as_posix(): path.read_bytes()
             for path in sorted(stage.rglob("*")) if path.is_file()}
    previous_path = out / "generator-state.json"
    if previous_path.is_file():
        previous = json.loads(previous_path.read_text(encoding="utf-8"))
        for name, old_hash in previous.get("files", {}).items():
            path = (out / name).resolve()
            if name not in files and path.is_relative_to(out) and path.is_file():
                if hashlib.sha256(path.read_bytes()).hexdigest() != old_hash:
                    raise RuntimeError(f"Refusing to delete a modified stale generated file: {path}")
                path.unlink()
    for name, content in files.items():
        write_changed(out / name, content)
    state = {"version": 1, "inputs": fingerprint,
             "files": {name: hashlib.sha256(content).hexdigest()
                       for name, content in files.items()}}
    write_changed(out / "generator-state.json", json_bytes(state))


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
    fingerprint = generator_input_hash(root)
    if cached_outputs(out, fingerprint) is not None:
        print(f"UI bindings up to date: {(out / 'mini_ui_sources.json').relative_to(root)}")
        return 0

    sys.path[:0] = [str(root / "tools"), str(root / BINDINGS / "scripts"),
                    str(root / BLINK / "build/scripts")]
    try:
        import json5  # noqa: F401 - required by the bundled IDL compiler.
        import mako  # noqa: F401 - required by the bundled binding generator.
        import jinja2  # noqa: F401 - required by the bundled factory templates.
        import web_idl
    except ModuleNotFoundError as error:
        parser.error(f"Missing binding-generation dependency {error.name!r}; "
                     f"install json5, mako and jinja2 with {sys.executable} -m pip")

    policy = load_policy(root)
    groups = {component: web_idl.AstGroup.read_from_file(
        str(root / f"gen/{BINDINGS}/web_idl_in_{component}.pickle"))
        for component in COMPONENTS}
    removed, removed_paths = prune_groups(groups, policy)
    web_idl.init(runtime_enabled_features_paths=[
        str(root / BLINK / "platform/runtime_enabled_features.json5")])

    with tempfile.TemporaryDirectory(prefix=".ui-bindings-", dir=out) as scratch:
        stage = Path(scratch)
        ast_inputs = []
        for component, group in groups.items():
            path = stage / f"ui-{component}-ast.pickle"
            group.write_to_file(str(path))
            ast_inputs.append(str(path))
        errors = []
        database = web_idl.build_database(filepaths=ast_inputs, report_error=errors.append)
        if errors:
            raise RuntimeError("Invalid retained Web IDL graph:\n" + "\n".join(errors))
        database_path = stage / "ui-web-idl.pickle"
        database.write_to_file(str(database_path))
        command = [sys.executable, str(root / BINDINGS / "scripts/generate_bindings.py"),
                   "--web_idl_database", str(database_path), "--root_src_dir", str(root),
                   "--root_gen_dir", str(stage), "--single_process"]
        for component in COMPONENTS:
            command.extend(["--output_reldir", f"{component}={BINDINGS}/{component}/v8"])
        command.extend(BINDING_TASKS)
        subprocess.run(command, cwd=root, check=True)
        generate_factories(root, stage, removed)
        configuration_outputs, removed_settings = generate_core_configuration(root, stage, policy)

        out_relative = out.relative_to(root).as_posix()
        binding_sources = sorted(
            f"{out_relative}/{path.relative_to(stage).as_posix()}"
            for path in (stage / BINDINGS).rglob("*.cc")
            if "for_testing" not in path.name)
        old_bindings = sorted(path.relative_to(root).as_posix()
                              for path in (root / "gen" / BINDINGS).rglob("*.cc"))
        removed_native = [path.removesuffix(".idl") + ".cc" for path in removed_paths
                          if (root / (path.removesuffix(".idl") + ".cc")).is_file()]
        overlay = {
            "version": 1,
            "add": binding_sources,
            "exclude": sorted(set(old_bindings + removed_native + [
                f"{BLINK}/modules/modules_initializer_lite.cc",
                f"{BINDINGS}/modules/v8/module_bindings_initializer.cc",
            ])),
            "replace": {f"gen/{path}": f"{out_relative}/{path}"
                        for path in (*FACTORIES, *configuration_outputs)
                        if path.endswith(".cc")},
            "include_dirs": [out_relative],
            "defines": [],
        }
        write_changed(stage / "mini_ui_sources.json", json_bytes(overlay))
        profile = {
            "version": 1,
            "interfaces": len(database.interfaces),
            "dictionaries": len(database.dictionaries),
            "unions": len(database.union_types),
            "removed_definitions": sorted(removed),
            "removed_idl_files": removed_paths,
            "removed_members": EXTRA_REMOVED_MEMBERS,
            "removed_settings": removed_settings,
            "retained_idl_files": {
                component: sorted(source_path(node) for node in group if node.GetChildren())
                for component, group in groups.items()},
        }
        write_changed(stage / "mini_ui_profile.json", json_bytes(profile))
        publish(root, out, stage, fingerprint)
    print(f"Generated {len(binding_sources)} UI binding translation units "
          f"for {len(database.interfaces)} interfaces")
    return 0


if __name__ == "__main__":
    # Pickles, origin-trial sets and generated output must not depend on hash seed.
    if os.environ.get("PYTHONHASHSEED") != "0":
        environment = dict(os.environ, PYTHONHASHSEED="0")
        raise SystemExit(subprocess.call([sys.executable, str(Path(__file__).resolve()),
                                          *sys.argv[1:]], env=environment))
    raise SystemExit(main())
