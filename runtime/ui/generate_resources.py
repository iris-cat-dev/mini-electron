#!/usr/bin/env python3
# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.
"""Generate Blink's built-in resources for the local-only UI profile."""

from __future__ import annotations

import argparse
import ast
from dataclasses import dataclass
import hashlib
import importlib.util
import json
import re
import sys
from pathlib import Path
import tempfile
import xml.etree.ElementTree as ET

RESOURCE_GRD = "third_party/blink/public/blink_resources.grd"
IMAGE_GRD = "third_party/blink/public/blink_image_resources.grd"
RESOURCE_IDS = "gen/third_party/blink/public/resources/grit/blink_resources.h"
IMAGE_IDS = "gen/third_party/blink/public/resources/grit/blink_image_resources.h"
POLICY = "build/mini_ui/sources.py"

HEADER_FILE = "mini_ui_blink_resources.h"
SOURCE_FILE = "mini_ui_blink_resources.cc"
MANIFEST_FILE = "mini_ui_blink_resources_manifest.json"
OVERLAY_FILE = "mini_ui_resource_sources.json"
STATE_FILE = "mini_ui_resource_generator_state.json"

# These are capability exclusions, never a resource allowlist. The inventory is
# derived from the GRDs and the generated Windows ID headers below.
EXPLICIT_OMISSIONS = frozenset({
    "IDR_UASTYLE_TRANSITION_CSS",
    "IDR_UASTYLE_TRANSITION_ANIMATIONS_CSS",
})
POLICY_OMISSION_SENTINELS = frozenset({
    "IDR_AUDIO_SPATIALIZATION_COMPOSITE",
})
WINDOWS_CONDITIONS = {
    "is_android": False,
    "is_macosx": False,
    "is_win": True,
}
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
FLATTEN_INCLUDE_DIRECTIVE = re.compile(
    br"(?:<\s*include(?:\s|>)|<!--\s*#?\s*include(?:\s|--?>))",
    re.IGNORECASE,
)
ID_DEFINE = re.compile(r"^\s*#define\s+(IDR_[A-Z0-9_]+)\s+(\d+)\s*$")
SYMBOL_NAME = re.compile(r"^IDR_[A-Z0-9_]+$")
SCALE_CONTEXT = re.compile(r"^default_([1-9][0-9]*)_percent$")


@dataclass(frozen=True)
class ResourceInput:
    symbol: str
    source: str
    scale_percent: int
    flattenhtml: bool
    data: bytes
    sha256: str


@dataclass(frozen=True)
class EmittedResource:
    symbol: str
    resource_id: int
    source: str
    scale_percent: int
    flattenhtml: bool
    data: bytes
    sha256: str


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, indent=2, ensure_ascii=False,
                       sort_keys=True) + "\n").encode("utf-8")


def write_changed(path: Path, content: bytes) -> None:
    if path.is_file() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def checkout_path(root: Path, base: Path, value: str, description: str) -> tuple[Path, str]:
    if not value or "\\" in value:
        raise RuntimeError(f"Invalid {description} path: {value!r}")
    path = (base / value).resolve()
    if not path.is_relative_to(root):
        raise RuntimeError(f"{description} path escapes the checkout: {value}")
    relative = path.relative_to(root).as_posix()
    if not path.is_file():
        raise RuntimeError(f"Missing genuine {description} input: {relative}")
    return path, relative


def evaluate_windows_condition(expression: str) -> bool:
    try:
        tree = ast.parse(expression, mode="eval")
    except SyntaxError as error:
        raise RuntimeError(f"Unsupported GRD condition {expression!r}") from error

    def evaluate(node: ast.AST) -> bool:
        if isinstance(node, ast.Expression):
            return evaluate(node.body)
        if isinstance(node, ast.Name):
            if node.id not in WINDOWS_CONDITIONS:
                raise RuntimeError(
                    f"Unsupported variable {node.id!r} in GRD condition {expression!r}")
            return WINDOWS_CONDITIONS[node.id]
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
            return not evaluate(node.operand)
        if isinstance(node, ast.BoolOp) and isinstance(node.op, (ast.And, ast.Or)):
            values = [evaluate(item) for item in node.values]
            return all(values) if isinstance(node.op, ast.And) else any(values)
        if isinstance(node, ast.Constant) and isinstance(node.value, bool):
            return node.value
        raise RuntimeError(f"Unsupported GRD condition {expression!r}")

    return evaluate(tree)


def active_inventory(container: ET.Element, item_tag: str) -> list[ET.Element]:
    result: list[ET.Element] = []

    def visit(parent: ET.Element, enabled: bool) -> None:
        for child in parent:
            if child.tag == item_tag:
                if len(child):
                    raise RuntimeError(f"GRD <{item_tag}> entries may not have children")
                if enabled:
                    result.append(child)
                continue
            if child.tag == "if":
                if set(child.attrib) != {"expr"}:
                    raise RuntimeError("GRD <if> must have exactly one expr attribute")
                condition = evaluate_windows_condition(child.attrib["expr"])
                visit(child, enabled and condition)
                continue
            raise RuntimeError(
                f"Unsupported <{child.tag}> in GRD <{container.tag}> inventory")

    visit(container, True)
    return result


def read_xml(path: Path) -> ET.Element:
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError) as error:
        raise RuntimeError(f"Unable to parse authoritative GRD {path}: {error}") from error
    if root.tag != "grit":
        raise RuntimeError(f"Expected <grit> root in {path}")
    return root


def release_section(document: ET.Element, section: str, path: Path) -> ET.Element:
    releases = document.findall("release")
    if len(releases) != 1:
        raise RuntimeError(f"Expected exactly one <release> in {path}")
    matches = releases[0].findall(section)
    if len(matches) != 1:
        raise RuntimeError(f"Expected exactly one <{section}> in {path}")
    return matches[0]


def read_payload(root: Path, base: Path, value: str, description: str,
                 *, require_png: bool = False) -> tuple[str, bytes, str]:
    path, relative = checkout_path(root, base, value, description)
    data = path.read_bytes()
    if require_png and not data.startswith(PNG_SIGNATURE):
        raise RuntimeError(f"Scaled image is not a genuine PNG: {relative}")
    return relative, data, hashlib.sha256(data).hexdigest()


def parse_bindata_resources(root: Path) -> tuple[list[ResourceInput], ET.Element]:
    grd_path = root / RESOURCE_GRD
    document = read_xml(grd_path)
    includes = active_inventory(
        release_section(document, "includes", grd_path), "include")
    resources: list[ResourceInput] = []
    seen: set[str] = set()
    allowed_attributes = {"name", "file", "type", "compress", "flattenhtml"}

    for include in includes:
        unexpected = set(include.attrib) - allowed_attributes
        if unexpected:
            raise RuntimeError(
                f"Unsupported attributes for Blink BINDATA resource: {sorted(unexpected)}")
        symbol = include.get("name", "")
        if not SYMBOL_NAME.fullmatch(symbol) or symbol in seen:
            raise RuntimeError(f"Invalid or duplicate Blink resource symbol: {symbol!r}")
        seen.add(symbol)
        if include.get("type") != "BINDATA":
            raise RuntimeError(f"Unsupported resource type for {symbol}: {include.get('type')!r}")
        if include.get("compress") not in (None, "brotli"):
            raise RuntimeError(
                f"Unsupported pack compression for {symbol}: {include.get('compress')!r}")
        flatten_value = include.get("flattenhtml")
        if flatten_value not in (None, "true", "false"):
            raise RuntimeError(
                f"Unsupported flattenhtml value for {symbol}: {flatten_value!r}")
        flattenhtml = flatten_value == "true"
        source, data, sha256 = read_payload(
            root, grd_path.parent, include.get("file", ""), "Blink resource")
        if flattenhtml and FLATTEN_INCLUDE_DIRECTIVE.search(data):
            raise RuntimeError(
                f"{symbol} ({source}) now requires a flattenhtml include transform; "
                "raw source bytes would be incorrect, so use a genuine GRIT transform")
        resources.append(ResourceInput(
            symbol=symbol,
            source=source,
            scale_percent=0,
            flattenhtml=flattenhtml,
            data=data,
            sha256=sha256,
        ))
    return resources, document


def image_scale_contexts(document: ET.Element, path: Path) -> list[tuple[int, str]]:
    outputs = document.findall("outputs")
    if len(outputs) != 1:
        raise RuntimeError(f"Expected exactly one <outputs> in {path}")
    contexts: dict[int, str] = {}
    for output in outputs[0].findall("output"):
        context = output.get("context")
        if context is None:
            continue
        match = SCALE_CONTEXT.fullmatch(context)
        if match is None:
            raise RuntimeError(f"Unsupported scaled-image context {context!r} in {path}")
        scale = int(match.group(1))
        old = contexts.setdefault(scale, context)
        if old != context:
            raise RuntimeError(f"Conflicting image contexts for {scale}% in {path}")
    if 100 not in contexts or 200 not in contexts:
        raise RuntimeError(
            f"Blink image GRD must declare its current 100% and 200% contexts: {path}")
    return sorted(contexts.items())


def parse_image_resources(
        root: Path) -> tuple[list[ResourceInput], list[dict[str, object]], ET.Element]:
    grd_path = root / IMAGE_GRD
    document = read_xml(grd_path)
    structures = release_section(document, "structures", grd_path)
    if structures.get("fallback_to_low_resolution") != "true":
        raise RuntimeError(
            f"Blink image GRD must retain fallback_to_low_resolution=true: {grd_path}")
    if set(structures.attrib) != {"fallback_to_low_resolution"}:
        raise RuntimeError(
            f"Unsupported <structures> metadata in {grd_path}: {sorted(structures.attrib)}")
    entries = active_inventory(structures, "structure")
    contexts = image_scale_contexts(document, grd_path)
    resources: list[ResourceInput] = []
    fallbacks: list[dict[str, object]] = []
    seen: set[str] = set()

    for structure in entries:
        if set(structure.attrib) != {"type", "name", "file"}:
            raise RuntimeError(
                f"Unsupported scaled-image metadata: {sorted(structure.attrib)}")
        symbol = structure.get("name", "")
        if not SYMBOL_NAME.fullmatch(symbol) or symbol in seen:
            raise RuntimeError(f"Invalid or duplicate Blink image symbol: {symbol!r}")
        seen.add(symbol)
        if structure.get("type") != "chrome_scaled_image":
            raise RuntimeError(
                f"Unsupported scaled-image type for {symbol}: {structure.get('type')!r}")
        value = structure.get("file", "")
        source_100: str | None = None
        for scale, context in contexts:
            candidate = (grd_path.parent / context / value).resolve()
            if not candidate.is_relative_to(root):
                raise RuntimeError(f"Scaled-image path escapes the checkout: {context}/{value}")
            if not candidate.is_file():
                if scale == 100:
                    raise RuntimeError(
                        f"Missing genuine 100% scaled image: "
                        f"{candidate.relative_to(root).as_posix()}")
                fallbacks.append({
                    "symbol": symbol,
                    "requested_scale_percent": scale,
                    "resolved_scale_percent": 100,
                })
                continue
            source, data, sha256 = read_payload(
                root, grd_path.parent, f"{context}/{value}",
                "Blink scaled-image", require_png=True)
            if scale == 100:
                source_100 = source
            resources.append(ResourceInput(
                symbol=symbol,
                source=source,
                scale_percent=scale,
                flattenhtml=False,
                data=data,
                sha256=sha256,
            ))
        if source_100 is None:
            raise RuntimeError(f"No truthful 100% fallback exists for {symbol}")
    return resources, sorted(
        fallbacks,
        key=lambda item: (str(item["symbol"]), int(item["requested_scale_percent"])),
    ), document


def parse_id_header(path: Path) -> dict[str, int]:
    if not path.is_file():
        raise RuntimeError(f"Missing generated resource ID header: {path}")
    result: dict[str, int] = {}
    used_ids: dict[int, str] = {}
    for line in path.read_text(encoding="utf-8-sig").splitlines():
        match = ID_DEFINE.fullmatch(line)
        if match is None:
            continue
        symbol, value_text = match.groups()
        value = int(value_text)
        if value > 0x7fffffff:
            raise RuntimeError(f"Resource ID does not fit C++ int: {symbol}={value}")
        if symbol in result:
            raise RuntimeError(f"Duplicate resource symbol in {path}: {symbol}")
        if value in used_ids:
            raise RuntimeError(
                f"Duplicate resource ID {value} in {path}: {used_ids[value]}, {symbol}")
        result[symbol] = value
        used_ids[value] = symbol
    if not result:
        raise RuntimeError(f"No IDR_* definitions found in {path}")
    return result


def validate_header_inventory(
        inputs: list[ResourceInput], ids: dict[str, int], path: Path) -> None:
    symbols = {item.symbol for item in inputs}
    if symbols != set(ids):
        raise RuntimeError(
            f"Generated Windows resource header is stale relative to its GRD: {path}; "
            f"missing={sorted(symbols - set(ids))}, unexpected={sorted(set(ids) - symbols)}")


def load_policy(root: Path):
    path = root / POLICY
    if not path.is_file():
        raise RuntimeError(f"Missing UI source policy: {path}")
    name = "mini_ui_resource_policy"
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Unable to load UI source policy: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    forbidden = getattr(module, "forbidden", None)
    if not callable(forbidden):
        raise RuntimeError(f"UI source policy has no callable forbidden(): {path}")
    return forbidden


def apply_profile(
        bindata: list[ResourceInput], images: list[ResourceInput],
        bindata_ids: dict[str, int], image_ids: dict[str, int], forbidden,
) -> tuple[list[EmittedResource], list[dict[str, object]]]:
    bindata_symbols = {item.symbol for item in bindata}
    if not EXPLICIT_OMISSIONS.issubset(bindata_symbols):
        raise RuntimeError(
            "Blink GRD no longer contains the two exact UI view-transition omissions: "
            f"{sorted(EXPLICIT_OMISSIONS - bindata_symbols)}")

    retained: list[EmittedResource] = []
    omissions: list[dict[str, object]] = []
    policy_results: dict[tuple[str, str, int], str | None] = {}

    for item, ids in [
            *((entry, bindata_ids) for entry in bindata),
            *((entry, image_ids) for entry in images),
    ]:
        reason = forbidden(item.source)
        if reason is not None and not isinstance(reason, str):
            raise RuntimeError(
                f"UI source policy returned an invalid reason for {item.source}: {reason!r}")
        policy_results[(item.symbol, item.source, item.scale_percent)] = reason
        resource_id = ids[item.symbol]
        explicit = item.symbol in EXPLICIT_OMISSIONS
        if explicit or reason is not None:
            omissions.append({
                "resource_id": resource_id,
                "symbol": item.symbol,
                "source": item.source,
                "scale_percent": item.scale_percent,
                "size": len(item.data),
                "sha256": item.sha256,
                "reason": ("UI profile excludes view-transition rendering"
                           if explicit else f"source policy: {reason}"),
            })
            continue
        retained.append(EmittedResource(
            symbol=item.symbol,
            resource_id=resource_id,
            source=item.source,
            scale_percent=item.scale_percent,
            flattenhtml=item.flattenhtml,
            data=item.data,
            sha256=item.sha256,
        ))

    for symbol in POLICY_OMISSION_SENTINELS:
        matching = [item for item in bindata if item.symbol == symbol]
        if len(matching) != 1:
            raise RuntimeError(
                f"Required removed-capability resource changed or disappeared: {symbol}")
        item = matching[0]
        if policy_results[(item.symbol, item.source, item.scale_percent)] is None:
            raise RuntimeError(
                f"UI source policy unexpectedly permits removed-capability payload "
                f"{symbol} from {item.source}")

    retained_symbols = {item.symbol for item in retained}
    for symbol in {item.symbol for item in images} & retained_symbols:
        scales = {item.scale_percent for item in retained if item.symbol == symbol}
        if 100 not in scales:
            raise RuntimeError(f"Retained scaled image has no truthful 100% fallback: {symbol}")

    retained.sort(key=lambda item: (item.resource_id, item.scale_percent, item.source))
    omissions.sort(key=lambda item: (
        int(item["resource_id"]), int(item["scale_percent"]), str(item["source"])))
    return retained, omissions


def cpp_name(item: EmittedResource) -> str:
    suffix = "Default" if item.scale_percent == 0 else f"Scale{item.scale_percent}"
    return f"{item.symbol}_{suffix}"


def render_byte_array(name: str, data: bytes) -> list[str]:
    values = data if data else b"\0"
    lines = [f"alignas(16) constexpr unsigned char kData_{name}[] = {{"]
    for offset in range(0, len(values), 16):
        chunk = values[offset:offset + 16]
        lines.append("    " + ", ".join(f"0x{value:02x}" for value in chunk) + ",")
    lines.append("};")
    return lines


def render_header() -> bytes:
    return (
        "// Generated by runtime/ui/generate_resources.py. Do not edit.\n"
        "#ifndef MINI_UI_GENERATED_MINI_UI_BLINK_RESOURCES_H_\n"
        "#define MINI_UI_GENERATED_MINI_UI_BLINK_RESOURCES_H_\n\n"
        "#include <stddef.h>\n\n"
        "namespace mini_ui::detail {\n\n"
        "struct BuiltinBlinkResource {\n"
        "  const unsigned char* data;\n"
        "  size_t size;\n"
        "};\n\n"
        "const BuiltinBlinkResource* FindBuiltinBlinkResource(\n"
        "    int resource_id, int scale_percent = 0);\n\n"
        "}  // namespace mini_ui::detail\n\n"
        "#endif  // MINI_UI_GENERATED_MINI_UI_BLINK_RESOURCES_H_\n"
    ).encode("utf-8")


def render_source(resources: list[EmittedResource]) -> bytes:
    lines = [
        "// Generated by runtime/ui/generate_resources.py. Do not edit.",
        f'#include "{HEADER_FILE}"',
        "",
        '#include "third_party/blink/public/resources/grit/blink_image_resources.h"',
        '#include "third_party/blink/public/resources/grit/blink_resources.h"',
        "",
        "namespace mini_ui::detail {",
        "namespace {",
        "",
    ]
    for item in resources:
        name = cpp_name(item)
        lines.extend(render_byte_array(name, item.data))
        logical_size = "0" if not item.data else f"sizeof(kData_{name})"
        lines.extend([
            f"constexpr BuiltinBlinkResource kResource_{name} = {{",
            f"    kData_{name}, {logical_size}",
            "};",
            "",
        ])
    lines.extend([
        "}  // namespace",
        "",
        "const BuiltinBlinkResource* FindBuiltinBlinkResource(",
        "    int resource_id, int scale_percent) {",
        "  switch (resource_id) {",
    ])

    grouped: dict[str, list[EmittedResource]] = {}
    for item in resources:
        grouped.setdefault(item.symbol, []).append(item)
    for symbol, variants in sorted(
            grouped.items(), key=lambda pair: pair[1][0].resource_id):
        lines.append(f"    case {symbol}:")
        default = next((item for item in variants if item.scale_percent == 0), None)
        if default is not None:
            lines.append(f"      return &kResource_{cpp_name(default)};")
            continue
        scale_100 = next(item for item in variants if item.scale_percent == 100)
        for item in sorted(
                (entry for entry in variants if entry.scale_percent != 100),
                key=lambda entry: entry.scale_percent):
            lines.extend([
                f"      if (scale_percent == {item.scale_percent})",
                f"        return &kResource_{cpp_name(item)};",
            ])
        lines.append(f"      return &kResource_{cpp_name(scale_100)};")

    lines.extend([
        "    default:",
        "      return nullptr;",
        "  }",
        "}",
        "",
        "}  // namespace mini_ui::detail",
        "",
    ])
    return "\n".join(lines).encode("utf-8")


def generator_fingerprint(
        root: Path, out: Path, payloads: list[ResourceInput]) -> str:
    inputs = {
        Path(__file__).resolve(),
        root / POLICY,
        root / RESOURCE_GRD,
        root / IMAGE_GRD,
        root / RESOURCE_IDS,
        root / IMAGE_IDS,
        *(root / item.source for item in payloads),
    }
    digest = hashlib.sha256()
    for path in sorted(inputs, key=lambda item: item.as_posix()):
        if not path.is_file():
            raise RuntimeError(f"Missing resource generator input: {path}")
        label = (path.relative_to(root).as_posix()
                 if path.is_relative_to(root) else path.name)
        digest.update(label.encode("utf-8") + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    digest.update(json_bytes({
        "output_root": out.relative_to(root).as_posix(),
        "windows_conditions": WINDOWS_CONDITIONS,
    }))
    return digest.hexdigest()


def expected_owned_files() -> frozenset[str]:
    return frozenset({HEADER_FILE, SOURCE_FILE, MANIFEST_FILE, OVERLAY_FILE})


def cached_outputs(out: Path, fingerprint: str) -> bool:
    state_path = out / STATE_FILE
    if not state_path.is_file():
        return False
    try:
        state = json.loads(state_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return False
    files = state.get("files")
    if (state.get("version") != 1 or state.get("inputs") != fingerprint
            or not isinstance(files, dict)
            or set(files) != expected_owned_files()):
        return False
    for name, expected_hash in files.items():
        path = (out / name).resolve()
        if (not path.is_relative_to(out) or not path.is_file()
                or not isinstance(expected_hash, str)
                or hashlib.sha256(path.read_bytes()).hexdigest() != expected_hash):
            return False
    return True


def make_overlay(root: Path, out: Path) -> dict[str, object]:
    include_root = out.relative_to(root).as_posix()
    return {
        "version": 1,
        "add": [f"{include_root}/{SOURCE_FILE}"],
        "exclude": [],
        "replace": {},
        "include_dirs": [include_root],
        "defines": [],
    }


def make_manifest(
        fingerprint: str, resources: list[EmittedResource],
        omissions: list[dict[str, object]], fallbacks: list[dict[str, object]],
) -> dict[str, object]:
    return {
        "version": 1,
        "generator": "runtime/ui/generate_resources.py",
        "input_fingerprint": fingerprint,
        "resource_grds": [RESOURCE_GRD, IMAGE_GRD],
        "resource_id_headers": [RESOURCE_IDS, IMAGE_IDS],
        "resources": [{
            "resource_id": item.resource_id,
            "symbol": item.symbol,
            "source": item.source,
            "scale_percent": item.scale_percent,
            "size": len(item.data),
            "sha256": item.sha256,
            "flattenhtml": item.flattenhtml,
            "storage": "raw_decompressed_bytes",
        } for item in resources],
        "omissions": omissions,
        "scale_resolution": {
            "fallback_scale_percent": 100,
            "missing_variants": fallbacks,
        },
    }


def publish(out: Path, stage: Path, fingerprint: str) -> None:
    files = {
        path.relative_to(stage).as_posix(): path.read_bytes()
        for path in sorted(stage.rglob("*")) if path.is_file()
    }
    if set(files) != expected_owned_files():
        raise RuntimeError(
            "Resource generator staged an unexpected output set: "
            f"{sorted(set(files) ^ expected_owned_files())}")

    state_path = out / STATE_FILE
    if state_path.is_file():
        try:
            previous = json.loads(state_path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError) as error:
            raise RuntimeError(f"Invalid previous resource generator state: {state_path}") from error
        previous_files = previous.get("files", {})
        if not isinstance(previous_files, dict):
            raise RuntimeError(f"Invalid previous resource generator state: {state_path}")
        for name, old_hash in previous_files.items():
            path = (out / name).resolve()
            if name in files or not path.is_relative_to(out) or not path.is_file():
                continue
            if not isinstance(old_hash, str) or hashlib.sha256(path.read_bytes()).hexdigest() != old_hash:
                raise RuntimeError(
                    f"Refusing to delete modified stale resource output: {path}")
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    options = parser.parse_args()

    root = options.root.resolve()
    out = options.out.resolve()
    if not root.is_dir():
        parser.error(f"--root is not a repository directory: {root}")
    if out == root or not out.is_relative_to(root):
        parser.error("--out must be a child directory inside --root")
    out.mkdir(parents=True, exist_ok=True)

    bindata, _ = parse_bindata_resources(root)
    images, fallbacks, _ = parse_image_resources(root)
    bindata_ids = parse_id_header(root / RESOURCE_IDS)
    image_ids = parse_id_header(root / IMAGE_IDS)
    validate_header_inventory(bindata, bindata_ids, root / RESOURCE_IDS)
    validate_header_inventory(images, image_ids, root / IMAGE_IDS)

    forbidden = load_policy(root)
    resources, omissions = apply_profile(
        bindata, images, bindata_ids, image_ids, forbidden)
    fingerprint = generator_fingerprint(root, out, [*bindata, *images])
    if cached_outputs(out, fingerprint):
        print(f"Blink UI resources up to date: "
              f"{(out / OVERLAY_FILE).relative_to(root)}")
        return 0

    with tempfile.TemporaryDirectory(prefix=".ui-resources-", dir=out) as scratch:
        stage = Path(scratch) / "publish"
        write_changed(stage / HEADER_FILE, render_header())
        write_changed(stage / SOURCE_FILE, render_source(resources))
        write_changed(stage / MANIFEST_FILE, json_bytes(make_manifest(
            fingerprint, resources, omissions, fallbacks)))
        write_changed(stage / OVERLAY_FILE, json_bytes(make_overlay(root, out)))
        publish(out, stage, fingerprint)

    retained_ids = len({item.resource_id for item in resources})
    print(f"Generated {retained_ids} Blink UI resource IDs "
          f"({len(resources)} payload variants); omitted {len(omissions)} payloads")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
