# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from pathlib import Path
import re


_BOOTSTRAPS = ("internal/bootstrap/node", "internal/main/worker_thread")


def generate_node_bootstraps(root: Path) -> None:
    """Refresh vendored js2c resources from their canonical bootstrap scripts."""
    node = root / "third_party" / "libnode"
    generated = node / "gen" / "src" / "node" / "node_javascript.cc"
    original = generated.read_text(encoding="utf-8")
    result = original
    for module in _BOOTSTRAPS:
        source = (node / "lib" / (module + ".js")).read_text(encoding="utf-8")
        index = re.search(
            r'\{"' + re.escape(module) + r'", UnionBytes\(&([A-Za-z0-9_]+)_resource\)',
            result,
        )
        if index is None:
            raise RuntimeError(f"Node js2c index is missing {module}")
        identifier = index.group(1)
        pattern = re.compile(
            r"static const uint(?:8|16)_t " + re.escape(identifier)
            + r"_raw\[\] = \{[\s\S]*?\};\s*"
            + r"static StaticExternal(?:One|Two)ByteResource " + re.escape(identifier)
            + r"_resource\([^;]+;"
        )
        matches = list(pattern.finditer(result))
        if len(matches) != 1:
            raise RuntimeError(f"Expected one Node js2c resource for {module}")
        if source.isascii():
            values = source.encode("ascii")
            bits, kind = 8, "One"
        else:
            encoded = source.encode("utf-16-le")
            values = [int.from_bytes(encoded[i:i + 2], "little") for i in range(0, len(encoded), 2)]
            bits, kind = 16, "Two"
        definition = (
            f"static const uint{bits}_t {identifier}_raw[] = {{\n"
            + ",".join(str(value) for value in values) + "\n};\n\n"
            + f"static StaticExternal{kind}ByteResource {identifier}_resource("
            + f"{identifier}_raw, {len(values)}, nullptr);"
        )
        match = matches[0]
        result = result[:match.start()] + definition + result[match.end():]
    if result != original:
        generated.write_text(result, encoding="utf-8", newline="\n")
