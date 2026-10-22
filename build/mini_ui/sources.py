# Copyright 2026 The mini-electron Authors
# Use of this source code is governed by the Apache-2.0 license.

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import re
import xml.etree.ElementTree as ET

NS = {"m": "http://schemas.microsoft.com/developer/msbuild/2003"}
PROJECTS = (
    "chromium", "engine", "blink", "generated_bindings", "support", "skia",
    "icu", "harfbuzz", "v8_core_1", "v8_core_2", "v8_core_3",
)
UI_SOURCES = tuple(f"runtime/ui/{name}.cc" for name in (
    "view", "local_resources", "script_runtime", "cppgc_platform", "native_heap",
    "modules_initializer", "bindings_initializer", "blink_backend",
))
QUICKJS_SOURCES = tuple(f"third_party/quickjs/{name}.c" for name in (
    "quickjs", "libregexp", "libunicode", "cutils", "libbf",
))
# Blink's local loader still exchanges these value types. None owns a socket,
# HTTP transaction, TLS session, URLLoaderFactory or persistent cookie store.
TRANSPORT_FREE_SOURCES = {
    "services/metrics/public/cpp/ukm_source_id.cc",  # Passive identity, no recorder.
    "services/network/public/cpp/data_element.cc",
    "services/network/public/cpp/resource_request.cc",
    "services/network/public/cpp/resource_request_body.cc",
    "services/network/public/cpp/url_loader_completion_status.cc",
    "services/network/public/cpp/request_destination.cc",
    "services/network/public/cpp/web_sandbox_flags.cc",
    "net/http/http_byte_range.cc", "net/http/http_connection_info.cc",
    "net/http/http_content_disposition.cc", "net/http/http_request_headers.cc",
    "net/http/http_response_headers.cc", "net/http/http_util.cc",
    "net/cookies/site_for_cookies.cc",
    "net/url_request/redirect_info.cc", "net/url_request/referrer_policy.cc",
    "net/socket/socket_tag.cc", "net/dns/public/resolve_error_info.cc",
    "net/log/net_log_source.cc", "services/network/public/cpp/optional_trust_token_params.cc",
    "services/network/public/cpp/cors/cors_error_status.cc",
    "services/network/public/cpp/cors/cors.cc",
    "services/network/public/cpp/cors/origin_access_entry.cc",
    "services/network/public/cpp/cors/origin_access_list.cc",
    "services/network/public/cpp/request_mode.cc",
    "services/network/public/cpp/header_util.cc",
    "services/network/public/cpp/url_request_param_mojom_traits.cc",
    "net/third_party/quiche/src/quiche/common/structured_headers.cc",
    "services/network/public/cpp/is_potentially_trustworthy.cc",
    "services/network/public/cpp/network_switches.cc",
    "services/network/public/cpp/constants.cc",
    "services/network/public/cpp/cross_origin_embedder_policy_parser.cc",
    "services/network/public/cpp/parsed_headers.cc",
    "services/network/public/cpp/avail_language_header_parser.cc",
    "services/network/public/cpp/content_language_parser.cc",
    "services/network/public/cpp/cross_origin_opener_policy_parser.cc",
    "services/network/public/cpp/document_isolation_policy_parser.cc",
    "services/network/public/cpp/link_header_parser.cc",
    "services/network/public/cpp/origin_agent_cluster_parser.cc",
    "services/network/public/cpp/timing_allow_origin_parser.cc",
    "services/network/public/cpp/x_frame_options_parser.cc",
    "components/link_header_util/link_header_util.cc",
    "gpu/command_buffer/common/mailbox.cc",
    "gpu/command_buffer/common/mailbox_holder.cc",
    "gpu/command_buffer/common/sync_token.cc",
    "gpu/ipc/common/mailbox_mojom_traits.cc",
    "services/network/public/cpp/content_security_policy/content_security_policy.cc",
    "services/network/public/cpp/content_security_policy/csp_source.cc",
    "services/network/public/cpp/content_security_policy/csp_source_list.cc",
    "services/network/public/cpp/content_security_policy/csp_context.cc",
}
# Wire-value conversion only: no CORS fetcher, cookie store or HTTP client.
TRANSPORT_FREE_SOURCES.update(f"services/network/public/cpp/{name}.cc" for name in (
    "alternate_protocol_usage_mojom_traits", "http_request_headers_mojom_traits",
    "cookie_setting_overrides_mojom_traits", "cors/cors_mojom_traits",
    "cross_origin_embedder_policy_mojom_traits", "cross_origin_opener_policy_mojom_traits",
    "cross_origin_embedder_policy", "cross_origin_opener_policy", "crash_keys",
    "document_isolation_policy_mojom_traits", "document_isolation_policy",
    "schemeful_site_mojom_traits", "cookie_manager_shared_mojom_traits",
    "ip_address_mojom_traits", "load_timing_info_mojom_traits",
    "parsed_request_cookie_mojom_traits", "storage_access_api_mojom_traits",
    "network_param_mojom_traits", "net_ipc_param_traits",
    "url_request_mojom_traits", "url_loader_completion_status_mojom_traits",
    "ip_endpoint_mojom_traits", "isolation_info_mojom_traits",
    "network_anonymization_key_mojom_traits", "client_hints",
    "network_isolation_key_mojom_traits",
))
TRANSPORT_FREE_SOURCES.update(f"net/base/{name}.cc" for name in (
    "address_family", "auth", "data_url", "features_net", "filename_util",
    "filename_util_icu", "filename_util_internal", "host_port_pair", "ip_address",
    "ip_endpoint", "isolation_info", "load_timing_info", "lookup_string_in_fixed_set",
    "mime_sniffer", "mime_util", "network_anonymization_key",
    "network_isolation_key", "net_errors", "net_string_util_icu", "parse_number",
    "platform_mime_util_win", "port_util", "proxy_chain", "proxy_server",
    "registry_controlled_domains/registry_controlled_domain", "schemeful_site",
    "url_util_net",
))
TYPE_ONLY_HEADERS = {path.removesuffix(".cc") + ".h" for path in TRANSPORT_FREE_SOURCES} | {
    "net/base/request_priority.h", "net/filter/source_stream.h",
    "net/storage_access_api/status.h",
    "net/cert/cert_status_flags.h", "net/cert/ct_policy_status.h",
    "net/cert/sct_status_flags.h",
    "net/http/http_version.h",
    "net/http/alternate_protocol_usage.h",  # Response protocol enum only.
    "net/http/http_status_code.h", "net/http/http_status_code_list.h",
    "net/cert/ocsp_revocation_status.h",
    "net/cert/cert_status_flags_list.h",
    "services/network/public/cpp/cross_origin_embedder_policy.h",
    "services/network/public/cpp/cross_origin_opener_policy.h",
    "services/network/public/cpp/network_features.h",
    "services/network/public/cpp/constants.h",
    "services/network/public/cpp/features.h",  # Inline CPU body-drain byte budget only.
    "services/network/public/cpp/parsed_headers.h",
    "services/network/public/cpp/network_ipc_param_traits.h",  # HTTP connection enum only in UI.
    "services/network/public/cpp/mutable_network_traffic_annotation_tag_mojom_traits.h",
    "net/http/structured_headers.h",  # Header-only RFC structured-field wrapper.
    "net/third_party/quiche/src/quiche/common/platform/api/quiche_export.h",
    "net/third_party/quiche/src/quiche/common/platform/api/quiche_logging.h",
    "net/third_party/quiche/overrides/quiche_platform_impl/quiche_export_impl.h",
    "net/third_party/quiche/overrides/quiche_platform_impl/quiche_logging_impl.h",
    "third_party/blink/renderer/modules/modules_initializer.h",
    "gin/public/gin_embedders.h",  # Embedder-slot enum only; no gin runtime.
    "gin/public/wrapper_info.h",  # Slot indices + embedder tag; no implementation.
    "gin/gin_export.h",  # Export macro used by the metadata declaration above.
    "third_party/blink/renderer/modules/modules_export.h",
    "gpu/command_buffer/common/mailbox.h",
    "gpu/command_buffer/common/mailbox_holder.h",  # Passive image-handle metadata.
    "gpu/command_buffer/common/command_buffer_id.h",  # Typed integer ID only.
    "gpu/ipc/common/vulkan_ycbcr_info.h",  # Numeric image-conversion metadata only.
    "gpu/ipc/common/mailbox_holder_mojom_traits.h",
    "gpu/ipc/common/sync_token_mojom_traits.h",
    "gpu/command_buffer/common/sync_token.h",
    "gpu/command_buffer/common/shared_image_usage.h",
    "gpu/command_buffer/common/constants.h",
    "third_party/blink/renderer/core/inspector/agent_registry.h",  # Traced COW observer list only.
}
TYPE_ONLY_HEADERS.update("third_party/blink/renderer/platform/widget/compositing/" + name for name in (
    "layer_tree_settings.h", "layer_tree_view.h", "layer_tree_view_delegate.h",
    "widget_compositor.h", "widget_swap_queue.h", "render_frame_metadata_observer_impl.h",
))
# Native diagnostic values/IDs are needed without a DevTools runtime.
NATIVE_DIAGNOSTIC_FILES = {
    f"third_party/blink/renderer/core/inspector/{name}{suffix}"
    for name in ("console_message", "console_message_storage", "identifiers_factory")
    for suffix in (".cc", ".h")
}
LEGACY_FAKE_SOURCES = {
    "base/power_monitor/power_monitor_empty.cc",
    "base/trace_event/trace_event_stub.cc",
    "base/win/dispatch_stub.cc",
    "cc/paint/skottie_wrapper_stub.cc",
    "mojo/public/cpp/mojo_empty.cc", "mojo/public/cpp/mojo_lite.cc",
    "mojo/public/cpp/mojo_lite_msg.cc",
    "third_party/blink/public/platform/modules/modules_mojo_empty.cc",
    "third_party/abseil-cpp/absl/base/internal/raw_logging_empty.cc",
    "third_party/abseil-cpp/absl/hash/internal/hash_absl.cc",
    "v8/src/libplatform/tracing/recorder-empty.cc",
    "v8/src/libplatform/default-platform-wrap.cc",
    "third_party/liburlpattern/url_pattern.cc",
    "third_party/liburlpattern/url_parse.cc",
    "third_party/liburlpattern/url_tokenize.cc",
    "third_party/liburlpattern/url_utils.cc",
}
# These are source/header implementation roots, not API type names. In particular,
# v8-platform.h and cppgc public headers remain valid native task/allocator APIs.
FORBIDDEN_ROOTS = (
    "runtime/electron/", "runtime/node/", "runtime/network/", "runtime/storage/", "runtime/engine/",
    "base/trace_event/trace_buffer.cc",
    "base/trace_event/trace_config_category_filter.cc",
    "base/trace_event/trace_event_memory_overhead.cc",
    "base/trace_event/trace_log_constants.cc",
    "base/trace_event/tracing_agent.cc",
    "third_party/node/", "third_party/nodejs/", "third_party/libnode/", "third_party/libuv/",
    "third_party/libcurl/", "third_party/libcurl76/", "third_party/curl/", "third_party/openssl/",
    "third_party/boringssl/", "third_party/angle/", "third_party/dawn/",
    "third_party/tint/", "third_party/webrtc/", "third_party/ffmpeg/",
    "third_party/leveldb/", "third_party/sqlite/", "media/",
    "third_party/libxml/src/nanohttp.c", "third_party/libxml/src/nanoftp.c",
    "third_party/skia/modules/skottie/src/layers/AudioLayer.",
    "net/third_party/quiche/",
    "third_party/blink/common/loader/throttling_url_loader.",
    "third_party/blink/common/loader/mime_sniffing_",
    "third_party/blink/common/loader/url_loader_factory_bundle",
    "third_party/blink/renderer/platform/loader/child_url_loader_factory_bundle.",
    "third_party/blink/renderer/platform/loader/tracked_child_url_loader_factory_bundle.",
    "net/socket/", "net/ssl/", "net/cert/", "net/dns/", "net/quic/",
    "net/http/", "net/ftp/", "net/websockets/", "net/url_request/",
    "services/network/", "services/audio/", "services/video_capture/",
    "storage/browser/", "gpu/command_buffer/", "gpu/ipc/", "ui/gl/",
    "ui/gfx/win/d3d_shared_fence.",
    "v8/src/api/", "v8/src/builtins/", "v8/src/codegen/", "v8/src/compiler/",
    "v8/src/execution/", "v8/src/interpreter/", "v8/src/maglev/",
    "v8/src/snapshot/", "v8/src/objects/", "v8/src/heap/cppgc-js/",
    "v8/src/wasm/", "gin/",
)
BLINK_REMOVED = (
    "core/html/media/", "core/html/track/", "core/html/fenced_frame/",
    "core/html/portal/", "core/html/permission/", "core/inspector/",
    "core/shadow_realm/", "core/view_transition/",
    "core/speculation_rules/", "core/loader/speculation_rule_loader.",
    "core/loader/resource/speculation_rules_resource.",
    "core/html/anchor_element_metrics.", "core/html/anchor_element_metrics_sender.",
    "core/html/anchor_element_viewport_position_tracker.",
    "core/loader/anchor_element_interaction_tracker.",
    "core/page/link_highlight.", "core/paint/link_highlight_impl.",
    "core/frame/remote_frame_view.",
    "core/css/css_view_transition_rule", "core/css/style_rule_view_transition",
    "core/style/style_view_transition_group", "core/style/style_view_transition_name",
    "core/layout/layout_view_transition_content", "core/layout/layout_view_transition_root",
    "core/paint/timing/first_meaningful_paint_detector",
    "core/paint/timing/paint_timing_callback_manager",
    "core/workers/", "core/workers", "core/websockets/", "core/xmlhttprequest/",
    "core/mojo/", "core/testing/", "core/layout/custom/", "core/animation/worklet/",
    "core/timing/measure_memory/", "core/timing/worker_performance",
    "core/timing/worker_global_scope_performance",
    "core/timing/profiler", "core/timing/internals_profiler",
    "core/timing/background_tracing_helper.",
    "core/loader/text_track_loader.",
    "core/css/font_face_set_worker.", "core/css/offscreen_font_selector.",
    "core/exported/web_v8_context_snapshot.", "core/exported/web_v8_features.",
    "core/frame/screen_metrics_emulator.",
    "bindings/modules/v8/webgl_any.", "bindings/modules/v8/v8_binding_for_modules.",
    "bindings/modules/v8/serialization/",
    "build/win/precompile.cc",
    "platform/audio/", "platform/media/", "platform/mediastream/",
    "platform/peerconnection/", "platform/graphics/gpu/", "platform/graphics/video",
    "platform/graphics/web_graphics_context", "platform/graphics/static_bitmap_image_to_video",
    "platform/graphics/gpu", "platform/graphics/accelerated_", "platform/webgl/",
    "platform/webrtc/", "platform/testing/", "platform/widget/compositing/",
    "platform/widget/input/widget_input_handler_manager.",
    "platform/widget/input/widget_input_handler_impl.",
    "platform/loader/fetch/url_loader/worker_main_script_loader.",
    "platform/loader/fetch/url_loader/navigation_body_loader.",
    "platform/disk_data_allocator.", "platform/disk_data_metadata.",
    "platform/win/precompile_platform.cc",
    "platform/exported/mediastream/", "platform/exported/web_audio_bus.",
    "platform/exported/web_media_", "platform/exported/web_encrypted_media_",
    "platform/exported/web_video_",
    "platform/exported/web_surface_layer_bridge.",
    "modules/exported/web_crypto_", "modules/exported/web_dom_file_system.",
    "modules/exported/web_dom_media_stream_", "modules/exported/web_embedded_worker_",
    "platform/graphics/begin_frame_provider.", "platform/graphics/image_to_buffer_copier.",
    "platform/graphics/mailbox_ref.",
    "platform/graphics/paint/foreign_layer_display_item.",
    "platform/graphics/compositing/paint_artifact_compositor.",
    "platform/graphics/compositing/property_tree_manager.",
    "platform/graphics/compositing/content_layer_client_impl.",
    "platform/graphics/compositing/pending_layer.",
    "platform/graphics/compositing/layers_as_json.",
    "platform/graphics/compositing/adjust_mask_layer_geometry.",
    "platform/instrumentation/resource_coordinator/",
    "modules/exported/web_service_worker_", "modules/exported/web_shared_worker_",
    "modules/exported/web_shared_storage_worklet_thread_impl.",
    "controller/",
)
REPLACED_INITIALIZERS = (
    "third_party/blink/renderer/modules/modules_initializer_lite.cc",
    "third_party/blink/renderer/bindings/modules/v8/module_bindings_initializer.cc",
)
REMOVED_BINDING_FILES = (
    "v8_initializer.cc", "v8_initializer_win.cc", "v8_context_snapshot.cc",
    "v8_context_snapshot_impl.cc", "v8_embedder_graph_builder.cc", "v8_gc_controller.cc",
    "v8_gc_for_context_dispose.cc", "v8_code_cache.cc", "script_cache_consumer.cc",
    "script_streamer.cc", "script_decoder.cc", "worker_or_worklet_script_controller.cc",
    "v8_wasm_response_extensions.cc", "thread_debugger.cc", "profiler_trace_builder.cc",
    "remote_window_proxy.cc", "v8_metrics.cc",
)
REMOVED_BINDING_TOKENS = (
    "audio", "video", "html_media", "media_element", "media_stream", "media_source",
    "media_capabilities", "media_session", "media_recorder", "media_devices",
    "media_key", "media_metadata", "webgl", "webgpu", "gpu_", "service_worker",
    "extendable_", "worker_", "worklet", "indexed_db", "idb_", "cache_storage",
    "file_system", "storage_", "sql_", "rtc_", "web_socket", "xml_http_request",
    "dev_tools", "mojo", "database", "encrypted", "speech_",
)


@dataclass
class Source:
    path: str
    project: str
    kind: str = "cxx"
    metadata: dict[str, str] = field(default_factory=dict)


def matches_condition(condition: str) -> bool:
    if not condition:
        return True
    expression = condition.replace("$(Configuration)", "Release").replace("$(Platform)", "x64")
    match = re.fullmatch(r"\s*'([^']*)'\s*(==|!=)\s*'([^']*)'\s*", expression)
    if not match:
        raise RuntimeError(f"Unsupported XML condition in UI source baseline: {condition}")
    left, op, right = match.groups()
    return (left == right) if op == "==" else (left != right)


def properties(element: ET.Element | None) -> dict[str, str]:
    result: dict[str, str] = {}
    if element is not None:
        for child in element:
            if matches_condition(child.get("Condition", "")):
                result[child.tag.rsplit("}", 1)[-1]] = (child.text or "").strip()
    return result


def forbidden(path: str, *, header: bool = False) -> str | None:
    path = path.replace("\\", "/").lower().lstrip("./")
    if path in TRANSPORT_FREE_SOURCES or path in NATIVE_DIAGNOSTIC_FILES:
        return None
    if header and (path in TYPE_ONLY_HEADERS or path.startswith("services/network/public/mojom/")):
        return None
    if path in LEGACY_FAKE_SOURCES:
        return "legacy fake symbol implementation"
    if not header and Path(path).name in REMOVED_BINDING_FILES:
        return "old V8 runtime integration"
    if path.startswith("third_party/quickjs/") and Path(path).name in (
        "miniv8api.h", "quickjs-libc.h",
    ):
        return "legacy QuickJS shell/fake V8 interface"
    if path.startswith("third_party/quickjs/") and Path(path).suffix in (".c", ".cc", ".cpp"):
        if path not in QUICKJS_SOURCES:
            return "QuickJS shell or legacy V8 emulation"
    if path.startswith((
        "third_party/blink/common/service_worker/",
        "third_party/blink/common/mediastream/",
        "third_party/blink/common/media/",
    )):
        return "removed Blink common feature"
    if path.startswith(FORBIDDEN_ROOTS):
        return "forbidden implementation dependency"
    if path.startswith("v8/src/") and not path.startswith((
        "v8/src/base/", "v8/src/libplatform/", "v8/src/heap/cppgc/",
        "v8/src/heap/base/", "v8/src/tracing/trace-event-no-perfetto.h",
    )):
        return "V8 JavaScript runtime implementation"
    if path.startswith("net/") and not header:
        return "network implementation outside the explicit value-type allowlist"
    prefix = "third_party/blink/renderer/"
    if path.startswith(prefix):
        relative = path[len(prefix):]
        if relative.startswith(BLINK_REMOVED):
            return "removed Blink implementation feature"
        if relative.startswith("modules/") and not relative.startswith((
            "modules/canvas/", "modules/exported/",
        )):
            return "removed Blink module"
    if not header and path in REPLACED_INITIALIZERS:
        return "replaced UI initialization"
    return None


def retain(path: str) -> tuple[bool, str]:
    reason = forbidden(path)
    if reason:
        return False, reason
    if path in TRANSPORT_FREE_SOURCES:
        return True, "transport-free request/URL/MIME value type"
    if path == "base/memory/shared_memory_tracker.cc":
        return True, "CPU shared-memory naming and accounting without a dump manager"
    if path == "base/win/atl_throw.cc":
        return False, "no retained UI consumer of the optional ATL adapter"
    name = Path(path).name
    if name == "character_property_data_generator.cc":
        return False, "build-time Unicode table generator"
    if any(part in path.split("/") for part in ("test", "tests", "testing", "test_support")) or re.search(r"(?:_test(?:_helpers?|_utils?)?|_unittest|_fuzzer|_perftest)\.", name):
        return False, "non-runtime source"
    if path.startswith("v8/"):
        if name in ("default-platform-wrap.cc", "recorder-empty.cc"):
            return False, "legacy embedding wrapper"
        if "/heap/base/asm/" in path:
            return False, "replaced with x64 MASM stack scanner"
        return path.startswith(("v8/src/base/", "v8/src/libplatform/", "v8/src/heap/base/", "v8/src/heap/cppgc/")), "standalone cppgc support"
    if name in REMOVED_BINDING_FILES or "compile_hints" in name:
        return False, "V8 runtime integration replaced by QuickJS"
    if path.startswith("gen/third_party/blink/renderer/bindings/"):
        if name in ("init_idl_interfaces.cc", "properties_per_feature_installer.cc"):
            return True, "binding aggregate; generator must replace"
        if any(token in name for token in REMOVED_BINDING_TOKENS):
            return False, "removed generated interface"
        return True, "retained standard DOM binding"
    if path.startswith("gen/third_party/blink/renderer/modules/"):
        return False, "module aggregates replaced by UI generator"
    if path.startswith("gen/third_party/blink/renderer/core/inspector/"):
        return False, "DevTools protocol"
    if path.startswith("third_party/skia/"):
        if name in ("SkImageGenerator_none.cpp", "SkImageGenerator_skia.cpp"):
            return False, "obsolete empty Skia port translation unit"
        if any(part in path for part in ("/src/gpu/", "/src/text/gpu/", "/skottie/", "/sksg/", "/skresources/")) or name == "SkWGL_win.cpp":
            return False, "Skia GPU or animation module"
        return True, "CPU Skia"
    if path.startswith("third_party/icu/source/stubdata/"):
        return False, "replaced by real embedded ICU data"
    if path.startswith("third_party/quickjs/"):
        return path in QUICKJS_SOURCES, "QuickJS C core only"
    if path.startswith("third_party/zlib/google/zip_"):
        return False, "archive filesystem utilities are outside local UI resources"
    if path.startswith("third_party/") and not path.startswith("third_party/blink/"):
        allowed = (
            "third_party/icu/", "third_party/harfbuzz-ng/", "third_party/abseil-cpp/",
            "third_party/brotli/", "third_party/zlib/", "third_party/libpng/",
            "third_party/libjpeg_turbo/", "third_party/iccjpeg/", "third_party/libwebp/",
            "third_party/libavif/", "third_party/dav1d/", "third_party/libyuv/",
            "third_party/ots/", "third_party/woff2/", "third_party/libxml/",
            "third_party/libxslt/", "third_party/modp_b64/",
        )
        if name.endswith("_empty.cc"):
            return False, "legacy empty symbol implementation"
        return path.startswith(allowed), "retained text/image/DOM dependency"
    if path.startswith(("runtime/", "platform/", "ipc/", "crypto/", "components/", "services/", "gpu/", "media/", "net/")):
        return False, "not part of local UI implementation graph"
    if path.startswith("mojo/public/"):
        return name not in ("mojo_empty.cc", "mojo_lite.cc", "mojo_lite_msg.cc"), "Mojo public types, not legacy runtime emulation"
    if path.startswith(("base/", "ui/gfx/", "ui/base/", "ui/events/", "ui/display/", "ui/color/", "ui/native_theme/", "url/", "skia/", "cc/paint/", "cc/base/")):
        return True, "native UI infrastructure"
    if path.startswith(("third_party/blink/", "gen/third_party/blink/")):
        return True, "Blink UI core/platform"
    return False, "outside UI source allowlist"


def baseline(root: Path) -> tuple[dict[str, Source], list[dict[str, str]]]:
    selected: dict[str, Source] = {}
    excluded: list[dict[str, str]] = []
    for project in PROJECTS:
        filename = root / "build/windows/projects" / f"{project}.vcxproj"
        tree = ET.parse(filename)
        defaults: dict[str, str] = {}
        for group in tree.findall("m:ItemDefinitionGroup", NS):
            if matches_condition(group.get("Condition", "")):
                defaults.update(properties(group.find("m:ClCompile", NS)))
        for tag, kind in (("ClCompile", "cxx"), ("NASM", "nasm")):
            for item in tree.findall(f".//m:{tag}[@Include]", NS):
                if not matches_condition(item.get("Condition", "")):
                    continue
                specification = (filename.parent / item.attrib["Include"].replace("\\", "/")).resolve().relative_to(root).as_posix()
                metadata = defaults.copy()
                for key, value in properties(item).items():
                    metadata[key] = value.replace(f"%({key})", metadata.get(key, ""))
                if any(character in specification for character in "*?["):
                    paths = sorted(path.relative_to(root).as_posix()
                                   for path in root.glob(specification) if path.is_file())
                    if not paths:
                        excluded.append({"source": specification, "project": project,
                                         "reason": "MSBuild source wildcard has no matches"})
                else:
                    paths = (specification,)
                for path in paths:
                    keep, reason = retain(path)
                    if metadata.get("ExcludedFromBuild", "false").lower() == "true":
                        keep, reason = False, "XML Release x64 exclusion"
                    if not keep:
                        excluded.append({"source": path, "project": project, "reason": reason})
                        continue
                    if kind == "cxx" and Path(path).suffix.lower() == ".c":
                        item_kind = "c"
                    else:
                        item_kind = kind
                    selected.setdefault(path, Source(path, project, item_kind, metadata))
    # raw_logging_empty.cc is not a valid dependency of a real standalone library.
    selected["third_party/abseil-cpp/absl/base/internal/raw_logging.cc"] = Source(
        "third_party/abseil-cpp/absl/base/internal/raw_logging.cc", "chromium")
    for path in ("base/power_monitor/power_monitor.cc", "base/power_monitor/power_monitor_source.cc", "base/power_monitor/power_monitor_features.cc"):
        selected[path] = Source(path, "chromium")
    for path in QUICKJS_SOURCES:
        selected[path] = Source(path, "quickjs", "c")
    for path in UI_SOURCES:
        selected[path] = Source(path, "ui")
    scanner = "v8/src/heap/base/asm/x64/push_registers_masm.asm"
    selected[scanner] = Source(scanner, "cppgc", "masm")
    return selected, excluded
