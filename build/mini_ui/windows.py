# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

import argparse
from dataclasses import asdict
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

from .sources import (
    NS, PROJECTS, QUICKJS_SOURCES, UI_SOURCES, TRANSPORT_FREE_SOURCES,
    TYPE_ONLY_HEADERS, Source, baseline, forbidden,
)
import xml.etree.ElementTree as ET

SYSTEM_LIBRARIES = (
    "kernel32.lib", "user32.lib", "gdi32.lib", "advapi32.lib", "shell32.lib",
    "ole32.lib", "oleaut32.lib", "uuid.lib", "comdlg32.lib", "comctl32.lib",
    "shlwapi.lib", "version.lib", "dbghelp.lib", "setupapi.lib", "usp10.lib",
    "imm32.lib", "dwrite.lib", "windowscodecs.lib", "bcrypt.lib", "winmm.lib",
    "uxtheme.lib", "dwmapi.lib",
    "rpcrt4.lib", "psapi.lib", "userenv.lib", "propsys.lib", "runtimeobject.lib",
    "mincore.lib", "powrprof.lib", "shcore.lib", "ntdll.lib",
)
COMMON_DEFINES = (
    "WIN32", "NDEBUG", "OS_WIN", "_WINDOWS", "UNICODE", "_UNICODE", "NOMINMAX", "WIN32_LEAN_AND_MEAN",
    "_CRT_SECURE_NO_WARNINGS", "_SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING",
    "WINVER=0x0A00", "_WIN32_WINNT=0x0A00", "MINI_UI_IMPLEMENTATION=1",
    "MINI_UI_QUICKJS=1", "CONFIG_BIGNUM=1", "CPPGC_IS_STANDALONE", "U_STATIC_IMPLEMENTATION",
    "ICU_UTIL_DATA_IMPL=1", "SK_SUPPORT_GPU=0", "SKCMS_DISABLE_SKX", "SKCMS_DISABLE_HSW",
    # Match the existing Blink cppgc ABI, without enabling a V8 JavaScript heap.
    "CPPGC_CAGED_HEAP", "CPPGC_YOUNG_GENERATION", "CPPGC_POINTER_COMPRESSION",
    "CPPGC_ENABLE_LARGER_CAGE", "CPPGC_SLIM_WRITE_BARRIER",
)
DROP_DEFINE_PREFIXES = (
    "V8_", "MINI_ELECTRON_", "CPPGC_", "CURL", "BUILDING_LIBCURL", "USE_IPV6",
    "USE_WEBSOCKETS", "USE_SCHANNEL", "USE_WINSOCK", "USE_WINDOWS_SSPI",
    "USE_WIN32_LDAP", "USE_OPENSSL", "HAVE_LIBZ", "HAVE_ZLIB_H", "HAVE_BROTLI",
    "SK_GANESH", "SK_GRAPHITE", "SK_DIRECT3D", "SK_VULKAN", "SK_SUPPORT_GPU",
    "ANGLE", "DAWN", "TINT", "HAVE_INSPECTOR", "_USRDLL", "_HAS_CONSTEXPR",
    "ICU_UTIL_DATA_IMPL", "BITDEPTH=",
)
EXTRA_INCLUDE_DIRS = (
    "include", ".", "gen", "v8", "v8/include",
    "third_party/skia", "third_party/abseil-cpp", "third_party/icu/source/common",
    "third_party/icu/source/i18n", "third_party/harfbuzz-ng/src/src",
    "third_party/libxml/win32/include", "third_party/libxml/src/include",
    "third_party/libxslt/src",
    "net/third_party/quiche/overrides",
    "net/third_party/quiche/src/quiche/common/platform/default",
    "net/third_party/quiche/src",
    "base/allocator/partition_allocator/src", "gen/base/allocator/partition_allocator/src",
)
SOURCE_DEFINES = (
    ("mojo/core/", (
        "MOJO_SYSTEM_IMPL_IMPLEMENTATION", "IS_MOJO_CORE_EMBEDDER_IMPL",
        "IS_MOJO_CORE_EMBEDDER_FEATURES_IMPL", "IS_MOJO_CORE_PORTS_IMPL")),
    ("mojo/public/c/system/", ("MOJO_SYSTEM_IMPLEMENTATION",)),
    ("mojo/public/cpp/bindings/", (
        "IS_MOJO_CPP_BINDINGS_BASE_IMPL", "IS_MOJO_CPP_BINDINGS_IMPL")),
    ("mojo/public/cpp/system/", ("MOJO_CPP_SYSTEM_IMPLEMENTATION",)),
    ("mojo/public/cpp/platform/", ("IS_MOJO_CPP_PLATFORM_IMPL",)),
    ("mojo/public/cpp/base/", ("IS_MOJO_BASE_IMPL",)),
    ("crypto/random.cc", ("CRYPTO_IMPLEMENTATION",)),
    ("ipc/", ("IPC_IMPLEMENTATION", "IPC_MESSAGE_SUPPORT_IMPL")),
    ("third_party/abseil-cpp/", ("ABSL_ALLOCATOR_NOTHROW=1", "_ENABLE_EXTENDED_ALIGNED_STORAGE")),
)


def write_changed(path: Path, content: str) -> None:
    if path.is_file() and path.read_text(encoding="utf-8") == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


def write_json(path: Path, value: object) -> None:
    write_changed(path, json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def inside(root: Path, value: str) -> Path:
    path = (root / value).resolve()
    if not path.is_relative_to(root):
        raise RuntimeError(f"UI graph path escapes the checkout: {value}")
    return path


def find_llvm() -> Path:
    candidates = []
    if os.environ.get("LLVM_INSTALL_DIR"):
        candidates.append(Path(os.environ["LLVM_INSTALL_DIR"]) / "bin")
    found = shutil.which("clang-cl.exe")
    if found:
        candidates.append(Path(found).parent)
    candidates.append(Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM/bin")
    for directory in candidates:
        compiler = directory / "clang-cl.exe"
        if not compiler.is_file():
            continue
        version = subprocess.check_output([str(compiler), "--version"], text=True)
        if not re.search(r"clang version 22\.", version):
            continue
        required = ("clang.exe", "lld-link.exe", "llvm-lib.exe", "llvm-readobj.exe", "llvm-nm.exe")
        if all((directory / name).is_file() for name in required):
            return directory.resolve()
    raise RuntimeError("LLVM 22 with clang-cl, clang, lld-link, llvm-lib, llvm-readobj and llvm-nm is required; set LLVM_INSTALL_DIR")


def find_compiler_runtime(llvm: Path) -> Path:
    resource = Path(subprocess.check_output(
        [str(llvm / "clang.exe"), "--print-resource-dir"], text=True).strip())
    runtime = resource / "lib/windows/clang_rt.builtins-x86_64.lib"
    if not runtime.is_file():
        raise RuntimeError(f"LLVM x64 compiler runtime is required: {runtime}")
    return runtime.resolve()


def visual_studio_environment() -> dict[str, str]:
    env = os.environ.copy()
    env["VSLANG"] = "1033"
    # Reuse an existing x64 Native Tools prompt, otherwise discover VS locally.
    if env.get("VSCMD_ARG_TGT_ARCH") == "x64" and env.get("INCLUDE") and env.get("LIB"):
        return env
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    if not vswhere.is_file():
        raise RuntimeError("Visual Studio C++ x64 tools + Windows SDK are required (vswhere.exe missing)")
    installation = subprocess.check_output([
        str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
        "-property", "installationPath", "-utf8",
    ]).decode("utf-8-sig").strip()
    vcvars = Path(installation) / "VC/Auxiliary/Build/vcvarsall.bat"
    if not installation or not vcvars.is_file():
        raise RuntimeError("Visual Studio x64 C++ toolchain was not found")
    # /c consumes a CMD command string, not C-runtime escaped argv quoting.
    command = subprocess.list2cmdline([
        env.get("COMSPEC", "cmd.exe"), "/d", "/u", "/s", "/c",
    ]) + f' ""{vcvars}" x64 >nul && set"'
    result = subprocess.run(
        command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    for line in result.stdout.decode("utf-16-le").splitlines():
        key, separator, value = line.partition("=")
        if separator and key:
            env[key.upper()] = value
    env["VSLANG"] = "1033"
    if not env.get("INCLUDE") or not env.get("LIB"):
        raise RuntimeError("vcvarsall did not configure the Windows SDK")
    return env


def load_overlay(root: Path, filename: Path, sources: dict[str, Source], excluded: list[dict[str, str]]) -> dict:
    document = json.loads(filename.read_text(encoding="utf-8"))
    allowed = {"version", "add", "exclude", "replace", "include_dirs", "defines", "facade_sources"}
    if document.get("version") != 1 or set(document) - allowed:
        raise RuntimeError(f"Invalid UI source overlay schema: {filename}")
    for key in ("add", "exclude", "include_dirs", "defines", "facade_sources"):
        if not isinstance(document.get(key, []), list) or not all(isinstance(x, str) for x in document.get(key, [])):
            raise RuntimeError(f"{filename}: {key} must be an array of strings")
    replacements = document.get("replace", {})
    if not isinstance(replacements, dict) or not all(isinstance(k, str) and isinstance(v, str) for k, v in replacements.items()):
        raise RuntimeError(f"{filename}: replace must map source paths to source paths")
    for old in [*document.get("exclude", []), *replacements]:
        old = inside(root, old).relative_to(root).as_posix()
        sources.pop(old, None)
        excluded.append({"source": old, "project": "overlay", "reason": "UI binding/initialization cutover"})
    for path in [*document.get("add", []), *replacements.values()]:
        path = inside(root, path).relative_to(root).as_posix()
        if Path(path).suffix not in (".cc", ".cpp", ".c", ".S", ".asm"):
            raise RuntimeError(f"Only source files, never prebuilt objects/libraries, may enter the UI overlay: {path}")
        if reason := forbidden(path):
            raise RuntimeError(f"UI overlay attempts to restore {reason}: {path}")
        kind = {".c": "c", ".S": "gas", ".asm": "masm"}.get(Path(path).suffix, "cxx")
        sources.setdefault(path, Source(path, "overlay", kind))
    for path in document.get("include_dirs", []):
        relative = inside(root, path).relative_to(root).as_posix() + "/"
        if forbidden(relative, header=True):
            raise RuntimeError(f"Forbidden UI overlay include directory: {path}")
    for define in document.get("defines", []):
        key = define.partition("=")[0]
        invariant = next((value for value in COMMON_DEFINES if value.partition("=")[0] == key), None)
        if invariant is not None and define != invariant:
            raise RuntimeError(f"UI overlay changes a shared ABI invariant: {define} (required {invariant})")
        if define.startswith(DROP_DEFINE_PREFIXES) or any(char in define for char in "\r\n"):
            raise RuntimeError(f"Forbidden UI overlay define: {define}")
    return document


def expand_includes(root: Path, value: str) -> list[str]:
    result = []
    for item in value.split(";"):
        if not item or item.startswith("%("):
            continue
        # The legacy solution lives one directory below the repository root.
        item = item.replace("\\", "/").replace("$(SolutionDir)../", "").replace("$(SolutionDir)..", "")
        if "$(" in item or "%(" in item:
            raise RuntimeError(f"Unresolved UI compile include property: {item}")
        path = inside(root, item)
        relative = path.relative_to(root).as_posix()
        if not forbidden(relative + "/", header=True) and relative not in ("gpu", "third_party/khronos", "third_party/quickjs"):
            result.append(str(path))
    return result


def compile_flags(root: Path, generated: Path, source: Source, overlays: list[dict]) -> list[str]:
    inherited = source.metadata.get("PreprocessorDefinitions", "").split(";")
    overridden = {value.split("=", 1)[0] for value in COMMON_DEFINES}
    defines = [x for x in inherited if x and not x.startswith(("%(", *DROP_DEFINE_PREFIXES))
               and x.split("=", 1)[0] not in overridden]
    defines.extend(COMMON_DEFINES)
    for prefix, values in SOURCE_DEFINES:
        if source.path.startswith(prefix):
            defines.extend(values)
    if source.path.endswith(".mojom-blink.cc"):
        defines.append("BLINK_MOJO_IMPL=1")
    if source.path.startswith("third_party/dav1d/") and "_tmpl" in Path(source.path).stem and "/bitdepth16/" not in source.path:
        defines.append("BITDEPTH=8")
    if (source.project in ("blink", "generated_bindings")
            or source.path.startswith(("runtime/ui/", "third_party/blink/", "gen/third_party/blink/"))
            or "/generated/third_party/blink/" in source.path):
        defines.append("INSIDE_BLINK=1")
    if source.path.startswith("v8/"):
        defines += ["V8_TARGET_OS_WIN", "V8_HAVE_TARGET_OS", "V8_TARGET_ARCH_X64", "_CRT_RAND_S"]
    includes = [str(generated)]
    # A source-level QuickJS facade must win over upstream V8 JS headers.
    for overlay in overlays:
        includes.extend(str(inside(root, x)) for x in overlay.get("include_dirs", []))
    includes += expand_includes(root, source.metadata.get("AdditionalIncludeDirectories", ""))
    includes += [str(root / x) for x in EXTRA_INCLUDE_DIRS]
    for overlay in overlays:
        defines.extend(overlay.get("defines", []))
    # Do not inherit Link/Lib directives, PCH, /GL or Directory.Build.targets.
    optimization = {"MinSpace": "/O1", "MaxSpeed": "/O2", "Disabled": "/Od"}.get(source.metadata.get("Optimization"), "/O1")
    # The bytecode interpreter benefits from speed optimization independently
    # of the native facade; do not widen this to every runtime translation unit.
    if source.path == "third_party/quickjs/quickjs.c":
        optimization = "/O2"
    flags = ["/nologo", "/c", "/MT", optimization, "/Ob2", "/Gy", "/Gw", "/GF", "/Oy-", "/Zc:__cplusplus", "/bigobj", "/utf-8", "-mno-avx"]
    if source.kind == "c":
        flags += ["/TC", "/clang:-std=gnu11"]
    else:
        flags += ["/TP", "/std:c++20", "/GR-"]
        if source.path.startswith("runtime/ui/") or source.project == "host":
            flags.append("/EHsc")
        else:
            flags += ["/EHs-c-", "/clang:-fno-exceptions"]
    flags += ["/D" + x for x in dict.fromkeys(defines)]
    flags += ["/I" + x for x in dict.fromkeys(includes)]
    options = source.metadata.get("AdditionalOptions", "").replace("%(AdditionalOptions)", "").strip()
    if options not in ("", "-mno-avx", "/bigobj"):
        raise RuntimeError(f"Untranslated UI baseline compiler options for {source.path}: {options}")
    return flags


def embed_icu(root: Path, output: Path) -> Source:
    data = root / "third_party/icu/common/icudtl.dat"
    header = root / "third_party/icu/source/common/unicode/uvernum.h"
    version = re.search(r"#define\s+U_ICU_VERSION_MAJOR_NUM\s+(\d+)", header.read_text(encoding="utf-8-sig"))
    if version is None or not data.is_file():
        raise RuntimeError("Matching ICU common data and unicode/uvernum.h are required")
    # Assemble the real ICU data into COFF .rdata; never use ICU's bootstrap stubdata.
    # The symbol spelling follows U_ICUDATA_ENTRY_POINT in the checked-in ICU headers.
    symbol = f"icudt{version.group(1)}_dat"
    assembly = output / "generated/mini_ui/icu_data.S"
    filename = json.dumps(data.as_posix(), ensure_ascii=False)
    write_changed(assembly, f'.section .rdata,"dr"\n.balign 16\n.globl {symbol}\n{symbol}:\n.incbin {filename}\n')
    return Source(assembly.relative_to(root).as_posix(), "icu_data", "gas")


def ninja_path(value: str | Path) -> str:
    return str(value).replace("\\", "/").replace("$", "$$").replace(" ", "$ ").replace(":", "$:")


def generate_graph(root: Path, output: Path, llvm: Path, env: dict[str, str]) -> None:
    generated = output / "generated"
    generator = root / "runtime/ui/generate_bindings.py"
    overlay_path = root / "runtime/ui/build_sources.json"
    mojo_overlay = root / "runtime/ui/mojo_sources.json"
    mojom_generator = root / "runtime/ui/generate_mojom.py"
    css_generator = root / "runtime/ui/generate_css.py"
    profile_generators = (
        (root / "runtime/ui/generate_probes.py", "mini_ui_probe_sources.json"),
        (root / "runtime/ui/generate_feature_state.py", "mini_ui_feature_state_sources.json"),
        (root / "runtime/ui/generate_resources.py", "mini_ui_resource_sources.json"),
    )
    missing = [str(path.relative_to(root)) for path in
               (generator, mojom_generator, css_generator, overlay_path, mojo_overlay,
                *(script for script, _ in profile_generators)) if not path.is_file()]
    if missing:
        raise RuntimeError("UI integration prerequisite missing: " + ", ".join(missing))
    subprocess.run([sys.executable, str(generator), "--root", str(root), "--out", str(generated)], cwd=root, env=env, check=True)
    generated_overlay = generated / "mini_ui_sources.json"
    if not generated_overlay.is_file():
        raise RuntimeError(f"UI binding generator did not write {generated_overlay}")
    generated_profiles = []
    for script, manifest in profile_generators:
        subprocess.run([sys.executable, str(script), "--root", str(root), "--out", str(generated)],
                       cwd=root, env=env, check=True)
        generated_profile = generated / manifest
        if not generated_profile.is_file():
            raise RuntimeError(f"UI profile generator did not write {generated_profile}")
        generated_profiles.append(generated_profile)
    write_changed(generated / "ipc/ipc_buildflags.h",
                  "#ifndef IPC_IPC_BUILDFLAGS_H_\n#define IPC_IPC_BUILDFLAGS_H_\n"
                  "#include \"build/buildflag.h\"\n"
                  "#define BUILDFLAG_INTERNAL_IPC_MESSAGE_LOG_ENABLED() (0)\n"
                  "#endif\n")
    write_changed(generated / "mojo/buildflags.h",
                  "#ifndef MOJO_BUILDFLAGS_H_\n#define MOJO_BUILDFLAGS_H_\n"
                  "#include \"build/buildflag.h\"\n"
                  "#define BUILDFLAG_INTERNAL_MOJO_USE_APPLE_CHANNEL() (0)\n"
                  "#define BUILDFLAG_INTERNAL_MOJO_SUPPORT_LEGACY_CORE() (1)\n"
                  "#endif\n")
    sources, excluded = baseline(root)
    # Parse only CPU SkSL entries. Never import the supplement's GPU flags/targets.
    skia_tree = ET.parse(root / "build/windows/skia_gpu_sources.props")
    skia_metadata = next(item.metadata for item in sources.values() if item.project == "skia")
    for element in skia_tree.findall(".//m:ClCompile[@Include]", NS):
        path = element.attrib["Include"].replace("\\", "/").replace("$(SolutionDir)../", "")
        if path.startswith("third_party/skia/src/sksl/") and ("/codegen/" not in path or "/SkSLRasterPipeline" in path):
            sources[path] = Source(path, "skia", metadata=skia_metadata.copy())
        elif path in ("third_party/skia/src/core/SkRuntimeEffect.cpp", "third_party/skia/src/core/SkRuntimeBlender.cpp"):
            sources[path] = Source(path, "skia", metadata=skia_metadata.copy())
    seed_overlay_paths = (generated_overlay, overlay_path, mojo_overlay)
    overlays = [load_overlay(root, path, sources, excluded) for path in seed_overlay_paths]
    mojo_selection = generated / "mini_ui_mojo_selection.json"
    write_changed(mojo_selection, json.dumps({
        "version": 1,
        "sources": [{"path": path} for path in sorted(sources)],
    }, indent=2, sort_keys=True) + "\n")
    subprocess.run([sys.executable, str(mojom_generator), "--root", str(root), "--out", str(generated),
                    "--source-closure", str(mojo_selection)], cwd=root, env=env, check=True)
    generated_mojo_overlay = generated / "mini_ui_mojo_sources.json"
    if not generated_mojo_overlay.is_file():
        raise RuntimeError(f"Mojo binding generator did not write {generated_mojo_overlay}")
    overlays.append(load_overlay(root, generated_mojo_overlay, sources, excluded))
    overlays.extend(load_overlay(root, path, sources, excluded) for path in generated_profiles)
    overlay_paths = (*seed_overlay_paths, generated_mojo_overlay, *generated_profiles)
    css_selection = generated / "mini_ui_css_selection.json"
    write_changed(css_selection, json.dumps({
        "version": 1,
        "sources": [{"path": path} for path in sorted(sources) if path.startswith("gen/third_party/blink/renderer/core/")],
    }, indent=2, sort_keys=True) + "\n")
    subprocess.run([sys.executable, str(css_generator), "--root", str(root), "--out", str(generated),
                    "--source-closure", str(css_selection)], cwd=root, env=env, check=True)
    generated_css_overlay = generated / "mini_ui_css_sources.json"
    if not generated_css_overlay.is_file():
        raise RuntimeError(f"CSS database generator did not write {generated_css_overlay}")
    overlays.append(load_overlay(root, generated_css_overlay, sources, excluded))
    facade_sources = sorted({
        inside(root, path).relative_to(root).as_posix()
        for overlay in overlays for path in overlay.get("facade_sources", [])
    })
    for path in facade_sources:
        if path not in sources or not path.startswith("runtime/ui/"):
            raise RuntimeError(f"QuickJS facade provenance must name a compiled runtime/ui source: {path}")
    registration_roots = (
        "gen/third_party/blink/renderer/bindings/modules/v8/init_idl_interfaces.cc",
        "gen/third_party/blink/renderer/bindings/modules/v8/properties_per_feature_installer.cc",
        "gen/third_party/blink/renderer/core/html_element_factory.cc",
        "gen/third_party/blink/renderer/core/event_factory.cc",
    )
    stale = [path for path in registration_roots if path in sources]
    if stale:
        raise RuntimeError("UI binding generator must replace or remove old registration aggregates: " + ", ".join(stale))
    icu = embed_icu(root, output)
    sources[icu.path] = icu
    missing = [path for path in sources if not inside(root, path).is_file()]
    required = [path for path in UI_SOURCES + QUICKJS_SOURCES if path not in sources]
    if missing or required:
        raise RuntimeError("UI source prerequisites missing: " + ", ".join(missing + required))
    host = root / "examples/ui/main.cc"
    if not host.is_file():
        raise RuntimeError("UI host prerequisite missing: examples/ui/main.cc")
    tools = {name: str(llvm / name) for name in ("clang-cl.exe", "clang.exe", "llvm-lib.exe", "lld-link.exe", "llvm-readobj.exe", "llvm-nm.exe")}
    compiler_runtime = find_compiler_runtime(llvm)
    # A newly generated header can shadow a legacy gen/ header without changing
    # the old depfile. Invalidate consumers when that include-search set changes.
    header_inventory = generated / "header-search-inputs.txt"
    write_changed(header_inventory, "\n".join(sorted(
        path.relative_to(generated).as_posix()
        for path in generated.rglob("*.h")
    )) + "\n")
    for name in ("ml64.exe", "nasm.exe"):
        found = shutil.which(name, path=env.get("PATH"))
        if not found and name == "nasm.exe":
            cached = root / ".build-tools/nasm-2.16.03-windows-x64/nasm-2.16.03/nasm.exe"
            if cached.is_file():
                found = str(cached)
        if not found:
            raise RuntimeError(f"{name} is required for x64 cppgc/image codecs; install locally (no dependency bootstrap is performed)")
        tools[name] = found
    action = root / "build/mini_ui/actions.py"
    policy = root / "build/mini_ui/sources.py"
    action_command = subprocess.list2cmdline([sys.executable, str(action)]).replace("$", "$$")
    lines = ["ninja_required_version = 1.10", f"builddir = {ninja_path(output)}", "", "rule compile", f"  command = {action_command} compile $job", "  description = UI $source", "  deps = msvc", "  msvc_deps_prefix = Note: including file:", "", "rule action", f"  command = {action_command} $mode $job", "  description = UI $mode $out", "", "pool link_pool", "  depth = 1", ""]
    objects = []
    inventory = []
    host_object = None
    # Encode SDK/toolset selection in command lines so Ninja invalidates objects
    # when configuration changes and does not depend on the launching shell.
    system_includes = [path for path in env["INCLUDE"].split(";") if path]
    system_libraries = [path for path in env["LIB"].split(";") if path]
    entries = [*sorted(sources.values(), key=lambda x: x.path), Source("examples/ui/main.cc", "host")]
    for source in entries:
        digest = hashlib.sha256(source.path.encode()).hexdigest()[:20]
        obj = output / "obj" / f"{digest}.obj"
        job_path = output / "jobs" / f"{digest}.json"
        obj.parent.mkdir(parents=True, exist_ok=True)
        path = inside(root, source.path)
        if source.kind in ("c", "cxx"):
            command = [tools["clang-cl.exe"], *compile_flags(root, generated, source, overlays),
                       *("/imsvc" + directory for directory in system_includes),
                       "/showIncludes", str(path), "/Fo" + str(obj)]
        elif source.kind == "gas":
            command = [tools["clang.exe"], "--target=x86_64-pc-windows-msvc", "-c", str(path), "-o", str(obj)]
        elif source.kind == "masm":
            command = [tools["ml64.exe"], "/nologo", "/c", "/Fo" + str(obj), str(path)]
        else:
            dav1d = root / "third_party/dav1d"
            command = [tools["nasm.exe"], "-fwin64", "-I" + str(dav1d / "config/win/x64") + "/", "-I" + str(dav1d / "libdav1d") + "/", "-I" + str(dav1d / "libdav1d/src") + "/", str(path), "-o", str(obj)]
        job = {"root": str(root), "source": source.path, "output": str(obj), "kind": source.kind, "command": command}
        write_json(job_path, job)
        implicit = [job_path, action, policy]
        if source.kind in ("c", "cxx"):
            implicit.append(header_inventory)
        if source.project == "icu_data":
            implicit.append(root / "third_party/icu/common/icudtl.dat")
        # NASM includes are not tracked by /showIncludes. Track all assembly inputs.
        if source.kind == "nasm":
            implicit.extend(sorted((root / "third_party/dav1d").rglob("*.asm")))
            implicit.append(root / "third_party/dav1d/config/win/x64/config.asm")
        rule = "compile" if source.kind in ("c", "cxx") else "action"
        lines += [f"build {ninja_path(obj)}: {rule} {ninja_path(path)} | " + " ".join(ninja_path(x) for x in implicit), f"  job = {subprocess.list2cmdline([str(job_path)]).replace('$', '$$')}", f"  source = {source.path}"]
        if rule == "action":
            lines.append("  mode = compile")
        if source.project == "host":
            host_object = str(obj)
        else:
            objects.append(str(obj))
        inventory.append({**asdict(source), "object": str(obj), "command": command})
    library = output / "mini-ui.lib"
    archive_job = output / "jobs/archive.json"
    archive_response = output / "archive.rsp"
    write_changed(archive_response, subprocess.list2cmdline(["/nologo", "/machine:x64", "/out:" + str(library), *objects]) + "\n")
    object_sources = {entry["object"]: entry["path"] for entry in inventory if entry["project"] != "host"}
    write_json(archive_job, {"root": str(root), "output": str(library), "objects": objects, "object_sources": object_sources, "facade_sources": facade_sources, "command": [tools["llvm-lib.exe"], "@" + str(archive_response)], "tools": tools})
    lines += [f"build {ninja_path(library)}: action " + " ".join(ninja_path(x) for x in objects) + " | " + " ".join(ninja_path(x) for x in (archive_job, archive_response, action, policy)), "  mode = archive", f"  job = {subprocess.list2cmdline([str(archive_job)]).replace('$', '$$')}", "  pool = link_pool", ""]
    executable = output / "mini-ui-example.exe"
    link_job = output / "jobs/link.json"
    link_response = output / "link.rsp"
    link_flags = ["/nologo", "/machine:x64", "/subsystem:windows", "/opt:ref", "/opt:icf",
                  "/incremental:no", "/map:" + str(output / "mini-ui-example.map"),
                  "/out:" + str(executable), host_object, str(library), str(compiler_runtime),
                  *("/libpath:" + directory for directory in system_libraries), *SYSTEM_LIBRARIES]
    write_changed(link_response, subprocess.list2cmdline(link_flags) + "\n")
    write_json(link_job, {"root": str(root), "output": str(executable), "library": str(library), "command": [tools["lld-link.exe"], "@" + str(link_response)], "tools": tools, "system_libraries": SYSTEM_LIBRARIES, "compiler_runtime_libraries": [str(compiler_runtime)]})
    lines += [f"build {ninja_path(executable)}: action {ninja_path(host_object)} {ninja_path(library)} | " + " ".join(ninja_path(x) for x in (link_job, link_response, action, policy, compiler_runtime)), "  mode = link", f"  job = {subprocess.list2cmdline([str(link_job)]).replace('$', '$$')}", "  pool = link_pool", "", f"build library: phony {ninja_path(library)}", f"build host: phony {ninja_path(executable)}", "default host", ""]
    write_json(output / "source-closure.json", {"version": 1, "baseline_projects": PROJECTS, "overlays": [str(path) for path in (*overlay_paths, generated_css_overlay)], "sources": inventory, "excluded": excluded, "archive_inputs": objects, "object_sources": object_sources, "facade_sources": facade_sources, "type_only_sources": sorted(path for path in sources if path in TRANSPORT_FREE_SOURCES), "type_only_header_allowlist": sorted(TYPE_ONLY_HEADERS), "system_libraries": SYSTEM_LIBRARIES, "compiler_runtime_libraries": [str(compiler_runtime)], "prebuilt_project_libraries": []})
    write_changed(output / "build.ninja", "\n".join(lines))


def build(root: Path, args: argparse.Namespace) -> int:
    if sys.platform != "win32":
        raise RuntimeError("The independent UI backend requires Windows x64")
    root = root.resolve()
    output = inside(root, args.out)
    relative = output.relative_to(root).parts
    if len(relative) < 2 or relative[0] != "out" or not relative[1].startswith("ui-"):
        raise RuntimeError("UI output must be beneath out/ui-*; legacy Electron output directories cannot be reused")
    output.mkdir(parents=True, exist_ok=True)
    env = visual_studio_environment()
    llvm = find_llvm()
    ninja = shutil.which("ninja.exe", path=env.get("PATH")) or shutil.which("ninja", path=env.get("PATH"))
    if not ninja:
        raise RuntimeError("Ninja 1.10+ is required on PATH; no dependency bootstrap is performed")
    generate_graph(root, output, llvm, env)
    if args.ui_configure_only:
        print(f"UI graph configured: {output / 'build.ninja'} (no library has been built)")
        return 0
    subprocess.run([ninja, "-f", str(output / "build.ninja"), "-j", str(args.jobs), args.ui_target], cwd=root, env=env, check=True)
    if args.run:
        if args.ui_target != "host":
            raise RuntimeError("--run requires --ui-target host")
        return subprocess.call([str(output / "mini-ui-example.exe")], cwd=root, env=env)
    print(f"UI {args.ui_target} built: {output}")
    return 0
