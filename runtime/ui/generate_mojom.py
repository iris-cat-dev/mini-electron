#!/usr/bin/env python3
# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.
"""Generate genuine Mojo bindings for the local UI feature profile."""
from __future__ import annotations

import argparse
from collections import OrderedDict
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any

CONTROL_MODULES = (
    "mojo/public/interfaces/bindings/interface_control_messages.mojom",
    "mojo/public/interfaces/bindings/native_struct.mojom",
    "mojo/public/interfaces/bindings/pipe_control_messages.mojom",
)
PROFILE_MODULES = (
    "third_party/blink/public/mojom/navigation/navigation_params.mojom",
    "third_party/blink/public/mojom/page/page.mojom",
    "third_party/blink/public/mojom/frame/frame.mojom",
    "third_party/blink/public/mojom/page/widget.mojom",
)
ENDPOINT_ROOT_MODULES = (
    "third_party/blink/public/mojom/blob/blob.mojom",
    "third_party/blink/public/mojom/blob/blob_registry.mojom",
    "third_party/blink/public/mojom/blob/file_backed_blob_factory.mojom",
    "third_party/blink/public/mojom/blob/blob_url_store.mojom",
    "third_party/blink/public/mojom/associated_interfaces/associated_interfaces.mojom",
    "third_party/blink/public/mojom/messaging/transferable_message.mojom",
    "third_party/blink/public/mojom/messaging/static_bitmap_image.mojom",
    "third_party/blink/public/mojom/context_menu/context_menu.mojom",
    "third_party/blink/public/mojom/frame/policy_container.mojom",
    "third_party/blink/public/mojom/loader/content_security_notifier.mojom",
    "third_party/blink/public/mojom/browser_interface_broker.mojom",
    "third_party/blink/public/mojom/input/pointer_lock_context.mojom",
    "third_party/blink/public/mojom/widget/platform_widget.mojom",
    "third_party/blink/public/mojom/annotation/annotation.mojom",
    "third_party/blink/public/mojom/clipboard/clipboard.mojom",
    "third_party/blink/public/mojom/close_watcher/close_listener.mojom",
    "third_party/blink/public/mojom/choosers/color_chooser.mojom",
    "third_party/blink/public/mojom/choosers/date_time_chooser.mojom",
    "third_party/blink/public/mojom/choosers/file_chooser.mojom",
    "third_party/blink/public/mojom/choosers/popup_menu.mojom",
    "third_party/blink/public/mojom/frame/find_in_page.mojom",
    "third_party/blink/public/mojom/frame/remote_frame.mojom",
    "third_party/blink/public/mojom/frame/back_forward_cache_controller.mojom",
    "third_party/blink/public/mojom/frame/reporting_observer.mojom",
    "third_party/blink/public/mojom/input/input_host.mojom",
    "third_party/blink/public/mojom/input/input_messages.mojom",
    "third_party/blink/public/mojom/input/input_event.mojom",
    "third_party/blink/public/mojom/page/display_cutout.mojom",
    "third_party/blink/public/mojom/page_state/page_state.mojom",
    "third_party/blink/public/mojom/device_posture/device_posture_provider.mojom",
    "third_party/blink/public/mojom/permissions/permission.mojom",
    "third_party/blink/public/mojom/renderer_preference_watcher.mojom",
    "third_party/blink/public/mojom/script/script_type.mojom",
    "third_party/blink/public/mojom/reporting/reporting.mojom",
    "services/network/public/mojom/url_loader.mojom",
    "services/network/public/mojom/url_loader_factory.mojom",
    "services/network/public/mojom/url_response_head.mojom",
    "ui/accessibility/ax_features.mojom",
)
ENDPOINT_PROFILE_MODULES = (
    "services/network/public/mojom/url_request.mojom",
    "services/network/public/mojom/url_response_head.mojom",
    "services/network/public/mojom/url_loader_completion_status.mojom",
    "services/network/public/mojom/network_param.mojom",
    "services/network/public/mojom/network_types.mojom",
    "third_party/blink/public/mojom/messaging/cloneable_message.mojom",
    "third_party/blink/public/mojom/widget/platform_widget.mojom",
    "services/network/public/mojom/parsed_headers.mojom",
    "third_party/blink/public/mojom/data_transfer/data_transfer.mojom",
    "third_party/blink/public/mojom/timing/resource_timing.mojom",
)
ENDPOINT_OBSERVER_IMPORTS = tuple(
    f"services/network/public/mojom/{name}.mojom" for name in (
        "cookie_access_observer", "cookie_manager", "trust_token_access_observer",
        "url_loader_network_service_observer", "devtools_observer",
        "accept_ch_frame_observer", "shared_dictionary_access_observer",
    )
)
UI_UNAVAILABLE_TYPEMAP_HEADERS = {
    "net/cert/cert_verify_result.h",
    *(module + ".h" for module in ENDPOINT_OBSERVER_IMPORTS),
}
WINDOWS_MOJOM_FEATURES = ("is_win", "file_path_is_string16")
PROFILE_SUFFIXES = (".cc", "-shared.cc", "-blink.cc")
PRODUCTION_SUFFIXES = (".cc", "-shared.cc", "-blink.cc")
PROFILE_VERSION = 14
NORMAL_EXPORT = (
    "--export_attribute", "BLINK_COMMON_EXPORT",
    "--export_header", "third_party/blink/public/common/common_export.h",
)
BLINK_EXPORT = (
    "--export_attribute", "CORE_EXPORT",
    "--export_header", "third_party/blink/renderer/core/core_export.h",
)
BLOB_BLINK_EXPORT = (
    "--export_attribute", "PLATFORM_EXPORT",
    "--export_header", "third_party/blink/renderer/platform/platform_export.h",
)
NETWORK_BLINK_EXPORT = (
    "--export_attribute", "BLINK_PLATFORM_EXPORT",
    "--export_header", "third_party/blink/public/platform/web_common.h",
)

def write_changed(path: Path, content: bytes) -> None:
    if path.is_file() and path.read_bytes() == content:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def configure_mojom_imports(tools: Path) -> None:
    for path in (tools / "mojom", tools / "bindings"):
        value = str(path)
        if value not in sys.path:
            sys.path.insert(0, value)


def _remove_named(items: list[Any], node_type: type[Any], names: set[str],
                  context: str, required: set[str] | None = None) -> None:
    found = {
        item.mojom_name.name for item in items
        if isinstance(item, node_type) and item.mojom_name.name in names
    }
    missing = (required or set()) - found
    if missing:
        raise RuntimeError(f"{context}: expected declarations missing: {sorted(missing)}")
    items[:] = [
        item for item in items
        if not (isinstance(item, node_type) and item.mojom_name.name in names)
    ]


def _definition(tree: Any, node_type: type[Any], name: str, context: str) -> Any:
    matches = [
        item for item in tree.definition_list
        if isinstance(item, node_type) and item.mojom_name.name == name
    ]
    if len(matches) != 1:
        raise RuntimeError(f"{context}: expected exactly one {name}, found {len(matches)}")
    return matches[0]


def _remove_import(tree: Any, filename: str, context: str) -> None:
    matches = [item for item in tree.import_list if item.import_filename == filename]
    if len(matches) != 1:
        raise RuntimeError(
            f"{context}: expected exactly one import of {filename}, found {len(matches)}")
    tree.import_list.items[:] = [
        item for item in tree.import_list if item.import_filename != filename
    ]


def apply_ui_profile(module_path: str, tree: Any) -> None:
    """Remove the unavailable feature family from the parsed official AST."""
    from mojom.parse import ast

    if module_path == ENDPOINT_PROFILE_MODULES[0]:
        trusted = _definition(tree, ast.Struct, "TrustedUrlRequestParams", module_path)
        observers = {
            "cookie_observer", "trust_token_observer", "url_loader_network_observer",
            "devtools_observer", "accept_ch_frame_observer", "shared_dictionary_observer",
        }
        _remove_named(
            trusted.body.items, ast.StructField, observers,
            f"{module_path}:TrustedUrlRequestParams", required=observers)
        for imported in ENDPOINT_OBSERVER_IMPORTS:
            _remove_import(tree, imported, module_path)
        return

    if module_path in ENDPOINT_PROFILE_MODULES[1:3]:
        name = ("URLResponseHead" if module_path == ENDPOINT_PROFILE_MODULES[1]
                else "URLLoaderCompletionStatus")
        value = _definition(tree, ast.Struct, name, module_path)
        _remove_named(
            value.body.items, ast.StructField, {"ssl_info"},
            f"{module_path}:{name}", required={"ssl_info"})
        if module_path == ENDPOINT_PROFILE_MODULES[1]:
            _remove_import(
                tree, "services/proxy_resolver/public/mojom/proxy_resolver.mojom",
                module_path)
        return

    if module_path == ENDPOINT_PROFILE_MODULES[3]:
        tls_types = {
            "SSLInfo", "CertVerifyResult", "X509Certificate",
            "SSLCertRequestInfo", "AuthCredentials",
        }
        _remove_named(
            tree.definition_list, ast.Struct, tls_types, module_path,
            required=tls_types)
        return

    if module_path == ENDPOINT_PROFILE_MODULES[4]:
        unused_enums = {"EffectiveConnectionType", "CTPolicyCompliance"}
        _remove_named(
            tree.definition_list, ast.Enum, unused_enums, module_path,
            required=unused_enums)
        return
    if module_path == ENDPOINT_PROFILE_MODULES[5]:
        message = _definition(tree, ast.Struct, "CloneableMessage", module_path)
        _remove_named(
            message.body.items, ast.StructField, {"file_system_access_tokens"},
            f"{module_path}:CloneableMessage",
            required={"file_system_access_tokens"})
        _remove_import(
            tree,
            "third_party/blink/public/mojom/file_system_access/file_system_access_transfer_token.mojom",
            module_path)
        return


    if module_path == ENDPOINT_PROFILE_MODULES[6]:
        _remove_named(
            tree.definition_list, ast.Interface,
            {"WidgetCompositor", "Widget", "RenderInputRouterClient"},
            module_path,
            required={"WidgetCompositor", "Widget", "RenderInputRouterClient"})
        host = _definition(tree, ast.Interface, "WidgetHost", module_path)
        hardware_methods = {
            "CreateFrameSink", "RegisterRenderFrameMetadataObserver",
        }
        _remove_named(
            host.body.items, ast.Method, hardware_methods,
            f"{module_path}:WidgetHost", required=hardware_methods)
        for imported in (
            "cc/mojom/render_frame_metadata.mojom",
            "services/viz/public/mojom/compositing/compositor_frame_sink.mojom",
            "third_party/blink/public/mojom/input/input_handler.mojom",
            "third_party/blink/public/mojom/widget/visual_properties.mojom",
            "third_party/blink/public/mojom/widget/record_content_to_visible_time_request.mojom",
            "services/viz/public/mojom/hit_test/input_target_client.mojom",
            "ui/base/mojom/menu_source_type.mojom",
            "mojo/public/mojom/base/time.mojom",
        ):
            _remove_import(tree, imported, module_path)
        return

    if module_path == ENDPOINT_PROFILE_MODULES[7]:
        parsed = _definition(tree, ast.Struct, "ParsedHeaders", module_path)
        browser_fields = {
            "accept_ch", "critical_ch",
            "client_hints_ignored_due_to_clear_site_data_header",
            "supports_loading_mode", "reporting_endpoints", "cookie_indices",
            "no_vary_search_with_parse_error", "observe_browsing_topics",
            "allow_cross_origin_event_reporting",
        }
        _remove_named(
            parsed.body.items, ast.StructField, browser_fields,
            f"{module_path}:ParsedHeaders", required=browser_fields)
        for imported in (
            "services/network/public/mojom/no_vary_search.mojom",
            "services/network/public/mojom/supports_loading_mode.mojom",
            "services/network/public/mojom/web_client_hints_types.mojom",
        ):
            _remove_import(tree, imported, module_path)
        return

    if module_path == ENDPOINT_PROFILE_MODULES[8]:
        file = _definition(tree, ast.Struct, "DataTransferFile", module_path)
        _remove_named(
            file.body.items, ast.StructField, {"file_system_access_token"},
            f"{module_path}:DataTransferFile",
            required={"file_system_access_token"})
        _remove_import(
            tree,
            "third_party/blink/public/mojom/file_system_access/file_system_access_data_transfer_token.mojom",
            module_path)
        return

    if module_path == ENDPOINT_PROFILE_MODULES[9]:
        timing = _definition(tree, ast.Struct, "ResourceTimingInfo", module_path)
        _remove_named(
            timing.body.items, ast.StructField, {"service_worker_router_info"},
            f"{module_path}:ResourceTimingInfo",
            required={"service_worker_router_info"})
        _remove_import(
            tree, "services/network/public/mojom/service_worker_router_info.mojom",
            module_path)
        return

    view_transition_import = (
        "third_party/blink/public/mojom/frame/view_transition_state.mojom")
    if module_path == PROFILE_MODULES[0]:
        commit = _definition(
            tree, ast.Struct, "CommitNavigationParams", module_path)
        _remove_named(
            commit.body.items, ast.StructField, {"view_transition_state"},
            f"{module_path}:CommitNavigationParams",
            required={"view_transition_state"})
        _remove_named(
            tree.definition_list, ast.Struct,
            {"PageSwapEventParams", "PageRevealEventParams"}, module_path,
            required={"PageSwapEventParams"})
        _remove_import(tree, view_transition_import, module_path)
        return

    if module_path == PROFILE_MODULES[1]:
        for struct_name in ("PageRestoreParams", "PrerenderPageActivationParams"):
            struct = _definition(tree, ast.Struct, struct_name, module_path)
            _remove_named(
                struct.body.items, ast.StructField, {"view_transition_state"},
                f"{module_path}:{struct_name}", required={"view_transition_state"})
        page_broadcast = _definition(tree, ast.Interface, "PageBroadcast", module_path)
        _remove_named(
            page_broadcast.body.items, ast.Method, {"SetPageAttributionSupport"},
            f"{module_path}:PageBroadcast", required={"SetPageAttributionSupport"})
        _remove_import(tree, view_transition_import, module_path)
        _remove_import(
            tree, "services/network/public/mojom/attribution.mojom", module_path)
        return

    if module_path == PROFILE_MODULES[2]:
        _remove_named(
            tree.definition_list, ast.Enum, {"ViewTransitionSameOriginOptIn"},
            module_path, required={"ViewTransitionSameOriginOptIn"})
        local_frame_host = _definition(
            tree, ast.Interface, "LocalFrameHost", module_path)
        _remove_named(
            local_frame_host.body.items, ast.Method,
            {"OnViewTransitionOptInChanged", "CreateNewPopupWidget"},
            f"{module_path}:LocalFrameHost",
            required={"OnViewTransitionOptInChanged", "CreateNewPopupWidget"})
        local_frame = _definition(tree, ast.Interface, "LocalFrame", module_path)
        local_frame_methods = {
            "SnapshotDocumentForViewTransition",
            "NotifyViewTransitionAbortedToOldDocument",
            "DispatchPageSwap",
            "DispatchPageReveal",
            "BindDevToolsAgent",
        }
        _remove_named(
            local_frame.body.items, ast.Method, local_frame_methods,
            f"{module_path}:LocalFrame",
            required={
                "SnapshotDocumentForViewTransition",
                "NotifyViewTransitionAbortedToOldDocument",
                "DispatchPageSwap",
                "BindDevToolsAgent",
            })
        local_main_frame = _definition(
            tree, ast.Interface, "LocalMainFrame", module_path)
        animation_methods = {"AnimateDoubleTapZoom", "ZoomToFindInPageRect"}
        _remove_named(
            local_main_frame.body.items, ast.Method, animation_methods,
            f"{module_path}:LocalMainFrame", required=animation_methods)
        _remove_import(tree, view_transition_import, module_path)
        for imported in (
            "third_party/blink/public/mojom/devtools/devtools_agent.mojom",
            "third_party/blink/public/mojom/devtools/inspector_issue.mojom",
        ):
            _remove_import(tree, imported, module_path)
        return

    if module_path == PROFILE_MODULES[3]:
        frame_widget = _definition(tree, ast.Interface, "FrameWidget", module_path)
        unavailable_methods = {
            "BindWidgetCompositor", "EnableDeviceEmulation", "DisableDeviceEmulation",
        }
        _remove_named(
            frame_widget.body.items, ast.Method, unavailable_methods,
            f"{module_path}:FrameWidget", required=unavailable_methods)
        frame_widget_host = _definition(
            tree, ast.Interface, "FrameWidgetHost", module_path)
        animation_methods = {
            "AnimateDoubleTapZoomInMainFrame", "ZoomToFindInPageRectInMainFrame",
        }
        _remove_named(
            frame_widget_host.body.items, ast.Method, animation_methods,
            f"{module_path}:FrameWidgetHost", required=animation_methods)
        _remove_import(
            tree, "third_party/blink/public/mojom/widget/device_emulation_params.mojom",
            module_path)
        return

    raise RuntimeError(f"No UI Mojo profile is defined for {module_path}")


def schema_source(root: Path, module_path: str) -> Path:
    for base in (root, root / "gen"):
        source = (base / module_path).resolve()
        source.relative_to(root)
        if source.is_file():
            return source
    raise RuntimeError(f"Missing Mojom import: {module_path}")


def load_asts(
        root: Path, tools: Path) -> tuple[dict[str, Any], list[str], list[str]]:
    """Parse the endpoint and profiled UI import closures with Chromium's parser."""
    configure_mojom_imports(tools)
    from mojom.parse import conditional_features
    from mojom.parse import parser as syntax_parser

    trees: dict[str, Any] = {}
    order: list[str] = []
    visiting: set[str] = set()

    def visit(module_path: str) -> None:
        if module_path in trees:
            return
        if module_path in visiting:
            raise RuntimeError(f"Circular Mojom import at {module_path}")
        source = schema_source(root, module_path)
        visiting.add(module_path)
        tree = syntax_parser.Parse(
            source.read_text(encoding="utf-8-sig"), str(source))
        conditional_features.RemoveDisabledDefinitions(
            tree, WINDOWS_MOJOM_FEATURES)
        if module_path in (*PROFILE_MODULES, *ENDPOINT_PROFILE_MODULES):
            apply_ui_profile(module_path, tree)
        trees[module_path] = tree
        for imported in tree.import_list:
            visit(imported.import_filename)
        visiting.remove(module_path)
        order.append(module_path)

    # The complete endpoint closure is emitted. Imported structs have their own
    # out-of-line validators and variant deserializers, so headers alone are not
    # a linkable endpoint implementation.
    for module_path in (*ENDPOINT_ROOT_MODULES, *PROFILE_MODULES):
        visit(module_path)
    endpoint_order = list(order)
    return trees, order, endpoint_order


def write_modules(staging: Path, trees: dict[str, Any], order: list[str],
                  emitted_modules: set[str]) -> None:
    """Translate selected ASTs to the official serialized module model."""
    from mojom.generate import translate

    modules: dict[str, Any] = {}
    for module_path in order:
        tree = trees[module_path]
        imports = {
            imported.import_filename: modules[imported.import_filename]
            for imported in tree.import_list
        }
        try:
            module = translate.OrderedModule(tree, module_path, imports)
        except Exception as error:
            raise RuntimeError(f"Cannot translate UI Mojom {module_path}: {error}") from error
        module.metadata = OrderedDict()
        modules[module_path] = module
    for module_path in order:
        if module_path not in emitted_modules:
            continue
        destination = staging / f"{module_path}-module"
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open("wb") as output:
            modules[module_path].Dump(output)


def typemap_configs(root: Path, blink: bool) -> tuple[Path, ...]:
    result = []
    for path in (root / "gen").rglob("*.typemap_config"):
        is_blink = path.name.endswith("_blink.typemap_config")
        if is_blink == blink:
            result.append(path)
    if blink:
        result.append(root / "runtime/ui/mojom_blink.typemap_config")
    return tuple(sorted(result))


def build_typemap(configs: tuple[Path, ...]) -> bytes:
    """Load checked-in GN metadata through Chromium's typemap converter."""
    from generate_type_mappings import LoadCppTypemapConfig

    mappings: dict[str, dict[str, Any]] = {}
    owners: dict[str, Path] = {}
    for path in configs:
        for name, mapping in LoadCppTypemapConfig(str(path)).items():
            if name.startswith("network.mojom."):
                mapping = dict(mapping)
                for key in ("public_headers", "traits_headers"):
                    mapping[key] = [
                        header for header in mapping.get(key, [])
                        if header not in UI_UNAVAILABLE_TYPEMAP_HEADERS
                    ]
            if name == "network.mojom.ConnectionInfo":
                for key in ("public_headers", "traits_headers"):
                    mapping[key] = [
                        ("net/http/http_connection_info.h"
                         if header == "net/http/http_response_info.h" else header)
                        for header in mapping.get(key, [])
                    ]
            if name == "network.mojom.HttpResponseHeaders":
                header = "net/http/http_response_headers.h"
                if header not in mapping["public_headers"]:
                    mapping["public_headers"].append(header)
            previous = mappings.get(name)
            if previous is not None and previous != mapping:
                raise RuntimeError(
                    f"Conflicting typemap metadata for {name}: "
                    f"{owners[name]} and {path}")
            mappings[name] = mapping
            owners[name] = path
    return (json.dumps({"c++": mappings}, indent=2, sort_keys=True) + "\n").encode(
        "utf-8")


def fingerprint(root: Path, schemas: tuple[str, ...],
                endpoint_modules: tuple[str, ...],
                configs: tuple[Path, ...], selected_sources: set[str]) -> str:
    files = {Path(__file__).resolve()}
    files.update(schema_source(root, name) for name in schemas)
    files.update(configs)
    tools = root / "mojo/public/tools"
    files.update((tools / "bindings").glob("*.py"))
    files.update((tools / "bindings/generators").glob("*.py"))
    files.update((tools / "bindings/generators/cpp_templates").glob("*.tmpl"))
    files.update((tools / "mojom").rglob("*.py"))
    digest = hashlib.sha256()
    profile = {
        "version": PROFILE_VERSION,
        "controls": CONTROL_MODULES,
        "profile_modules": PROFILE_MODULES,
        "endpoint_roots": ENDPOINT_ROOT_MODULES,
        "endpoint_profiles": ENDPOINT_PROFILE_MODULES,
        "features": WINDOWS_MOJOM_FEATURES,
        "normal_export": NORMAL_EXPORT,
        "blink_export": BLINK_EXPORT,
        "blob_blink_export": BLOB_BLINK_EXPORT,
        "network_blink_export": NETWORK_BLINK_EXPORT,
        "selected_profile_sources": sorted(
            path for path in selected_sources
            if any(path == f"gen/{module}{suffix}"
                   for module in PROFILE_MODULES for suffix in PROFILE_SUFFIXES)),
        "selected_endpoint_sources": sorted(
            path for path in selected_sources
            if any(path == f"gen/{module}{suffix}"
                   for module in endpoint_modules
                   for suffix in PRODUCTION_SUFFIXES)),
        "generator_passes": (
            "non_variant", "message_ids", "normal", "blink"),
    }
    digest.update(json.dumps(profile, sort_keys=True).encode("utf-8") + b"\0")
    for path in sorted(files):
        digest.update(path.relative_to(root).as_posix().encode("utf-8") + b"\0")
        digest.update(path.read_bytes())
    return digest.hexdigest()


def _owned_output(output: Path, relative: str) -> Path:
    path = (output / relative).resolve()
    path.relative_to(output)
    return path


def generate(root: Path, output: Path, selected_sources: set[str]) -> None:
    relative_output = output.relative_to(root).as_posix()
    output.mkdir(parents=True, exist_ok=True)
    tools = root / "mojo/public/tools"
    trees, order, endpoint_order = load_asts(root, tools)
    normal_configs = typemap_configs(root, blink=False)
    blink_configs = typemap_configs(root, blink=True)
    all_configs = tuple(sorted((*normal_configs, *blink_configs)))
    current_fingerprint = fingerprint(
        root, tuple((*CONTROL_MODULES, *order)), tuple(endpoint_order),
        all_configs, selected_sources)
    stamp_path = output / "mini_ui_mojo_stamp.json"
    previous = json.loads(
        stamp_path.read_text(encoding="utf-8")) if stamp_path.is_file() else {}
    previous_files = previous.get("files", {})
    if (previous.get("fingerprint") == current_fingerprint
            and isinstance(previous_files, dict) and previous_files
            and all(isinstance(name, str) and isinstance(digest, str)
                    and _owned_output(output, name).is_file()
                    and hashlib.sha256(
                        _owned_output(output, name).read_bytes()).hexdigest() == digest
                    for name, digest in previous_files.items())):
        return

    parser = tools / "mojom/mojom_parser.py"
    driver = tools / "bindings/mojom_bindings_generator.py"
    environment = dict(os.environ, PYTHONHASHSEED="0")
    generated_files: dict[str, str] = {}
    with tempfile.TemporaryDirectory(
            prefix="mini-ui-mojo-", dir=output.parent) as temporary:
        staging = Path(temporary)
        subprocess.run(
            [sys.executable, str(parser), "--input-root", str(root),
             "--output-root", str(staging), "--mojoms", *CONTROL_MODULES],
            cwd=root, env=environment, check=True)
        emitted_modules = set(endpoint_order)
        emitted_modules.update(PROFILE_MODULES)
        write_modules(staging, trees, order, emitted_modules)
        configure_mojom_imports(tools)
        from mojom.generate import template_expander
        from generators import mojom_cpp_generator

        cache = staging / "template_cache"
        cache.mkdir()
        template_expander.PrecompileTemplates(
            {"c++": mojom_cpp_generator}, str(cache))
        normal_typemap = staging / "normal.typemap"
        blink_typemap = staging / "blink.typemap"
        normal_typemap.write_bytes(build_typemap(normal_configs))
        blink_typemap.write_bytes(build_typemap(blink_configs))
        command = [
            sys.executable, str(driver), "-o", str(staging), "generate",
            "--depth", str(root), "--generators", "c++",
            "--bytecode_path", str(cache),
        ]

        def generate_cpp(module_paths: list[str],
                         normal_export: tuple[str, ...] = (),
                         blink_export: tuple[str, ...] = ()) -> None:
            # The C++ driver loads serialized modules by logical schema path;
            # generated schema inputs must not acquire an extra "gen/" prefix.
            inputs = [str(root / module) for module in module_paths]
            normal_options = (
                "--typemap", str(normal_typemap), *normal_export)
            for options in (
                    ("--generate_non_variant_code",),
                    ("--generate_non_variant_code", "--generate_message_ids"),
                    ()):
                subprocess.run(
                    [*command, *options, *normal_options, *inputs],
                    cwd=root, env=environment, check=True)
            subprocess.run(
                [*command, "--variant", "blink", "--for_blink",
                 "--typemap", str(blink_typemap), *blink_export, *inputs],
                cwd=root, env=environment, check=True)

        controls = [str(root / module) for module in CONTROL_MODULES]
        for options in (
                ("--generate_non_variant_code",),
                ("--generate_non_variant_code", "--generate_message_ids"),
                ()):
            subprocess.run(
                [*command, *options, *controls], cwd=root,
                env=environment, check=True)

        blob_modules = [
            module for module in endpoint_order
            if module.startswith("third_party/blink/public/mojom/blob/")]
        network_modules = [
            module for module in endpoint_order
            if module.startswith("services/network/public/mojom/")]
        support_modules = [
            module for module in endpoint_order
            if module not in set((*blob_modules, *network_modules, *PROFILE_MODULES))]
        generate_cpp(support_modules)
        generate_cpp(
            network_modules, blink_export=NETWORK_BLINK_EXPORT)
        generate_cpp(
            blob_modules, normal_export=NORMAL_EXPORT,
            blink_export=BLOB_BLINK_EXPORT)
        generate_cpp(
            list(PROFILE_MODULES), normal_export=NORMAL_EXPORT,
            blink_export=BLINK_EXPORT)

        for path in sorted(staging.rglob("*")):
            if not path.is_file() or path.suffix not in (".h", ".cc"):
                continue
            relative = path.relative_to(staging).as_posix()
            content = path.read_bytes()
            write_changed(output / relative, content)
            generated_files[relative] = hashlib.sha256(content).hexdigest()

    replacements = {
        f"gen/{module}{suffix}": f"{relative_output}/{module}{suffix}"
        for module in CONTROL_MODULES
        for suffix in (".cc", "-shared.cc")
    }
    from mojom.parse import ast

    # Native-only enum modules use their C++ traits and emit no codec symbols.
    endpoint_producer_modules = {
        module for module in endpoint_order
        if any(
            isinstance(definition, (ast.Interface, ast.Struct, ast.Union, ast.Feature))
            or (isinstance(definition, ast.Enum)
                and definition.enum_value_list is not None)
            for definition in trees[module].definition_list)
    }
    producer_additions = []
    empty_producer_sources = set()
    for module in dict.fromkeys((*PROFILE_MODULES, *endpoint_order)):
        if module not in PROFILE_MODULES and module not in endpoint_producer_modules:
            continue
        suffixes = PROFILE_SUFFIXES if module in PROFILE_MODULES else PRODUCTION_SUFFIXES
        for suffix in suffixes:
            source = f"gen/{module}{suffix}"
            generated = f"{relative_output}/{module}{suffix}"
            if b"// Includes removed due to no code being generated." in (
                    output / f"{module}{suffix}").read_bytes():
                empty_producer_sources.add(source)
                replacements.pop(source, None)
                continue
            if source in selected_sources:
                replacements[source] = generated
            else:
                producer_additions.append(generated)
    overlay = {
        "version": 1,
        "add": sorted(producer_additions),
        "exclude": sorted(empty_producer_sources | {
            f"gen/{module}{suffix}"
            for module in endpoint_order
            if module not in endpoint_producer_modules
            for suffix in PRODUCTION_SUFFIXES}),
        "replace": replacements,
        "include_dirs": [relative_output],
        "defines": [],
    }
    overlay_name = "mini_ui_mojo_sources.json"
    content = (json.dumps(overlay, indent=2, sort_keys=True) + "\n").encode(
        "utf-8")
    write_changed(output / overlay_name, content)
    generated_files[overlay_name] = hashlib.sha256(content).hexdigest()
    if isinstance(previous_files, dict):
        for obsolete in sorted(set(previous_files) - set(generated_files)):
            _owned_output(output, obsolete).unlink(missing_ok=True)
    stamp = {
        "fingerprint": current_fingerprint,
        "files": generated_files,
        "profile_version": PROFILE_VERSION,
    }
    write_changed(
        stamp_path,
        (json.dumps(stamp, indent=2, sort_keys=True) + "\n").encode("utf-8"))
    print(
        f"Generated {len(CONTROL_MODULES)} control protocols, "
        f"{len(endpoint_order)} local endpoint closure protocols, and "
        f"{len(PROFILE_MODULES)} profiled Blink protocols for the UI build")


def main() -> None:
    arguments = argparse.ArgumentParser()
    arguments.add_argument("--root", type=Path, required=True)
    arguments.add_argument("--out", type=Path, required=True)
    arguments.add_argument("--source-closure", type=Path, required=True)
    options = arguments.parse_args()
    root = options.root.resolve()
    output = options.out.resolve()
    output.relative_to(root)
    selection_path = options.source_closure.resolve()
    selection_path.relative_to(root)
    selection = json.loads(selection_path.read_text(encoding="utf-8"))
    if selection.get("version") != 1:
        raise RuntimeError(f"Unsupported UI source selection: {selection_path}")
    selected_sources = {entry["path"].replace("\\", "/")
                        for entry in selection["sources"]}
    generate(root, output, selected_sources)


if __name__ == "__main__":
    main()
