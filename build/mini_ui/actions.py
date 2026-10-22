# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

from sources import forbidden

# SDK/CRT libraries are the only implicit linker inputs allowed by COFF directives.
# winmm supplies timers used by native task support, not a media implementation.
SYSTEM_LIBRARIES = {
    "kernel32", "user32", "gdi32", "advapi32", "shell32", "ole32", "oleaut32",
    "uuid", "comdlg32", "comctl32", "shlwapi", "version", "dbghelp", "setupapi",
    "usp10", "imm32", "dwrite", "windowscodecs", "bcrypt", "winmm", "rpcrt4",
    "psapi", "userenv", "propsys", "runtimeobject", "oldnames", "libcmt",
    "libcpmt", "libucrt", "libvcruntime", "ucrt", "vcruntime", "msvcprt",
    "mincore", "powrprof", "shcore", "ntdll",
}
# Windows SDK ucrt/wchar.h emits this x64 CPU-dispatch fallback even when
# clang does not enable AVX2. It is not a project/API compatibility substitute.
SDK_CRT_ALTERNATENAMES = {
    "_Avx2WmemEnabled=_Avx2WmemEnabledWeakValue",
}
# These real implementation/data TUs include only SDK/standard headers. Keep
# their empty checkout-header inventories; source/object/command hashes still apply.
SYSTEM_HEADERS_ONLY_SOURCES = {
    "base/third_party/superfasthash/superfasthash.c",
    "gen/third_party/blink/renderer/platform/character_property_data.cc",
}
FORBIDDEN_IMPORT_DLLS = re.compile(
    r"^(?:node|libnode|uv|libuv|libssl.*|libcrypto.*|libcurl.*|curl.*|"
    r"libegl.*|libgles.*|d3d.*|dxgi|dcomp|vulkan.*|dawn.*|tint.*|"
    r"winhttp|wininet|schannel|secur32|avcodec.*|avformat.*|"
    r"mf|mfplat|mfreadwrite|xaudio.*|dsound|openal.*)\.dll$", re.IGNORECASE)
FORBIDDEN_SYMBOLS = re.compile(
    r"(?:\bcurl_(?:easy|multi|global)_|\bSSL_(?:new|connect|read|write|CTX_)|"
    r"\buv_(?:loop|run|tcp|udp|pipe)_|\bnode::|\bnode_module_register\b|"
    r"\bv8::internal::Isolate|"
    r"\b(?:Dawn|dawn)::|\b(?:tint|angle)::|\bWebGL(?:2)?RenderingContext::|"
    r"\bGPUCanvasContext::|\bServiceWorkerGlobalScope::|\bAudioContext::)")
# These names are legitimate on the source-level QuickJS facade. Their defining
# object must come from the exact facade_sources declared by the backend owner.
# Match the defined function, not ScriptCompiler enum/data types in a caller's
# parameter or return types.
FACADE_APIS = re.compile(
    r"\bv8::(?:Isolate::(?:New|Initialize)\w*|V8::Initialize\w*|ScriptCompiler::\w+)\("
)


def record(path: Path, value: object) -> None:
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command: list[str], root: Path) -> str:
    process = subprocess.run(command, cwd=root, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = process.stdout.decode("utf-8", errors="replace")
    if process.returncode:
        sys.stdout.write(text)
        raise RuntimeError(f"Command failed ({process.returncode}): {subprocess.list2cmdline(command[:2])}")
    return text


def check_directives(text: str) -> list[str]:
    libraries = re.findall(r'(?i)[/-]defaultlib:(?:"([^"\r\n]+)"|([^\s]+))', text)
    result = []
    for quoted, bare in libraries:
        library = (quoted or bare).rstrip('"').lower()
        # Even a familiar basename must not smuggle a checkout-local .lib path.
        if "/" in library or "\\" in library or ":" in library:
            raise RuntimeError(f"Non-SDK default library in COFF directives: {library}")
        name = library.removesuffix(".lib")
        if name not in SYSTEM_LIBRARIES:
            raise RuntimeError(f"Undeclared library in COFF directives: {library}")
        result.append(library)
    for directive in re.finditer(
        r'(?i)[/-](wholearchive|alternatename|force)(?::(?:"([^"\r\n]*)"(?=\s|$)|([^\s]*))|(?=\s|$))',
        text,
    ):
        value = directive.group(2) or directive.group(3) or ""
        if directive.group(1).lower() != "alternatename" or value not in SDK_CRT_ALTERNATENAMES:
            raise RuntimeError("COFF directives attempt to add libraries or substitute unresolved symbols")
    return sorted(set(result))


def compile_source(job: dict, root: Path, output: Path) -> None:
    text = run(job["command"], root)
    includes = []
    for line in text.splitlines():
        match = re.match(r"^Note: including file:\s*(.*)$", line)
        if not match:
            continue
        path = Path(match.group(1)).resolve()
        if path.is_relative_to(root):
            relative = path.relative_to(root).as_posix()
            if reason := forbidden(relative, header=True):
                raise RuntimeError(f"{job['source']} includes {reason}: {relative}; fix the UI registration/binding source, not the linker")
            includes.append(relative)
    if job["kind"] in ("c", "cxx") and not includes and job["source"] not in SYSTEM_HEADERS_ONLY_SOURCES:
        raise RuntimeError(f"No checkout include dependencies were reported for {job['source']}; /showIncludes auditing cannot be skipped")
    record(output.with_suffix(".includes.json"), {"source": job["source"], "source_sha256": file_hash(root / job["source"]), "object_sha256": file_hash(output), "headers": sorted(set(includes)), "command_sha256": hashlib.sha256(json.dumps(job["command"]).encode()).hexdigest()})
    # Ninja's msvc dependency parser consumes the same actual compiler output.
    sys.stdout.write(text)


def archive(job: dict, root: Path, output: Path) -> None:
    provenance = job["object_sources"]
    if set(provenance) != set(job["objects"]):
        raise RuntimeError("UI archive has an incomplete object/source provenance inventory")
    for obj, source in provenance.items():
        if reason := forbidden(source):
            raise RuntimeError(f"UI archive includes {reason}: {source}")
        evidence = json.loads(Path(obj).with_suffix(".includes.json").read_text(encoding="utf-8"))
        if evidence["source"] != source or evidence["object_sha256"] != file_hash(Path(obj)):
            raise RuntimeError(f"UI object does not match its actual compiler provenance: {obj}")
    sys.stdout.write(run(job["command"], root))
    listing = run([job["tools"]["llvm-lib.exe"], "/nologo", "/list", str(output)], root)
    members = [line.strip().replace("\\", "/").rsplit("/", 1)[-1] for line in listing.splitlines() if line.strip()]
    expected = [Path(path).name for path in job["objects"]]
    if Counter(members) != Counter(expected) or any(not x.endswith(".obj") for x in members):
        raise RuntimeError("mini-ui.lib does not contain exactly the newly compiled object members (nested/import/legacy archives are forbidden)")
    details = run([job["tools"]["llvm-readobj.exe"], "--file-headers", "--coff-directives", str(output)], root)
    formats = re.findall(r"^Format:\s*(\S+)", details, re.MULTILINE)
    if len(formats) != len(expected) or any(value != "COFF-x86-64" for value in formats):
        raise RuntimeError("mini-ui.lib contains a non-x64 COFF object or import-library member")
    libraries = check_directives(details)
    symbols = run([job["tools"]["llvm-nm.exe"], "--print-file-name", "--defined-only", "--demangle", str(output)], root)
    if match := FORBIDDEN_SYMBOLS.search(symbols):
        raise RuntimeError(f"Forbidden runtime symbol in UI archive: {match.group(0)}")
    source_by_member = {Path(obj).name: source for obj, source in provenance.items()}
    facade_symbols = []
    for line in symbols.splitlines():
        if not FACADE_APIS.search(line):
            continue
        member = re.search(r"([0-9a-f]{20}\.obj)", line)
        source = source_by_member.get(member.group(1)) if member else None
        if source not in job["facade_sources"]:
            raise RuntimeError(f"V8-named JS API is not defined by the declared QuickJS facade: {line}")
        facade_symbols.append({"source": source, "member": member.group(1), "symbol": line})
    # COFF directives are audited before a consumer can accidentally link old libs.
    (output.parent / "archive-members.txt").write_text(listing, encoding="utf-8")
    (output.parent / "archive-coff.txt").write_text(details, encoding="utf-8")
    (output.parent / "archive-symbols.txt").write_text(symbols, encoding="utf-8")
    record(output.parent / "archive-audit.json", {"version": 1, "archive": str(output), "members": members, "object_sources": provenance, "quickjs_facade_symbols": facade_symbols, "default_libraries": libraries, "sha256": file_hash(output), "host_link_verified": False})


def link(job: dict, root: Path, output: Path) -> None:
    text = run(job["command"], root)
    sys.stdout.write(text)
    imports = run([job["tools"]["llvm-readobj.exe"], "--coff-imports", str(output)], root)
    dlls = sorted(set(re.findall(r"^\s*Name:\s*(\S+\.dll)\s*$", imports, re.MULTILINE | re.IGNORECASE)))
    if not dlls:
        raise RuntimeError("No PE import inventory was returned; dependency auditing cannot be skipped")
    banned = [dll for dll in dlls if FORBIDDEN_IMPORT_DLLS.match(dll)]
    # A socket transport must not be pulled in via OS libraries either.
    transport = re.findall(r"\b(?:WSAStartup|connect|send|recv|socket|WSASocket[AW]?|SSL_connect)\b", imports)
    if banned or transport:
        raise RuntimeError("Forbidden UI host import: " + ", ".join(banned + transport))
    map_file = output.with_suffix(".map")
    if not map_file.is_file():
        raise RuntimeError("UI host link map was not emitted")
    (output.parent / "host-imports.txt").write_text(imports, encoding="utf-8")
    record(output.parent / "host-link-audit.json", {"version": 1, "executable": str(output), "library": job["library"], "imports": dlls, "system_libraries": job["system_libraries"], "compiler_runtime_libraries": job["compiler_runtime_libraries"], "map": str(map_file), "sha256": file_hash(output), "host_link_verified": True, "runtime_smoke_verified": False})


def main() -> int:
    mode, filename = sys.argv[1:]
    job = json.loads(Path(filename).read_text(encoding="utf-8"))
    root = Path(job["root"]).resolve()
    output = Path(job["output"])
    try:
        {"compile": compile_source, "archive": archive, "link": link}[mode](job, root, output)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        output.unlink(missing_ok=True)
        print(f"UI {mode} failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
