// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "runtime/electron/common/renderer_server.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "runtime/electron/common/world_ids.h"
#include "build/build_config.h"
#include "runtime/electron/common/renderer_client.h"
#include "runtime/electron/common/renderer_platform.h"
#include "runtime/electron/common/renderer_protocol.h"
#include "runtime/electron/renderer/sandbox_preload_host.h"
#include "runtime/engine/api/internal_api.h"
#include "runtime/engine/browser/web_view_host.h"
#include "runtime/engine/common/live_id_detect.h"
#include "runtime/engine/public/engine_api.h"
#include "runtime/engine/common/thread_call.h"
#include "runtime/engine/renderer/platform_event_handler.h"
#include "ui/events/keycodes/dom/keycode_converter.h"
#include "runtime/network/loader/web_url_loader_internal.h"
#include "runtime/network/loader/web_url_loader_manager.h"
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
#include "runtime/engine/renderer/renderer_cdp_service.h"
#endif
#include "runtime/engine/renderer/brokered_file_registry.h"
#include "net/base/mime_util.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/common/page/drag_operation.h"
#include "third_party/blink/public/platform/web_blob_info.h"
#include "third_party/blink/public/platform/web_drag_data.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "third_party/blink/public/web/web_input_method_controller.h"
#include "third_party/blink/public/web/web_settings.h"
#include "third_party/blink/public/web/web_view.h"
#include "base/strings/escape.h"
#include "runtime/engine/renderer/renderer_permission_broker.h"
#include "third_party/blink/public/web/web_document.h"
#include "runtime/engine/renderer/renderer_storage_broker.h"
#include "runtime/engine/renderer/renderer_websocket_broker.h"
#include "third_party/blink/public/web/web_element.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "third_party/blink/renderer/platform/weborigin/scheme_registry.h"
#include "third_party/blink/renderer/platform/blob/blob_data.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/point.h"
#include "ui/display/screen_info.h"
#include "ui/gfx/geometry/point_f.h"
#include "third_party/skia/include/core/SkImageInfo.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "url/gurl.h"
#include "url/url_util.h"
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
#include "platform/clipboard.h"
#endif

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace atom {
namespace {

using renderer_protocol::MessageKind;
using renderer_protocol::WireMessage;

std::string FrameIdString(mini_electron_web_frame_handle frame)
{
    return base::NumberToString(
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(frame)));
}

bool SameNavigationResource(const std::string& approved_url,
    const std::string& request_url)
{
    GURL approved(approved_url);
    GURL request(request_url);
    return approved.is_valid() && request.is_valid()
        && approved.GetWithoutRef() == request.GetWithoutRef();
}

const char* ResourceTypeName(int resource_type)
{
    switch (resource_type) {
    case 0:
    case 19:
        return "mainFrame";
    case 1:
    case 20:
        return "subFrame";
    case 2: return "stylesheet";
    case 3: return "script";
    case 4: return "image";
    case 5: return "font";
    case 6: return "subResource";
    case 7: return "object";
    case 8: return "media";
    case 9: return "worker";
    case 10: return "sharedWorker";
    case 11: return "prefetch";
    case 12: return "favicon";
    case 13: return "xhr";
    case 14: return "ping";
    case 15: return "serviceWorker";
    case 16: return "cspReport";
    case 17: return "pluginResource";
    case 21: return "json";
    default: return "other";
    }
}

bool IsSafeRelativePath(const std::string& path)
{
    if (path.empty() || path.size() > 4096 || path.front() == '/'
        || path.find('\\') != std::string::npos)
        return false;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        size_t length = (end == std::string::npos ? path.size() : end)
            - start;
        std::string_view component(path.data() + start, length);
        if (component.empty() || component == "." || component == "..")
            return false;
        if (end == std::string::npos)
            return true;
        start = end + 1;
    }
    return false;
}

struct ParsedArguments {
    renderer_protocol::PipeHandle read_pipe = renderer_protocol::kInvalidPipe;
    renderer_protocol::PipeHandle write_pipe = renderer_protocol::kInvalidPipe;
#if BUILDFLAG(IS_WIN)
    HANDLE control = nullptr;
    HANDLE frame = nullptr;
#else
    int control = -1;
    int frame = -1;
#endif
    size_t frame_size = 0;
    uint64_t auth_high = 0;
    uint64_t auth_low = 0;
};

struct ServerOptions {
    int contents_id = 0;
    bool context_isolation = true;
    bool webview_tag = false;
    bool preload_all_frames = false;
    bool web_security = true;
    bool allow_running_insecure_content = false;
    bool transparent = false;
    uint32_t background_color = 0xffffffff;
    std::string accept_languages;
    std::vector<RendererPreload> preloads;
    std::vector<RendererPrivilegedScheme> privileged_schemes;
    base::Value::Dict process_metadata;
};

template <typename Char>
bool Equals(const Char* value, const char* ascii)
{
    while (*value && *ascii) {
        if (static_cast<unsigned int>(*value) != static_cast<unsigned char>(*ascii))
            return false;
        ++value;
        ++ascii;
    }
    return !*value && !*ascii;
}

template <typename Char>
std::string NarrowAscii(const Char* value)
{
    std::string result;
    while (*value) {
        unsigned int character = static_cast<unsigned int>(*value++);
        if (character > 0x7f)
            return {};
        result.push_back(static_cast<char>(character));
    }
    return result;
}

template <typename Char>
bool ParseArguments(int argc, Char* argv[], ParsedArguments* parsed)
{
    std::map<std::string, uint64_t> values;
    for (int i = 1; i < argc; ++i) {
        std::string argument = NarrowAscii(argv[i]);
        size_t equals = argument.find('=');
        if (argument.rfind("--renderer-", 0) != 0 || equals == std::string::npos)
            continue;
        uint64_t value = 0;
        if (!base::StringToUint64(argument.substr(equals + 1), &value))
            return false;
        values.emplace(argument.substr(2, equals - 2), value);
    }
    auto get = [&](const char* key, uint64_t* value) {
        auto found = values.find(key);
        if (found == values.end())
            return false;
        *value = found->second;
        return true;
    };
    uint64_t read = 0, write = 0, control = 0, frame = 0, frame_size = 0;
    if (!get("renderer-read-handle", &read)
        || !get("renderer-write-handle", &write)
        || !get("renderer-control-handle", &control)
        || !get("renderer-frame-handle", &frame)
        || !get("renderer-frame-size", &frame_size)
        || !get("renderer-auth-high", &parsed->auth_high)
        || !get("renderer-auth-low", &parsed->auth_low)
        || frame_size < sizeof(renderer_protocol::FrameBufferHeader)
        || frame_size > sizeof(renderer_protocol::FrameBufferHeader)
                + renderer_protocol::kMaxFrameBytes)
        return false;
#if BUILDFLAG(IS_WIN)
    parsed->read_pipe = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(read));
    parsed->write_pipe = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(write));
    parsed->control = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(control));
    parsed->frame = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(frame));
#else
    parsed->read_pipe = static_cast<int>(read);
    parsed->write_pipe = static_cast<int>(write);
    parsed->control = static_cast<int>(control);
    parsed->frame = static_cast<int>(frame);
#endif
    parsed->frame_size = static_cast<size_t>(frame_size);
    return true;
}

bool ParseOptions(base::Value::Dict launch, ServerOptions* options)
{
    std::optional<int> contents_id = launch.FindInt("contentsId");
    if (!contents_id || *contents_id <= 0)
        return false;
    options->contents_id = *contents_id;
    options->context_isolation = launch.FindBool("contextIsolation").value_or(true);
    options->webview_tag = launch.FindBool("webviewTag").value_or(false);
    options->preload_all_frames = launch.FindBool("preloadAllFrames").value_or(false);
    options->web_security = launch.FindBool("webSecurity").value_or(true);
    options->allow_running_insecure_content
        = launch.FindBool("allowRunningInsecureContent").value_or(false);
    options->transparent = launch.FindBool("transparent").value_or(false);
    if (const std::string* color = launch.FindString("backgroundColor")) {
        unsigned rgb = 0;
        if (color->size() == 7 && (*color)[0] == '#'
            && std::string_view(*color).substr(1).find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos
            && base::HexStringToUInt(std::string_view(*color).substr(1), &rgb))
            options->background_color = 0xff000000u | rgb;
    }
    if (const std::string* languages = launch.FindString("acceptLanguages"))
        options->accept_languages = *languages;
    if (base::Value::Dict* process = launch.FindDict("process"))
        options->process_metadata = process->Clone();
    if (base::Value::List* preloads = launch.FindList("preloads")) {
        for (base::Value& value : *preloads) {
            if (!value.is_dict())
                return false;
            std::string* filename = value.GetDict().FindString("filename");
            std::string* source = value.GetDict().FindString("source");
            if (!filename || !source)
                return false;
            options->preloads.push_back({ *filename, *source });
        }
    }
    if (base::Value::List* schemes = launch.FindList("privilegedSchemes")) {
        for (base::Value& value : *schemes) {
            if (!value.is_dict())
                return false;
            std::string* scheme = value.GetDict().FindString("scheme");
            if (!scheme || scheme->empty())
                return false;
            RendererPrivilegedScheme entry;
            entry.scheme = *scheme;
            entry.standard = value.GetDict().FindBool("standard").value_or(false);
            entry.secure = value.GetDict().FindBool("secure").value_or(false);
            entry.support_fetch_api = value.GetDict().FindBool("supportFetchAPI").value_or(false);
            entry.cors_enabled = value.GetDict().FindBool("corsEnabled").value_or(false);
            entry.allow_service_workers = value.GetDict().FindBool("allowServiceWorkers").value_or(false);
            options->privileged_schemes.push_back(std::move(entry));
        }
    }
    return true;
}

template <typename Task>
void PostBlink(Task&& task)
{
    using OwnedTask = std::decay_t<Task>;
    auto* owned = new OwnedTask(std::forward<Task>(task));
    mini_electron_call_blink_thread_async([](void* first, void*) {
        std::unique_ptr<OwnedTask> task(static_cast<OwnedTask*>(first));
        (*task)();
    }, owned, nullptr);
}

class RendererServerImpl {
public:
    using ParentReply = std::function<void(base::Value::Dict, std::string)>;
    struct ParentReplyEntry {
        ParentReply reply;
        bool run_on_reader_thread = false;
    };

    RendererServerImpl(ParsedArguments arguments, ServerOptions options,
        void* frame_memory)
        : arguments_(arguments)
        , options_(std::move(options))
        , frame_(static_cast<renderer_protocol::FrameBufferHeader*>(frame_memory))
    {
    }

    ~RendererServerImpl()
    {
        content::SetRendererStorageBroker({});
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
        mini_electron::SetRendererClipboardBroker({});
#endif
        content::SetRendererPermissionBroker({});
        StopControlThread();
        if (reader_.joinable())
            reader_.join();
        CleanupGuestFrames();
    }

    bool Start(std::string* error)
    {
        mini_electron_settings settings {};
        settings.version = kMiniElectronVersion;
        settings.mask = MINI_ELECTRON_ENABLE_DISABLE_CC;
        mini_electron_init(&settings);
        owner_runner_ = base::SequencedTaskRunner::GetCurrentDefault();
        content::SetRendererPermissionBroker(
            [this](const std::string& permission, bool user_gesture,
                bool is_request, content::RendererPermissionReply reply) {
                base::Value::Dict payload;
                payload.Set("permission", permission);
                payload.Set("userGesture", user_gesture);
                payload.Set("isRequest", is_request);
                content::RendererPermissionReply failure_reply = reply;
                if (!RequestParent("permission-request", std::move(payload),
                        [this, reply = std::move(reply)](
                            base::Value::Dict result,
                            std::string error) mutable {
                            bool granted = error.empty()
                                && result.FindBool("granted").value_or(false);
                            PostBlink([reply = std::move(reply),
                                          granted]() mutable {
                                reply(granted);
                            });
                        })) {
                    PostBlink([reply = std::move(failure_reply)]() mutable {
                        reply(false);
                    });
                }
            });
        content::SetRendererStorageBroker(
            [this](uint64_t frame_id, base::Value::Dict payload,
                content::RendererStorageReply reply) {
                payload.Set("kind", "storage");
                content::RendererStorageReply failure_reply = reply;
                auto frame = reinterpret_cast<mini_electron_web_frame_handle>(
                    static_cast<uintptr_t>(frame_id));
                if (!RequestParent("broker-request", std::move(payload),
                        [reply = std::move(reply)](
                            base::Value::Dict result,
                            std::string error) mutable {
                            reply(std::move(result), std::move(error));
                        },
                        frame, true)) {
                    failure_reply({}, "storage broker channel is closed");
                }
            });
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
        mini_electron::SetRendererClipboardBroker(
            [this](base::Value::Dict payload, base::Value::Dict* result) {
                payload.Set("kind", "clipboard");
                std::string error;
                return RequestParentSync("broker-request", std::move(payload),
                    main_frame_, result, &error);
            });
#endif
        RegisterSchemes();
#if BUILDFLAG(IS_MAC)
        view_ = mini_electron_create_web_view_with_background(
            options_.background_color, options_.transparent);
#else
        view_ = mini_electron_create_web_view();
#endif
        if (!view_) {
            *error = "engine refused to create renderer view";
            return false;
        }
        if (options_.transparent)
            mini_electron_set_transparent(view_, TRUE);
        content::WebViewHost* host = View();
        if (!host) {
            *error = "engine renderer view is unavailable";
            return false;
        }
        mini_electron_call_blink_thread_sync([](void* server, void*) {
            auto* self = static_cast<RendererServerImpl*>(server);
            content::WebViewHost* host = self->View();
            if (!host)
                return;
            host->setSetLanguage(self->options_.accept_languages);
            host->setTextInputStateCallback(
                [self] { self->ScheduleTextInputState(); });
            if (blink::WebView* web_view = host->getWebView()) {
                blink::WebSettings* settings = web_view->GetSettings();
                settings->SetWebSecurityEnabled(self->options_.web_security);
                settings->SetAllowRunningOfInsecureContent(
                    self->options_.allow_running_insecure_content);
            }
            if (blink::WebFrame* frame = host->getMainFrame();
                frame && frame->IsWebLocalFrame()) {
                size_t frame_id = blink::LocalFrameToken::Hasher()(
                    frame->ToWebLocalFrame()->GetLocalFrameToken());
                self->main_frame_ = reinterpret_cast<
                    mini_electron_web_frame_handle>(frame_id);
            }
            content::SetRendererFileChooserBroker(
                [self](uint64_t frame_id, base::Value::Dict request,
                    content::RendererFileChooserReply reply) {
                    auto frame = reinterpret_cast<
                        mini_electron_web_frame_handle>(
                        static_cast<uintptr_t>(frame_id));
                    content::RendererFileChooserReply failure_reply = reply;
                    auto epoch = self->frame_document_epochs_.find(frame_id);
                    if (epoch == self->frame_document_epochs_.end()) {
                        failure_reply({}, "file chooser frame is unavailable");
                        return;
                    }
                    const uint64_t document_epoch = epoch->second;
                    if (!self->RequestParent("file-chooser",
                            std::move(request),
                            [self, frame_id, document_epoch,
                                reply = std::move(reply)](
                                base::Value::Dict result,
                                std::string error) mutable {
                                self->MaterializeChosenFiles(frame_id,
                                    document_epoch, std::move(result),
                                    std::move(error), std::move(reply));
                            },
                            frame)) {
                        failure_reply({},
                            "file chooser broker channel is closed");
                    }
                });
            content::SetRendererWebSocketCommand(
                [self](uint64_t frame_id, base::Value::Dict command) {
                    double socket_id =
                        command.FindDouble("socketId").value_or(0);
                    auto frame = reinterpret_cast<
                        mini_electron_web_frame_handle>(
                        static_cast<uintptr_t>(frame_id));
                    if (!self->RequestParent("broker-request",
                            std::move(command),
                            [socket_id](base::Value::Dict result,
                                std::string error) mutable {
                                if (!error.empty()) {
                                    result.Set("socketId", socket_id);
                                    result.Set("event", "error");
                                    result.Set("message", std::move(error));
                                }
                                if (result.FindString("event")) {
                                    PostBlink(
                                        [event = std::move(result)]() mutable {
                                            content::
                                                DispatchRendererWebSocketEvent(
                                                    std::move(event));
                                        });
                                }
                            },
                            frame)) {
                        base::Value::Dict event;
                        event.Set("socketId", socket_id);
                        event.Set("event", "error");
                        event.Set("message",
                            "network broker channel is closed");
                        PostBlink([event = std::move(event)]() mutable {
                            content::DispatchRendererWebSocketEvent(
                                std::move(event));
                        });
                    }
                });
        }, this, nullptr);
        auto& callbacks = host->getClosure();
        callbacks.setDidCreateScriptContextCallback(&DidCreateContext, this);
        callbacks.setNavigationCallback(&Navigation, this);
        callbacks.setCreateViewCallback(&CreateView, this);
        callbacks.setDocumentReadyInBlinkCallback(&DocumentReady, this);
        callbacks.setTitleChangedCallback(&TitleChanged, this);
        callbacks.setURLChangedCallback(&UrlChanged, this);
        callbacks.setFrameURLChangedCallback(&FrameUrlChanged, this);
        callbacks.setFrameDetachedCallback(&FrameDetached, this);
        callbacks.setLoadingFinishCallback(&LoadingFinished, this);
        callbacks.setPaintBitUpdatedCallback(&PaintUpdated, this);
        callbacks.setDraggableRegionsChangedCallback(&DraggableRegionsChanged, this);
        callbacks.setConsoleCallback(&Console, this);
        callbacks.setCloseCallback(&CloseRequested, this);
        callbacks.setLoadUrlBeginCallback(&LoadUrlBegin, this);
        mini_electron_on_alert_box(view_, &Alert, this);
        mini_electron_on_confirm_box(view_, &Confirm, this);
        mini_electron_on_prompt_box(view_, &Prompt, this);
        mini_electron_set_auto_draw_to_hwnd(view_, FALSE);

        StartControlThread();
        Write({ MessageKind::kHello, 0,
            base::Value::Dict().Set("ready", true).Set("processId",
#if BUILDFLAG(IS_WIN)
                static_cast<int>(::GetCurrentProcessId())
#else
                static_cast<int>(::getpid())
#endif
            ) });
        owner_weak_ = weak_factory_.GetWeakPtr();
        reader_ = std::thread([this] { ReadLoop(); });
        return true;
    }

    void Run()
    {
        run_loop_ = std::make_unique<base::RunLoop>();
        run_loop_->Run();
        run_loop_.reset();
        content::SetRendererStorageBroker({});
        content::SetRendererPermissionBroker({});
        mini_electron_call_blink_thread_sync([](void* server, void*) {
            auto* self = static_cast<RendererServerImpl*>(server);
            content::SetRendererWebSocketCommand({});
            content::SetRendererFileChooserBroker({});
            content::ClearBrokeredFiles();
            if (content::WebViewHost* host = self->View())
                host->setTextInputStateCallback({});
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
            self->cdp_service_.reset();
#endif
        }, this, nullptr);
        if (view_)
            mini_electron_destroy_web_view(view_);
        view_ = NULL_WEBVIEW;
        // Preload proxy functions store their host as a raw V8 External. Keep
        // the hosts alive until destroying the web view has torn down every
        // frame context and drained pending Blink events.
        mini_electron_call_blink_thread_sync([](void* server, void*) {
            static_cast<RendererServerImpl*>(server)->preload_hosts_.clear();
        }, this, nullptr);
        // ClipboardHost runs on Blink's sequence. Keep its browser transport
        // installed until view/context teardown and queued Blink work drain.
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
        mini_electron::SetRendererClipboardBroker({});
#endif
        mini_electron_uninit();
    }

private:
    struct TextInputSnapshot {
        bool editable = false;
        uint64_t ime_serial = 0;
        int selection_start = -1;
        int selection_length = 0;
        int marked_start = -1;
        int marked_length = 0;
        float scale = 1.f;
        gfx::Rect selection_rect;
        gfx::Rect marked_rect;

        bool operator==(const TextInputSnapshot& other) const
        {
            return editable == other.editable
                && ime_serial == other.ime_serial
                && selection_start == other.selection_start
                && selection_length == other.selection_length
                && marked_start == other.marked_start
                && marked_length == other.marked_length
                && scale == other.scale
                && selection_rect == other.selection_rect
                && marked_rect == other.marked_rect;
        }
    };
    struct GuestFrame {
        void* memory = nullptr;
        size_t mapping_size = 0;
#if BUILDFLAG(IS_WIN)
        HANDLE handle = nullptr;
#else
        int fd = -1;
#endif
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        bool attached = false;
        bool focused = false;
        std::vector<uint8_t> snapshot;
    };
    struct PendingScreenshot {
        uint64_t request_id = 0;
        int width = 0;
        int height = 0;
        int output_width = 0;
        int output_height = 0;
        int original_width = 0;
        int original_height = 0;
        int quality = 80;
        std::string format;
    };
    struct PendingNavigation {
        std::string url;
        std::string method;
        std::string token;
    };


    content::WebViewHost* View() const
    {
        return static_cast<content::WebViewHost*>(
            common::LiveIdDetect::getWebViewIds()->getPtr(view_));
    }

    void ScheduleTextInputState()
    {
        // Called only on Blink's sequence. Query after the current lifecycle/
        // input update, never re-enter layout from a widget-host notification.
        if (text_input_pending_ || closing_)
            return;
        text_input_pending_ = true;
        PostBlink([this] {
            text_input_pending_ = false;
            if (!closing_)
                PublishTextInputState();
        });
    }

    void PublishTextInputState()
    {
        TextInputSnapshot state;
        state.ime_serial = text_input_serial_;
        content::WebViewHost* host = View();
        blink::WebView* web_view = host ? host->getWebView() : nullptr;
        blink::WebFrameWidget* widget = web_view ? web_view->MainFrameWidget() : nullptr;
        blink::WebLocalFrame* frame = web_view ? web_view->FocusedFrame() : nullptr;
        blink::WebInputMethodController* controller = widget
            ? widget->GetActiveWebInputMethodController() : nullptr;
        if (widget && widget->HasFocus() && frame && controller
            && controller->TextInputType() != blink::kWebTextInputTypeNone) {
            const blink::WebRange selection = controller->GetSelectionOffsets();
            if (!selection.IsNull()
                && frame->FirstRectForCharacterRange(selection.StartOffset(),
                    selection.length(), state.selection_rect)
                && state.selection_rect.height() > 0) {
                state.editable = true;
                state.selection_start = selection.StartOffset();
                state.selection_length = selection.length();
                // Blink viewport geometry is in physical pixels; publish the
                // scale from the same widget, not the host's pending resize.
                state.scale = widget->GetScreenInfo().device_scale_factor;
                const blink::WebRange marked = controller->CompositionRange();
                if (!marked.IsNull()) {
                    state.marked_start = marked.StartOffset();
                    state.marked_length = marked.length();
                    frame->FirstRectForCharacterRange(marked.StartOffset(),
                        marked.length(), state.marked_rect);
                }
            }
        }
        if (text_input_state_ && *text_input_state_ == state)
            return;
        text_input_state_ = state;
        base::Value::Dict payload;
        payload.Set("editable", state.editable);
        payload.Set("imeSerial", base::NumberToString(state.ime_serial));
        payload.Set("selectionStart", state.selection_start);
        payload.Set("selectionLength", state.selection_length);
        payload.Set("markedStart", state.marked_start);
        payload.Set("markedLength", state.marked_length);
        payload.Set("scale", static_cast<double>(state.scale));
        auto rect_value = [](const gfx::Rect& rect) {
            base::Value::Dict value;
            value.Set("x", rect.x());
            value.Set("y", rect.y());
            value.Set("width", rect.width());
            value.Set("height", rect.height());
            return value;
        };
        payload.Set("selectionRect", rect_value(state.selection_rect));
        payload.Set("markedRect", rect_value(state.marked_rect));
        SendEvent("text-input-state", std::move(payload));
    }

    void RegisterSchemes()
    {
        std::vector<RendererPrivilegedScheme> schemes = options_.privileged_schemes;
        PostBlink([schemes = std::move(schemes)] {
            for (const auto& entry : schemes) {
                if (entry.standard && !url::IsStandardScheme(entry.scheme))
                    url::AddStandardScheme(entry.scheme.c_str(), url::SCHEME_WITH_HOST);
                if (entry.secure) {
                    url::AddSecureScheme(entry.scheme.c_str());
                    blink::SchemeRegistry::RegisterURLSchemeBypassingSecureContextCheck(
                        WTF::String::FromUTF8(entry.scheme));
                }
                if (entry.cors_enabled)
                    url::AddCorsEnabledScheme(entry.scheme.c_str());
                if (entry.support_fetch_api)
                    blink::SchemeRegistry::RegisterURLSchemeAsSupportingFetchAPI(
                        WTF::String::FromUTF8(entry.scheme));
                if (entry.allow_service_workers)
                    blink::SchemeRegistry::RegisterURLSchemeAsAllowingServiceWorkers(
                        WTF::String::FromUTF8(entry.scheme));
            }
        });
        for (const auto& entry : options_.privileged_schemes)
            custom_schemes_.insert(entry.scheme);
        custom_schemes_.insert("mini-electron-broker");
    }

    bool Write(WireMessage message)
    {
        std::lock_guard<std::mutex> lock(write_lock_);
        std::string error;
        return renderer_protocol::WriteMessage(arguments_.write_pipe,
            arguments_.auth_high, arguments_.auth_low, message, &error);
    }

    void SendEvent(std::string type, base::Value::Dict payload,
        mini_electron_web_frame_handle frame = nullptr)
    {
        base::Value::Dict event;
        event.Set("type", std::move(type));
        event.Set("contentsId", options_.contents_id);
        event.Set("frameId", FrameIdString(frame));
        event.Set("payload", std::move(payload));
        Write({ MessageKind::kEvent, 0, std::move(event) });
    }

    uint64_t RequestParent(std::string type, base::Value::Dict payload,
        ParentReply callback, mini_electron_web_frame_handle frame = nullptr,
        bool reply_on_reader_thread = false)
    {
        uint64_t id = next_parent_request_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(parent_reply_lock_);
            parent_replies_.emplace(id,
                ParentReplyEntry { std::move(callback), reply_on_reader_thread });
        }
        base::Value::Dict event;
        event.Set("type", std::move(type));
        event.Set("contentsId", options_.contents_id);
        event.Set("frameId", FrameIdString(frame));
        event.Set("payload", std::move(payload));
        if (!Write({ MessageKind::kRequest, id, std::move(event) })) {
            std::lock_guard<std::mutex> lock(parent_reply_lock_);
            parent_replies_.erase(id);
            return 0;
        }
        return id;
    }

    bool RequestParentSync(std::string type, base::Value::Dict payload,
        mini_electron_web_frame_handle frame, base::Value::Dict* result,
        std::string* error)
    {
        std::mutex lock;
        std::condition_variable replied;
        bool done = false;
        if (!RequestParent(std::move(type), std::move(payload),
                [&](base::Value::Dict response, std::string response_error) {
                    {
                        std::lock_guard<std::mutex> guard(lock);
                        *result = std::move(response);
                        *error = std::move(response_error);
                        done = true;
                        // The waiter owns this condition variable on its stack.
                        // Keep its final notification before unlocking.
                        replied.notify_one();
                    }
                },
                frame, true)) {
            *error = "browser channel is closed";
            return false;
        }
        std::unique_lock<std::mutex> guard(lock);
        replied.wait(guard, [&] { return done; });
        return error->empty();
    }

    void StorePendingNavigation(uint64_t frame_id, uint64_t parent_frame_id,
        bool is_main_frame, std::string url, std::string method,
        std::string token)
    {
        if (!frame_id || token.empty())
            return;
        std::lock_guard<std::mutex> lock(navigation_lock_);
        pending_navigations_[frame_id] = {
            std::move(url), std::move(method), std::move(token)
        };
        frame_hierarchy_[frame_id] = { parent_frame_id, is_main_frame };
    }

    std::string TakePendingNavigation(uint64_t frame_id,
        const std::string& url, const std::string& method)
    {
        std::lock_guard<std::mutex> lock(navigation_lock_);
        auto found = pending_navigations_.find(frame_id);
        if (found == pending_navigations_.end()
            || !SameNavigationResource(found->second.url, url)
            || found->second.method != method)
            return {};
        std::string token = std::move(found->second.token);
        pending_navigations_.erase(found);
        return token;
    }

    bool HasPendingNavigation(uint64_t frame_id, const std::string& url,
        const std::string& method)
    {
        std::lock_guard<std::mutex> lock(navigation_lock_);
        auto found = pending_navigations_.find(frame_id);
        return found != pending_navigations_.end()
            && SameNavigationResource(found->second.url, url)
            && found->second.method == method;
    }
    uint64_t RequestParentWithId(std::string type, base::Value::Dict payload,
        std::function<void(uint64_t, base::Value::Dict, std::string)> callback,
        mini_electron_web_frame_handle frame = nullptr)
    {
        uint64_t id = next_parent_request_.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(parent_reply_lock_);
            parent_replies_.emplace(id, ParentReplyEntry {
                [id, callback = std::move(callback)](base::Value::Dict result,
                    std::string error) mutable {
                    callback(id, std::move(result), std::move(error));
                },
                false });
        }
        base::Value::Dict event;
        event.Set("type", std::move(type));
        event.Set("contentsId", options_.contents_id);
        event.Set("frameId", FrameIdString(frame));
        event.Set("payload", std::move(payload));
        if (!Write({ MessageKind::kRequest, id, std::move(event) })) {
            std::lock_guard<std::mutex> lock(parent_reply_lock_);
            parent_replies_.erase(id);
            return 0;
        }
        return id;
    }


#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
    bool DispatchDialogResponseOnReader(MessageKind kind, uint64_t request_id,
        const base::Value::Dict& payload)
    {
        const std::string* outer_method = payload.FindString("method");
        const base::Value::Dict* outer_params = payload.FindDict("params");
        if (!outer_method || *outer_method != "cdp.dispatch" || !outer_params)
            return false;
        const std::string* command = outer_params->FindString("method");
        const base::Value::Dict* command_params =
            outer_params->FindDict("params");
        if (!command || *command != "Page.handleJavaScriptDialog"
            || !command_params) {
            return false;
        }
        DispatchBrowserCdp(0, *command, *command_params,
            kind == MessageKind::kRequest ? request_id : 0);
        return true;
    }
#endif

    void ReadLoop()
    {
        std::string error;
        WireMessage message;
        while (renderer_protocol::ReadMessage(arguments_.read_pipe,
            arguments_.auth_high, arguments_.auth_low, &message, &error)) {
            if (message.kind == MessageKind::kResponse) {
                ParentReplyEntry entry;
                {
                    std::lock_guard<std::mutex> lock(parent_reply_lock_);
                    auto found = parent_replies_.find(message.request_id);
                    if (found == parent_replies_.end())
                        continue;
                    entry = std::move(found->second);
                    parent_replies_.erase(found);
                }
                base::Value::Dict result;
                if (base::Value::Dict* value = message.payload.FindDict("result"))
                    result = value->Clone();
                std::string response_error;
                if (std::string* value = message.payload.FindString("error"))
                    response_error = *value;
                if (entry.run_on_reader_thread) {
                    entry.reply(std::move(result), std::move(response_error));
                } else {
                    owner_runner_->PostTask(FROM_HERE,
                        base::BindOnce([](ParentReply reply,
                                           base::Value::Dict result,
                                           std::string error) {
                            reply(std::move(result), std::move(error));
                        }, std::move(entry.reply),
                            std::move(result), std::move(response_error)));
                }
            } else if (message.kind == MessageKind::kRequest
                || message.kind == MessageKind::kSend) {
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
                if (!DispatchDialogResponseOnReader(message.kind,
                        message.request_id, message.payload)) {
#endif
                    owner_runner_->PostTask(FROM_HERE,
                        base::BindOnce(&RendererServerImpl::HandleBrowserMessage,
                            owner_weak_, message.kind, message.request_id,
                            std::move(message.payload)));
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
                }
#endif
            } else if (message.kind == MessageKind::kShutdown) {
                break;
            } else if (message.kind == MessageKind::kCancel) {
                std::lock_guard<std::mutex> lock(cancel_lock_);
                cancelled_.insert(message.request_id);
            }
            message = WireMessage();
        }
        std::map<uint64_t, ParentReplyEntry> abandoned;
        {
            std::lock_guard<std::mutex> lock(parent_reply_lock_);
            abandoned.swap(parent_replies_);
        }
        for (auto& item : abandoned) {
            ParentReplyEntry& entry = item.second;
            if (entry.run_on_reader_thread) {
                entry.reply({}, "browser channel is closed");
            } else {
                owner_runner_->PostTask(FROM_HERE,
                    base::BindOnce([](ParentReply reply) {
                        reply({}, "browser channel is closed");
                    }, std::move(entry.reply)));
            }
        }
        owner_runner_->PostTask(FROM_HERE,
            base::BindOnce(&RendererServerImpl::Shutdown, owner_weak_));
    }

    void Reply(uint64_t id, base::Value::Dict result = {}, std::string error = {})
    {
        if (!id)
            return;
        {
            std::lock_guard<std::mutex> lock(cancel_lock_);
            if (cancelled_.erase(id))
                return;
        }
        base::Value::Dict payload;
        payload.Set("result", std::move(result));
        payload.Set("error", std::move(error));
        Write({ MessageKind::kResponse, id, std::move(payload) });
    }

    void HandleBrowserMessage(MessageKind kind, uint64_t request_id,
        base::Value::Dict payload)
    {
        std::string* method = payload.FindString("method");
        base::Value::Dict* params = payload.FindDict("params");
        if (!method || !params || !renderer_protocol::IsBrowserToRendererMethod(*method)) {
            if (kind == MessageKind::kRequest)
                Reply(request_id, {}, "renderer method is not allowed");
            return;
        }
        Dispatch(*method, params->Clone(),
            kind == MessageKind::kRequest ? request_id : 0);
    }

    void Dispatch(const std::string& method, base::Value::Dict params,
        uint64_t request_id)
    {
        if (method == "navigate") {
            std::string* url = params.FindString("url");
            if (!url || !GURL(*url).is_valid()) {
                Reply(request_id, {}, "invalid URL");
                return;
            }
            if (std::string* token = params.FindString("navigationToken")) {
                StorePendingNavigation(
                    static_cast<uint64_t>(
                        reinterpret_cast<uintptr_t>(main_frame_)),
                    0, true, *url, "GET", *token);
            }
            base::Value::Dict loading;
            loading.Set("loading", true);
            SendEvent("loading-state", std::move(loading), main_frame_);
            mini_electron_load_url(view_, url->c_str());
            Reply(request_id);
        } else if (method == "stop" || method == "reload" || method == "goBack"
            || method == "goForward" || method == "goToOffset" || method == "goToIndex") {
            PostBlink([this, method, params = std::move(params), request_id] {
                content::WebViewHost* view = View();
                if (!view) {
                    Reply(request_id, {}, "renderer view was destroyed");
                    return;
                }
                if (method == "stop") {
                    blink::WebFrame* frame = view->getMainFrame();
                    if (frame && frame->IsWebLocalFrame())
                        frame->ToWebLocalFrame()->DeprecatedStopLoading();
                } else if (method == "reload") {
                    view->reload(params.FindBool("ignoreCache").value_or(false));
                } else if (method == "goBack") {
                    view->navigateBackForwardSoon(-1);
                } else if (method == "goForward") {
                    view->navigateBackForwardSoon(1);
                } else if (method == "goToOffset") {
                    view->navigateBackForwardSoon(params.FindInt("offset").value_or(0));
                } else {
                    view->navigateToIndex(params.FindInt("index").value_or(0));
                }
                Reply(request_id);
            });
        } else if (method == "setViewport") {
            int width = params.FindInt("width").value_or(0);
            int height = params.FindInt("height").value_or(0);
            const float scale = static_cast<float>(
                params.FindDouble("scaleFactor").value_or(1.0));
            if (width <= 0 || height <= 0 || width > 16384 || height > 16384
                || !std::isfinite(scale) || scale <= 0.f) {
                Reply(request_id, {}, "invalid viewport");
                return;
            }
            bool schedule;
            {
                std::lock_guard<std::mutex> lock(viewport_lock_);
                viewport_width_ = width;
                viewport_height_ = height;
                viewport_scale_ = scale;
                schedule = !viewport_pending_;
                viewport_pending_ = true;
            }
            if (schedule) {
                PostBlink([this] {
                    int width;
                    int height;
                    float scale;
                    {
                        std::lock_guard<std::mutex> lock(viewport_lock_);
                        width = viewport_width_;
                        height = viewport_height_;
                        scale = viewport_scale_;
                        viewport_pending_ = false;
                    }
                    if (closing_)
                        return;
                    if (content::WebViewHost* view = View()) {
                        // Keep size and scale together, and apply only the
                        // latest pending tuple through the engine's resize gate.
                        view->setDeviceScaleFactor(scale);
                        view->onResize(width, height, false);
                    }
                });
            }
            Reply(request_id);
        } else if (method == "setFocus") {
            const bool focused = params.FindBool("focused").value_or(false);
            content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, view_,
                [this, focused](content::WebViewHost* view) {
                    if (focused) {
                        view->setFocus();
                    } else {
                        blink::WebFrameWidget* widget = view->getWebView()
                            ? view->getWebView()->MainFrameWidget() : nullptr;
                        blink::WebInputMethodController* controller = widget
                            ? widget->GetActiveWebInputMethodController() : nullptr;
                        if (controller && !controller->CompositionRange().IsNull())
                            controller->FinishComposingText(
                                blink::WebInputMethodController::kKeepSelection);
                        view->killFocus();
                    }
                    text_input_state_.reset();
                    ScheduleTextInputState();
                });
            Reply(request_id);
        } else if (method == "input") {
            base::Value::Dict* event = params.FindDict("event");
            if (!event || !DispatchInput(*event))
                Reply(request_id, {}, "invalid input event");
            else
                Reply(request_id);
        } else if (method == "copyImageAt") {
            const int x = params.FindInt("x").value_or(0);
            const int y = params.FindInt("y").value_or(0);
            PostBlink([this, x, y, request_id] {
                content::WebViewHost* view = View();
                blink::WebFrame* frame = view ? view->getMainFrame() : nullptr;
                blink::WebFrameWidget* widget = view && view->getWebView()
                    ? view->getWebView()->MainFrameWidget() : nullptr;
                if (!frame || !frame->IsWebLocalFrame() || !widget) {
                    Reply(request_id, {}, "renderer view was destroyed");
                    return;
                }
                const float scale = widget->GetOriginalScreenInfo().device_scale_factor;
                frame->ToWebLocalFrame()->CopyImageAtForTesting(
                    gfx::ScaleToRoundedPoint(gfx::Point(x, y), scale));
                Reply(request_id);
            });
        } else if (method == "executeJavaScript") {
            std::string* code = params.FindString("code");
            if (!code) {
                Reply(request_id, {}, "missing JavaScript source");
                return;
            }
            auto* pending = new std::pair<RendererServerImpl*, uint64_t>(this, request_id);
            mini_electron_run_js(view_, mini_electron_web_frame_get_main_frame(view_),
                code->c_str(), FALSE, [](mini_electron_web_view, void* parameter,
                    mini_electron_js_exec_state state, mini_electron_js_value value) {
                    std::unique_ptr<std::pair<RendererServerImpl*, uint64_t>> pending(
                        static_cast<std::pair<RendererServerImpl*, uint64_t>*>(parameter));
                    base::Value::Dict result;
                    switch (mini_electron_get_js_value_type(state, value)) {
                    case kMiniElectronJsTypeNumber:
                        result.Set("value", mini_electron_js_to_double(state, value));
                        break;
                    case kMiniElectronJsTypeBool:
                        result.Set("value", !!mini_electron_js_to_boolean(state, value));
                        break;
                    case kMiniElectronJsTypeNull:
                    case kMiniElectronJsTypeUndefined:
                        result.Set("value", base::Value());
                        break;
                    default: {
                        const char* text = mini_electron_js_to_string(state, value);
                        result.Set("value", text ? text : "");
                        break;
                    }
                    }
                    pending->first->Reply(pending->second, std::move(result));
                }, pending, nullptr);
        } else if (method == "insertCSS") {
            std::string* css = params.FindString("css");
            if (!css) {
                Reply(request_id, {}, "missing CSS");
                return;
            }
            auto* pending = new std::pair<RendererServerImpl*, uint64_t>(this, request_id);
            mini_electron_insert_css_by_frame_with_result(view_,
                mini_electron_web_frame_get_main_frame(view_), css->c_str(), 0,
                [](mini_electron_web_view, void* parameter, const utf8* key) {
                    std::unique_ptr<std::pair<RendererServerImpl*, uint64_t>> pending(
                        static_cast<std::pair<RendererServerImpl*, uint64_t>*>(parameter));
                    base::Value::Dict result;
                    result.Set("key", key ? key : "");
                    pending->first->Reply(pending->second, std::move(result));
                }, pending);
        } else if (method == "capturePage") {
            base::Value::Dict result;
            result.Set("generation", static_cast<double>(
                frame_->generation.load(std::memory_order_acquire)));
            result.Set("width", static_cast<int>(frame_->width));
            result.Set("height", static_cast<int>(frame_->height));
            result.Set("stride", static_cast<int>(frame_->stride));
            Reply(request_id, std::move(result));
        } else if (method == "setUserAgent") {
            std::string* user_agent = params.FindString("userAgent");
            if (!user_agent || user_agent->size() > 4096)
                Reply(request_id, {}, "invalid user agent");
            else {
                mini_electron_set_user_agent(view_, user_agent->c_str());
                Reply(request_id);
            }
        } else if (method == "setZoomLevel") {
            double level = params.FindDouble("level").value_or(0.0);
            mini_electron_set_zoom_factor(view_, static_cast<float>(std::pow(1.2, level)));
            Reply(request_id);
        } else if (method == "setIgnoreMenuShortcuts") {
            ignore_menu_shortcuts_ = params.FindBool("ignore").value_or(false);
            Reply(request_id);
        } else if (method == "set-background-color") {
            const std::string* color = params.FindString("color");
            unsigned rgb = 0;
            if (!color || color->size() != 7 || (*color)[0] != '#'
                || std::string_view(*color).substr(1).find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos
                || !base::HexStringToUInt(std::string_view(*color).substr(1), &rgb)) {
                Reply(request_id, {}, "invalid background color");
            } else {
                options_.background_color = 0xff000000u | rgb;
                PostBlink([this, background = options_.background_color] {
                    if (content::WebViewHost* host = View())
                        host->setBackgroundColor(background);
                });
                Reply(request_id);
            }
        } else if (method == "prepare-to-show") {
            if (closing_) {
                Reply(request_id, {}, "renderer is shutting down");
                return;
            }
            pending_preparations_.insert(request_id);
            auto complete = [runner = owner_runner_, weak = owner_weak_, request_id](bool success) {
                runner->PostTask(FROM_HERE, base::BindOnce(
                    &RendererServerImpl::CompletePreparation, weak, request_id, success));
            };
            // Enter the same Blink sequence as preceding IPC delivery. The
            // callback belongs to the forced commit, not an already queued draw.
            PostBlink([view_id = view_, complete = std::move(complete)]() mutable {
                base::ScopedClosureRunner abandoned(base::BindOnce(
                    [](decltype(complete) complete) { complete(false); }, complete));
                auto* host = static_cast<content::WebViewHost*>(
                    common::LiveIdDetect::getWebViewIds()->getPtr(view_id));
                blink::WebFrameWidget* widget = host && host->getWebView()
                    ? host->getWebView()->MainFrameWidget() : nullptr;
                if (!widget)
                    return;
                widget->RequestSuccessfulPresentationWithForcedRedraw(base::BindOnce(
                    [](base::ScopedClosureRunner abandoned,
                        decltype(complete) complete) {
                        (void)abandoned.Release();
                        complete(true);
                    }, std::move(abandoned), std::move(complete)));
            });
        } else if (method == "invalidate") {
            // Wake schedules a genuine compositor frame; paint notification is
            // emitted only when the engine produces that frame.
            mini_electron_wake(view_);
            Reply(request_id);
        } else if (method == "getState") {
            PostBlink([this, request_id] {
                base::Value::Dict result;
                if (content::WebViewHost* view = View()) {
                    result.Set("url", view->getUrl());
                    result.Set("title", view->getWindowTitle());
                    result.Set("canGoBack", view->canGoBack());
                    result.Set("canGoForward", view->canGoForward());
                    result.Set("zoomFactor", view->getZoomFactor());
                    Reply(request_id, std::move(result));
                } else {
                    Reply(request_id, {}, "renderer view was destroyed");
                }
            });
        } else if (method == "ipc-message") {
            std::string* channel = params.FindString("channel");
            base::Value::List* args = params.FindList("args");
            const std::string* frame_identity = params.FindString("frameId");
            std::optional<bool> all_frames = params.FindBool("allFrames");
            uint64_t frame_id = 0;
            if (!channel || !args || !frame_identity || !all_frames
                || channel->size() > 1024
                || (!*all_frames
                    && (!base::StringToUint64(*frame_identity, &frame_id)
                        || base::NumberToString(frame_id) != *frame_identity
                        || !frame_id
                        || static_cast<uint64_t>(
                            static_cast<uintptr_t>(frame_id)) != frame_id))) {
                Reply(request_id, {}, "invalid IPC message");
                return;
            }
            PostBlink([this, channel = std::move(*channel),
                          args = std::move(*args), frame_id, all_frames = *all_frames,
                          request_id]() mutable {
                if (all_frames) {
                    size_t remaining = preload_hosts_.size();
                    for (auto& [frame, preload] : preload_hosts_) {
                        preload->DeliverIpc(channel,
                            --remaining == 0 ? std::move(args) : args.Clone());
                    }
                    Reply(request_id);
                    return;
                }
                auto target = preload_hosts_.find(
                    static_cast<uintptr_t>(frame_id));
                if (target == preload_hosts_.end()) {
                    Reply(request_id, {}, "IPC target frame is unavailable");
                    return;
                }
                target->second->DeliverIpc(channel, std::move(args));
                Reply(request_id);
            });
        } else if (method.rfind("guest.", 0) == 0) {
            DispatchGuest(method, params);
            Reply(request_id);
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
        } else if (method.rfind("cdp.", 0) == 0) {
            DispatchCdp(method, std::move(params), request_id);
#endif
        } else if (method == "network.websocket-event") {
            PostBlink([event = std::move(params)]() mutable {
                content::DispatchRendererWebSocketEvent(std::move(event));
            });
            Reply(request_id);
        } else {
            Reply(request_id, {}, "renderer method is not implemented");
        }
    }
    static uint32_t NativeDragModifiers(int flags)
    {
        uint32_t modifiers = 0;
        if (flags & MINI_ELECTRON_SHIFT)
            modifiers |= blink::WebInputEvent::kShiftKey;
        if (flags & MINI_ELECTRON_CONTROL)
            modifiers |= blink::WebInputEvent::kControlKey;
        if (flags & MINI_ELECTRON_ALT)
            modifiers |= blink::WebInputEvent::kAltKey;
        if (flags & MINI_ELECTRON_META)
            modifiers |= blink::WebInputEvent::kMetaKey;
        return modifiers;
    }

    bool IsCurrentFrameDocument(
        uint64_t frame_id, uint64_t document_epoch) const
    {
        auto found = frame_document_epochs_.find(frame_id);
        return found != frame_document_epochs_.end()
            && found->second == document_epoch;
    }

    void ReportNativeDragOperation(uint64_t generation,
        ui::mojom::DragOperation operation, bool document_is_handling_drag)
    {
        if (native_drag_generation_ != generation)
            return;
        base::Value::Dict payload;
        payload.Set("generation", base::NumberToString(generation));
        payload.Set("accepted", document_is_handling_drag
            && operation != ui::mojom::DragOperation::kNone);
        SendEvent("native-drag-operation", std::move(payload), main_frame_);
    }

    void ResetNativeDragOnBlink(bool preserve_brokered_files = false)
    {
        native_drag_generation_ = 0;
        native_drag_ready_ = false;
        native_drag_data_ = blink::WebDragData();
        native_drag_pending_.clear();
        if (!preserve_brokered_files) {
            for (const std::string& url : native_drag_broker_urls_)
                content::UnregisterBrokeredFile(url);
        }
        native_drag_broker_urls_.clear();
    }

    void DispatchNativeDragOnBlink(const base::Value::Dict& event,
        const blink::WebDragData& data)
    {
        content::WebViewHost* host = View();
        blink::WebFrameWidget* widget = host && host->getWebView()
            ? host->getWebView()->MainFrameWidget() : nullptr;
        const std::string* type = event.FindString("type");
        if (!widget || !type)
            return;
        // AppKit reports DIPs; Blink drag hit testing uses viewport pixels.
        const float scale = widget->GetOriginalScreenInfo().device_scale_factor;
        gfx::PointF point(event.FindInt("x").value_or(0) * scale,
            event.FindInt("y").value_or(0) * scale);
        gfx::PointF screen_point(event.FindInt("screenX").value_or(0),
            event.FindInt("screenY").value_or(0));
        uint32_t modifiers = NativeDragModifiers(
            event.FindInt("modifiers").value_or(0));
        if (*type == "dragEnter") {
            widget->DragTargetDragEnter(data, point, screen_point,
                blink::kDragOperationCopy, modifiers,
                base::BindOnce(&RendererServerImpl::ReportNativeDragOperation,
                    base::Unretained(this), native_drag_generation_));
        } else if (*type == "dragOver") {
            widget->DragTargetDragOver(point, screen_point,
                blink::kDragOperationCopy, modifiers,
                base::BindOnce(&RendererServerImpl::ReportNativeDragOperation,
                    base::Unretained(this), native_drag_generation_));
        } else if (*type == "dragLeave") {
            widget->DragTargetDragLeave(point, screen_point);
        } else if (*type == "drop") {
            widget->DragTargetDrop(data, point, screen_point, modifiers,
                base::DoNothing());
        }
    }

    void BeginNativeDragOnBlink(base::Value::Dict event, uint64_t generation)
    {
        if (native_drag_generation_) {
            if (native_drag_ready_) {
                base::Value::Dict leave = event.Clone();
                leave.Set("type", "dragLeave");
                DispatchNativeDragOnBlink(leave, native_drag_data_);
            }
            ResetNativeDragOnBlink();
        }
        native_drag_generation_ = generation;
        native_drag_pending_.push_back(std::move(event));
    }

    void HandleNativeDragOnBlink(base::Value::Dict event,
        uint64_t generation)
    {
        if (!native_drag_generation_ || native_drag_generation_ != generation)
            return;
        const std::string* type = event.FindString("type");
        if (!type)
            return;
        if (!native_drag_ready_) {
            if (*type == "dragOver" && !native_drag_pending_.empty()
                && native_drag_pending_.back().FindString("type")
                    && *native_drag_pending_.back().FindString("type")
                        == "dragOver") {
                native_drag_pending_.back() = std::move(event);
            } else {
                native_drag_pending_.push_back(std::move(event));
            }
            return;
        }
        const bool terminal = *type == "drop" || *type == "dragLeave";
        const bool preserve_brokered_files = *type == "drop";
        DispatchNativeDragOnBlink(event, native_drag_data_);
        if (terminal)
            ResetNativeDragOnBlink(preserve_brokered_files);
    }

    void AcknowledgeNativeDragMaterialization(
        uint64_t generation, bool success)
    {
        base::Value::Dict payload;
        payload.Set("generation", base::NumberToString(generation));
        payload.Set("success", success);
        SendEvent("native-drag-materialized", std::move(payload), main_frame_);
    }

    void CompleteNativeDragOnBlink(uint64_t generation,
        blink::WebDragData data, std::string error)
    {
        if (native_drag_generation_ != generation)
            return;
        AcknowledgeNativeDragMaterialization(generation, error.empty());
        if (!error.empty()) {
            ResetNativeDragOnBlink();
            return;
        }
        native_drag_ready_ = true;
        native_drag_data_ = std::move(data);
        std::vector<base::Value::Dict> pending =
            std::move(native_drag_pending_);
        for (const base::Value::Dict& event : pending) {
            if (native_drag_generation_ != generation)
                break;
            const std::string* type = event.FindString("type");
            if (!type)
                continue;
            DispatchNativeDragOnBlink(event, native_drag_data_);
            if (*type == "drop" || *type == "dragLeave") {
                ResetNativeDragOnBlink(*type == "drop");
                break;
            }
        }
    }

    void MaterializeNativeDrag(uint64_t frame_id, uint64_t generation,
        base::Value::List files)
    {
        constexpr size_t kMaximumDroppedFiles = 1024;
        constexpr int64_t kMaximumDroppedFileBytes = 50LL * 1024 * 1024;
        constexpr int64_t kMaximumTotalBytes = 256LL * 1024 * 1024;
        if (files.empty() || files.size() > kMaximumDroppedFiles) {
            PostBlink([this, generation] {
                CompleteNativeDragOnBlink(generation, blink::WebDragData(),
                    "native drag exceeds file count limits");
            });
            return;
        }
        auto source = std::make_shared<base::Value::List>(std::move(files));
        auto output = std::make_shared<blink::WebDragData>();
        auto output_items = std::make_shared<
            std::vector<blink::WebDragData::Item>>();
        output_items->reserve(source->size());
        const std::string file_system_id =
            "mini-electron-drag-" + base::NumberToString(generation);
        output->SetFilesystemId(
            blink::WebString::FromUTF8(file_system_id));
        auto index = std::make_shared<size_t>(0);
        auto total_bytes = std::make_shared<int64_t>(0);
        auto next = std::make_shared<std::function<void(std::string)>>();
        *next = [this, frame_id, generation, source, output, output_items,
                    index, total_bytes, next, file_system_id](
                    std::string next_error) mutable {
            if (native_drag_generation_ != generation) {
                *next = {};
                return;
            }
            if (!next_error.empty()) {
                auto error = std::move(next_error);
                RendererServerImpl* self = this;
                const uint64_t completed_generation = generation;
                *next = {};
                self->CompleteNativeDragOnBlink(completed_generation,
                    blink::WebDragData(), std::move(error));
                return;
            }
            if (*index == source->size()) {
                output->SetItems(blink::WebVector<blink::WebDragData::Item>(
                    std::move(*output_items)));
                blink::WebDragData completed = std::move(*output);
                RendererServerImpl* self = this;
                const uint64_t completed_generation = generation;
                *next = {};
                self->CompleteNativeDragOnBlink(completed_generation,
                    std::move(completed), {});
                return;
            }
            const size_t item_index = (*index)++;
            base::Value::Dict* metadata = (*source)[item_index].GetIfDict();
            const std::string* name = metadata
                ? metadata->FindString("name") : nullptr;
            const std::string* path = metadata
                ? metadata->FindString("path") : nullptr;
            std::optional<int> size = metadata
                ? metadata->FindInt("size") : std::nullopt;
            std::optional<bool> directory = metadata
                ? metadata->FindBool("directory") : std::nullopt;
            const std::string* token = metadata
                ? metadata->FindString("token") : nullptr;
            const double last_modified = metadata
                ? metadata->FindDouble("lastModified").value_or(0) : 0;
            const bool valid_name = name && !name->empty()
                && name->size() <= 4096 && *name != "." && *name != ".."
                && name->find('/') == std::string::npos
                && name->find('\\') == std::string::npos;
            const bool valid_token = directory.value_or(false)
                ? !token
                : token && !token->empty() && token->size() <= 256
                    && std::all_of(token->begin(), token->end(), [](char c) {
                        return (c >= 'a' && c <= 'z')
                            || (c >= 'A' && c <= 'Z')
                            || (c >= '0' && c <= '9')
                            || c == '-' || c == '_';
                    });
            if (!valid_name || !path || path->empty() || path->size() > 4096
                || !size || *size < 0 || !directory || !valid_token
                || !std::isfinite(last_modified)
                || (*directory && *size != 0)
                || (!*directory && *size > kMaximumDroppedFileBytes)
                || *size > kMaximumTotalBytes - *total_bytes) {
                (*next)("native drag returned invalid file metadata");
                return;
            }
            *total_bytes += *size;
            std::string name_copy = *name;
            std::string path_copy = *path;
            std::string token_copy = token ? *token : std::string();
            const bool is_directory = *directory;
            const int item_size = *size;
            const std::string broker_host = is_directory
                ? "drag-" + base::NumberToString(generation) + "-"
                    + base::NumberToString(item_index)
                : token_copy;
            const std::string broker_url = GURL(
                "mini-electron-broker://" + broker_host + "/"
                + base::EscapePath(name_copy)).spec();
            auto register_item = [this, frame_id, generation, output_items,
                                     next, file_system_id, broker_url,
                                     name = std::move(name_copy),
                                     path = std::move(path_copy), item_size,
                                     last_modified, is_directory](
                                     std::vector<uint8_t> bytes) mutable {
                if (native_drag_generation_ != generation) {
                    *next = {};
                    return;
                }
                content::RegisterBrokeredFile(broker_url, name,
                    std::move(bytes), path, frame_id, is_directory);
                native_drag_broker_urls_.push_back(broker_url);
                std::string mime_type;
                if (!is_directory) {
                    net::GetMimeTypeFromFile(
                        base::FilePath::FromUTF8Unsafe(name), &mime_type);
                }
                scoped_refptr<blink::BlobDataHandle> blob =
                    content::GetBrokeredFileBlob(frame_id, broker_url,
                        blink::WebString::FromUTF8(mime_type));
                if (!blob || blob->size() != static_cast<uint64_t>(item_size)) {
                    (*next)("native drag failed to create brokered file");
                    return;
                }
                blink::WebDragData::FileSystemFileItem item;
                item.url = blink::WebURL(blink::KURL(
                    WTF::String::FromUTF8(broker_url)));
                item.size = item_size;
                item.file_system_id =
                    blink::WebString::FromUTF8(file_system_id);
                item.blob_info = blink::WebBlobInfo(blob,
                    blink::WebString::FromUTF8(name),
                    blink::WebString::FromUTF8(mime_type),
                    base::Time::FromMillisecondsSinceUnixEpoch(last_modified),
                    item_size);
                output_items->emplace_back(std::move(item));
                (*next)({});
            };
            if (is_directory) {
                register_item({});
                return;
            }
            ReadAuthorizedUpload(frame_id, token_copy, item_size,
                [register_item = std::move(register_item), next](
                    std::vector<uint8_t> bytes,
                    std::string read_error) mutable {
                    PostBlink([register_item = std::move(register_item), next,
                        bytes = std::move(bytes),
                        read_error = std::move(read_error)]() mutable {
                        if (!read_error.empty()) {
                            (*next)(std::move(read_error));
                            return;
                        }
                        register_item(std::move(bytes));
                    });
                });
        };
        PostBlink([next] { (*next)({}); });
    }

    bool DispatchNativeDrag(const base::Value::Dict& event)
    {
        const std::string* type = event.FindString("type");
        const std::string* generation_text = event.FindString("generation");
        uint64_t generation = 0;
        if (!type || !generation_text
            || !base::StringToUint64(*generation_text, &generation)
            || !generation
            || base::NumberToString(generation) != *generation_text
            || !event.FindInt("x") || !event.FindInt("y")
            || !event.FindInt("screenX") || !event.FindInt("screenY")
            || !event.FindInt("modifiers")) {
            return false;
        }
        if (*type == "dragEnter") {
            const base::Value::List* files = event.FindList("files");
            const uint64_t frame_id = static_cast<uint64_t>(
                reinterpret_cast<uintptr_t>(main_frame_));
            if (!files || !frame_id)
                return false;
            PostBlink([this, generation, event = event.Clone()]() mutable {
                BeginNativeDragOnBlink(std::move(event), generation);
            });
            MaterializeNativeDrag(frame_id, generation, files->Clone());
            return true;
        }
        if (*type != "dragOver" && *type != "dragLeave" && *type != "drop")
            return false;
        PostBlink([this, generation, event = event.Clone()]() mutable {
            HandleNativeDragOnBlink(std::move(event), generation);
        });
        return true;
    }


    bool DispatchInput(const base::Value::Dict& event)
    {
        const std::string* type = event.FindString("type");
        if (!type)
            return false;
        if (*type == "dragEnter" || *type == "dragOver"
            || *type == "dragLeave" || *type == "drop") {
            return DispatchNativeDrag(event);
        }
        int x = event.FindInt("x").value_or(0);
        int y = event.FindInt("y").value_or(0);
        int flags = event.FindInt("modifiers").value_or(0);
        int guest_id = HitTestGuest(x, y, *type);
        if (guest_id) {
            base::Value::Dict payload;
            payload.Set("guestContentsId", guest_id);
            payload.Set("event", event.Clone());
            SendEvent("guest-input", std::move(payload));
            return true;
        }
        if (*type == "imeSetComposition" || *type == "imeCommitText"
            || *type == "imeFinishComposition") {
            const std::string* text = event.FindString("text");
            if (*type != "imeFinishComposition" && !text)
                return false;
            const int replacement_start = event.FindInt("replacementStart").value_or(-1);
            const int replacement_length = event.FindInt("replacementLength").value_or(0);
            const int selection_start = event.FindInt("selectionStart").value_or(0);
            const int selection_length = event.FindInt("selectionLength").value_or(0);
            if (replacement_start < -1 || replacement_length < 0
                || selection_start < 0 || selection_length < 0
                || (replacement_start >= 0 && replacement_length
                    > std::numeric_limits<int>::max() - replacement_start)
                || selection_length > std::numeric_limits<int>::max() - selection_start)
                return false;
            uint64_t ime_serial = 0;
            if (const std::string* serial = event.FindString("imeSerial")) {
                if (!base::StringToUint64(*serial, &ime_serial))
                    return false;
            }
            PostBlink([this, type = *type, text = text ? *text : std::string(),
                          replacement_start, replacement_length,
                          selection_start, selection_length, ime_serial] {
                content::WebViewHost* host = View();
                text_input_serial_ = ime_serial;
                blink::WebFrameWidget* widget = host && host->getWebView()
                    ? host->getWebView()->MainFrameWidget() : nullptr;
                blink::WebInputMethodController* controller = widget
                    ? widget->GetActiveWebInputMethodController() : nullptr;
                if (!controller || !widget->HasFocus()) {
                    ScheduleTextInputState();
                    return;
                }
                const blink::WebRange replacement = replacement_start < 0
                    ? blink::WebRange()
                    : blink::WebRange(replacement_start, replacement_length);
                if (type == "imeSetComposition") {
                    controller->SetComposition(blink::WebString::FromUTF8(text),
                        {}, replacement, selection_start,
                        selection_start + selection_length);
                } else if (type == "imeCommitText") {
                    controller->CommitText(blink::WebString::FromUTF8(text),
                        {}, replacement, 0);
                } else {
                    controller->FinishComposingText(
                        blink::WebInputMethodController::kKeepSelection);
                }
                ScheduleTextInputState();
            });
            return true;
        }
        if (*type == "editCommand") {
            const std::string* command = event.FindString("command");
            if (!command)
                return false;
            mini_electron_execute_edit_command(view_, command->c_str());
            return true;
        }
        if (*type == "mouseWheel")
            return !!mini_electron_fire_mouse_wheel_event(view_, x, y,
                static_cast<int>(std::lround(event.FindDouble("deltaY").value_or(0))), flags);
        if (*type == "mouseMove" || *type == "mouseDown" || *type == "mouseUp") {
            unsigned message = MINI_ELECTRON_MSG_MOUSEMOVE;
            int button = event.FindInt("button").value_or(0);
            if (const std::string* name = event.FindString("button")) {
                if (*name == "right")
                    button = 2;
                else if (*name == "middle")
                    button = 1;
            }
#if BUILDFLAG(IS_WIN)
            const int native_message =
                event.FindInt("nativeMessage").value_or(0);
            switch (native_message) {
            case WM_MBUTTONDOWN:
            case WM_MBUTTONDBLCLK:
            case WM_MBUTTONUP:
                button = 1;
                break;
            case WM_RBUTTONDOWN:
            case WM_RBUTTONDBLCLK:
            case WM_RBUTTONUP:
                button = 2;
                break;
            default:
                break;
            }
#endif
            if (*type == "mouseDown")
                message = button == 2 ? MINI_ELECTRON_MSG_RBUTTONDOWN
                    : button == 1 ? MINI_ELECTRON_MSG_MBUTTONDOWN : MINI_ELECTRON_MSG_LBUTTONDOWN;
            else if (*type == "mouseUp")
                message = button == 2 ? MINI_ELECTRON_MSG_RBUTTONUP
                    : button == 1 ? MINI_ELECTRON_MSG_MBUTTONUP : MINI_ELECTRON_MSG_LBUTTONUP;
#if BUILDFLAG(IS_WIN)
            switch (native_message) {
            case WM_MOUSEMOVE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
            case WM_LBUTTONUP:
            case WM_MBUTTONDOWN:
            case WM_MBUTTONDBLCLK:
            case WM_MBUTTONUP:
            case WM_RBUTTONDOWN:
            case WM_RBUTTONDBLCLK:
            case WM_RBUTTONUP:
                message = static_cast<unsigned>(native_message);
                break;
            default:
                break;
            }
#endif
            return !!mini_electron_fire_mouse_event(view_, message, x, y, flags);
        }
        unsigned key = static_cast<unsigned>(event.FindInt("keyCode").value_or(0));
        bool system = event.FindBool("systemKey").value_or(false);
#if BUILDFLAG(IS_WIN)
        if ((*type == "keyDown" || *type == "keyUp" || *type == "char")
            && event.FindInt("nativeMessage") && event.FindInt("nativeLParam")) {
            auto event_type = *type == "keyDown"
                ? blink::WebInputEvent::Type::kRawKeyDown
                : *type == "keyUp" ? blink::WebInputEvent::Type::kKeyUp
                                   : blink::WebInputEvent::Type::kChar;
            uint64_t keyboard_layout = 0;
            if (const std::string* layout = event.FindString("keyboardLayout"))
                base::StringToUint64(*layout, &keyboard_layout);
            auto input = content::PlatformEventHandler::buildKeyboardEvent(
                event_type, static_cast<UINT>(*event.FindInt("nativeMessage")),
                *type == "char" ? event.FindInt("charCode").value_or(key) : key,
                static_cast<LPARAM>(static_cast<uint32_t>(
                    *event.FindInt("nativeLParam"))),
                flags, reinterpret_cast<HKL>(
                    static_cast<uintptr_t>(keyboard_layout)));
            input.is_system_key = system;
            content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, view_,
                [input](content::WebViewHost* view) {
                    view->onKeyboardEvent(input);
                });
            return true;
        }
#endif
        if ((*type == "keyDown" || *type == "keyUp" || *type == "char") &&
            event.FindInt("nativeKeyCode") && event.FindInt("domCode") &&
            event.FindInt("domKey")) {
            auto event_type = *type == "keyDown" ? blink::WebInputEvent::Type::kRawKeyDown
                : *type == "keyUp" ? blink::WebInputEvent::Type::kKeyUp
                                  : blink::WebInputEvent::Type::kChar;
            auto input = content::PlatformEventHandler::buildKeyboardEvent(
                event_type, 0, *type == "char" ? event.FindInt("charCode").value_or(key) : key,
                0, flags);
            input.native_key_code = *event.FindInt("nativeKeyCode");
            input.dom_code = *event.FindInt("domCode");
            input.dom_key = static_cast<uint32_t>(*event.FindInt("domKey"));
            input.is_system_key = system;
            auto copy_text = [](const std::string* text, auto& destination) {
                if (!text)
                    return;
                const std::u16string utf16 = base::UTF8ToUTF16(*text);
                std::fill(destination.begin(), destination.end(), 0);
                std::copy_n(utf16.begin(), std::min(utf16.size(), destination.size()),
                            destination.begin());
            };
            copy_text(event.FindString("text"), input.text);
            copy_text(event.FindString("unmodifiedText"), input.unmodified_text);
            // The legacy Windows builder guesses keypad from virtual keys.
            // Native DOM code distinguishes keypad Enter/arrows from navigation keys.
            int modifiers = input.GetModifiers() &
                ~(blink::WebInputEvent::kIsKeyPad | blink::WebInputEvent::kIsLeft |
                  blink::WebInputEvent::kIsRight);
            const auto location = ui::KeycodeConverter::DomCodeToLocation(
                static_cast<ui::DomCode>(input.dom_code));
            if (location == ui::DomKeyLocation::NUMPAD)
                modifiers |= blink::WebInputEvent::kIsKeyPad;
            else if (location == ui::DomKeyLocation::LEFT)
                modifiers |= blink::WebInputEvent::kIsLeft;
            else if (location == ui::DomKeyLocation::RIGHT)
                modifiers |= blink::WebInputEvent::kIsRight;
            input.SetModifiers(modifiers);
            content::ThreadCall::callBlinkThreadAsyncWithValid(FROM_HERE, view_,
                [input](content::WebViewHost* view) { view->onKeyboardEvent(input); });
            return true;
        }
        if (*type == "keyDown")
            return !!mini_electron_fire_key_down_event(view_, key, flags, system);
        if (*type == "keyUp")
            return !!mini_electron_fire_key_up_event(view_, key, flags, system);
        if (*type == "char")
            return !!mini_electron_fire_key_press_event(view_,
                static_cast<unsigned>(event.FindInt("charCode").value_or(key)), flags, system);
        return false;
    }

    void DispatchGuest(const std::string& method, const base::Value::Dict& params)
    {
        int id = params.FindInt("guestContentsId").value_or(0);
        if (id <= 0)
            return;
        std::lock_guard<std::mutex> lock(guest_lock_);
        auto found = guests_.find(id);
        if (method == "guest.detach") {
            if (found != guests_.end()) {
                ReleaseGuestFrame(&found->second);
                guests_.erase(found);
            }
            return;
        }
        if (found == guests_.end()) {
            if (method != "guest.attach" || guests_.size() >= kMaxGuestFrames)
                return;
            found = guests_.try_emplace(id).first;
        }
        GuestFrame& guest = found->second;
        auto update_bounds = [&] {
            guest.x = params.FindInt("x").value_or(guest.x);
            guest.y = params.FindInt("y").value_or(guest.y);
            guest.width = std::clamp(
                params.FindInt("width").value_or(guest.width), 0, 1 << 20);
            guest.height = std::clamp(
                params.FindInt("height").value_or(guest.height), 0, 1 << 20);
        };
        if (method == "guest.attach") {
            guest.attached = true;
            update_bounds();
        } else if (method == "guest.bounds") {
            update_bounds();
        } else if (method == "guest.focus") {
            for (auto& [other_id, other] : guests_)
                other.focused = false;
            guest.focused = params.FindBool("focused").value_or(false);
        }
    }

    int HitTestGuest(int x, int y, const std::string& type)
    {
        std::lock_guard<std::mutex> lock(guest_lock_);
        if (type == "keyDown" || type == "keyUp" || type == "char" || type == "editCommand"
            || type == "imeSetComposition" || type == "imeCommitText"
            || type == "imeFinishComposition") {
            for (auto& [id, guest] : guests_) {
                if (guest.attached && guest.focused)
                    return id;
            }
            return 0;
        }
        for (auto& [id, guest] : guests_) {
            int64_t right = static_cast<int64_t>(guest.x) + guest.width;
            int64_t bottom = static_cast<int64_t>(guest.y) + guest.height;
            if (guest.attached && x >= guest.x && y >= guest.y
                && static_cast<int64_t>(x) < right
                && static_cast<int64_t>(y) < bottom)
                return id;
        }
        return 0;
    }

#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
    void EnsureCdpService(std::function<void(content::RendererCdpService*)> callback)
    {
        PostBlink([this, callback = std::move(callback)]() mutable {
            if (!cdp_service_) {
                content::WebViewHost* host = View();
                if (!host) {
                    callback(nullptr);
                    return;
                }
                cdp_service_ = std::make_unique<content::RendererCdpService>(host,
                    [this](std::string method, base::Value::Dict params) {
                        base::Value::Dict payload;
                        payload.Set("method", std::move(method));
                        payload.Set("params", std::move(params));
                        SendEvent("cdp-message", std::move(payload));
                    }, [this](std::string reason) {
                        cdp_attached_.store(false);
                        {
                            std::lock_guard<std::mutex> lock(dialog_lock_);
                            if (dialog_active_ && !dialog_answered_) {
                                dialog_accepted_ = false;
                                dialog_answered_ = true;
                            }
                        }
                        dialog_cv_.notify_all();
                        base::Value::Dict payload;
                        payload.Set("reason", std::move(reason));
                        SendEvent("cdp-detached", std::move(payload));
                    });
                cdp_service_->SetUploadBroker([this](std::vector<std::string> paths,
                    content::RendererCdpService::UploadBrokerReply reply) {
                    AuthorizeUploads(std::move(paths), std::move(reply));
                });
            }
            callback(cdp_service_.get());
        });
    }

    void DispatchCdp(const std::string& method, base::Value::Dict params,
        uint64_t request_id)
    {
        if (method == "cdp.dispatch") {
            std::string* command = params.FindString("method");
            base::Value::Dict command_params;
            if (base::Value::Dict* value = params.FindDict("params"))
                command_params = value->Clone();
            if (command && *command == "Page.handleJavaScriptDialog") {
                DispatchBrowserCdp(0, *command, command_params, request_id);
                return;
            }
        }
        EnsureCdpService([this, method, params = std::move(params), request_id](
                             content::RendererCdpService* cdp) mutable {
            if (!cdp) {
                Reply(request_id, {}, "CDP renderer is unavailable");
                return;
            }
            std::string error;
            if (method == "cdp.attach") {
                std::string version;
                if (std::string* value = params.FindString("protocolVersion"))
                    version = *value;
                if (!cdp->Attach(version, &error)) {
                    base::Value::Dict detached;
                    detached.Set("reason", error.empty() ? "CDP attach failed" : error);
                    SendEvent("cdp-detached", std::move(detached));
                    Reply(request_id, {}, std::move(error));
                } else {
                    cdp_attached_.store(true);
                    Reply(request_id);
                }
            } else if (method == "cdp.detach") {
                cdp->Detach("target closed");
                cdp_attached_.store(false);
                Reply(request_id);
            } else if (method == "cdp.inspectElement") {
                int x = params.FindInt("x").value_or(0);
                int y = params.FindInt("y").value_or(0);
                if (cdp->InspectElement(x, y, &error))
                    Reply(request_id);
                else
                    Reply(request_id, {}, std::move(error));
            } else {
                int id = next_cdp_command_.fetch_add(1, std::memory_order_relaxed);
                std::string* command = params.FindString("method");
                base::Value::Dict command_params;
                if (base::Value::Dict* value = params.FindDict("params"))
                    command_params = value->Clone();
                if (!command || !id) {
                    Reply(request_id, {}, "invalid CDP command");
                    return;
                }
                if (DispatchBrowserCdp(id, *command, command_params, request_id))
                    return;
                if (!cdp->Dispatch(id, *command, std::move(command_params),
                        [this, request_id](base::Value::Dict result,
                            std::string callback_error) {
                            Reply(request_id, std::move(result),
                                std::move(callback_error));
                        }, &error))
                    Reply(request_id, {}, std::move(error));
            }
        });
    }

    bool DispatchBrowserCdp(int command_id, const std::string& method,
        const base::Value::Dict& params, uint64_t request_id)
    {
        base::Value::Dict event;
        if (method == "Input.dispatchMouseEvent") {
            std::string* type = params.FindString("type");
            if (!type)
                Reply(request_id, {}, "missing mouse event type");
            else {
                event.Set("type", *type == "mousePressed" ? "mouseDown"
                    : *type == "mouseReleased" ? "mouseUp"
                    : *type == "mouseWheel" ? "mouseWheel" : "mouseMove");
                event.Set("x", static_cast<int>(params.FindDouble("x").value_or(0)));
                event.Set("y", static_cast<int>(params.FindDouble("y").value_or(0)));
                const std::string* button = params.FindString("button");
                event.Set("button", button && *button == "right" ? 2
                    : button && *button == "middle" ? 1 : 0);
                event.Set("buttons", params.FindInt("buttons").value_or(0));
                event.Set("clickCount", params.FindInt("clickCount").value_or(0));
                event.Set("deltaX", static_cast<int>(params.FindDouble("deltaX").value_or(0)));
                event.Set("deltaY", static_cast<int>(params.FindDouble("deltaY").value_or(0)));
                event.Set("modifiers", params.FindInt("modifiers").value_or(0));
                if (DispatchInput(event))
                    Reply(request_id);
                else
                    Reply(request_id, {}, "native mouse injection failed");
            }
            return true;
        }
        if (method == "Input.dispatchKeyEvent" || method == "Input.insertText") {
            if (method == "Input.insertText") {
                std::string* text = params.FindString("text");
                if (!text) {
                    Reply(request_id, {}, "missing input text");
                    return true;
                }
                View()->onImeText(base::UTF8ToUTF16(*text), true);
            } else {
                std::string* type = params.FindString("type");
                event.Set("type", type && *type == "keyUp" ? "keyUp"
                    : type && *type == "char" ? "char" : "keyDown");
                event.Set("keyCode", params.FindInt("windowsVirtualKeyCode").value_or(0));
                event.Set("charCode", params.FindInt("nativeVirtualKeyCode").value_or(0));
                event.Set("modifiers", params.FindInt("modifiers").value_or(0));
                if (!DispatchInput(event)) {
                    Reply(request_id, {}, "native key injection failed");
                    return true;
                }
            }
            Reply(request_id);
            return true;
        }
        if (method == "Page.handleJavaScriptDialog") {
            bool accept = params.FindBool("accept").value_or(false);
            std::string prompt = params.FindString("promptText")
                ? *params.FindString("promptText") : std::string();
            std::string user_input;
            {
                std::lock_guard<std::mutex> lock(dialog_lock_);
                if (!dialog_active_ || dialog_answered_) {
                    Reply(request_id, {}, "no JavaScript dialog is active");
                    return true;
                }
                dialog_accepted_ = accept;
                dialog_prompt_ = std::move(prompt);
                user_input = dialog_prompt_;
                dialog_answered_ = true;
            }
            dialog_cv_.notify_all();
            base::Value::Dict closed;
            closed.Set("method", "Page.javascriptDialogClosed");
            base::Value::Dict closed_params;
            closed_params.Set("result", accept);
            closed_params.Set("userInput", std::move(user_input));
            closed.Set("params", std::move(closed_params));
            SendEvent("cdp-message", std::move(closed));
            Reply(request_id);
            return true;
        }
        if (method == "Page.captureScreenshot") {
            BeginScreenshot(params, request_id);
            return true;
        }
        return false;
    }
#endif
    void BeginScreenshot(const base::Value::Dict& params, uint64_t request_id)
    {
        std::string format = params.FindString("format")
            ? *params.FindString("format") : "png";
        if (format != "png" && format != "jpeg") {
            Reply(request_id, {}, "unsupported screenshot format");
            return;
        }
        content::WebViewHost* host = View();
        blink::WebFrame* frame = host ? host->getMainFrame() : nullptr;
        blink::WebLocalFrame* main_frame = frame && frame->IsWebLocalFrame()
            ? frame->ToWebLocalFrame() : nullptr;
        if (!main_frame) {
            Reply(request_id, {}, "page has no local main frame");
            return;
        }
        int original_width = std::max(1, mini_electron_get_content_width(view_));
        int original_height = std::max(1, mini_electron_get_content_height(view_));
        double x = 0;
        double y = 0;
        double width = original_width;
        double height = original_height;
        double scale = 1;
        if (const base::Value::Dict* clip = params.FindDict("clip")) {
            x = clip->FindDouble("x").value_or(0);
            y = clip->FindDouble("y").value_or(0);
            width = clip->FindDouble("width").value_or(width);
            height = clip->FindDouble("height").value_or(height);
            scale = clip->FindDouble("scale").value_or(1);
        } else if (params.FindBool("captureBeyondViewport").value_or(false)) {
            blink::WebElement root = main_frame->GetDocument().DocumentElement();
            if (!root.IsNull()) {
                gfx::Size scroll = root.GetScrollSize();
                width = std::max(width, static_cast<double>(scroll.width()));
                height = std::max(height, static_cast<double>(scroll.height()));
            }
        }
        int capture_width = static_cast<int>(std::ceil(width));
        int capture_height = static_cast<int>(std::ceil(height));
        int output_width = static_cast<int>(std::ceil(width * scale));
        int output_height = static_cast<int>(std::ceil(height * scale));
        if (x < 0 || y < 0 || capture_width <= 0 || capture_height <= 0
            || output_width <= 0 || output_height <= 0
            || capture_width > 16384 || capture_height > 16384
            || output_width > 16384 || output_height > 16384
            || static_cast<uint64_t>(output_width) * output_height
                > 128ULL * 1024 * 1024) {
            Reply(request_id, {}, "screenshot dimensions exceed compositor limits");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(screenshot_lock_);
            if (pending_screenshot_) {
                Reply(request_id, {}, "another screenshot is pending");
                return;
            }
            pending_screenshot_ = std::make_unique<PendingScreenshot>();
            pending_screenshot_->request_id = request_id;
            pending_screenshot_->width = capture_width;
            pending_screenshot_->height = capture_height;
            pending_screenshot_->output_width = output_width;
            pending_screenshot_->output_height = output_height;
            pending_screenshot_->original_width = original_width;
            pending_screenshot_->original_height = original_height;
            pending_screenshot_->quality = std::clamp(
                params.FindInt("quality").value_or(80), 0, 100);
            pending_screenshot_->format = std::move(format);
        }
        std::string script = "window.__miniElectronCaptureScroll=[scrollX,scrollY];"
            "scrollTo(" + base::NumberToString(x) + ","
            + base::NumberToString(y) + ");";
        main_frame->ExecuteScript(blink::WebScriptSource(
            blink::WebString::FromUTF8(script)));
        mini_electron_resize(view_, capture_width, capture_height);
        mini_electron_wake(view_);
    }

    void CompleteScreenshot(const void* buffer, int width, int height)
    {
        std::unique_ptr<PendingScreenshot> pending;
        {
            std::lock_guard<std::mutex> lock(screenshot_lock_);
            if (!pending_screenshot_
                || pending_screenshot_->width != width
                || pending_screenshot_->height != height)
                return;
            pending = std::move(pending_screenshot_);
        }
        const auto* source = static_cast<const uint8_t*>(buffer);
        constexpr size_t red_byte =
            kN32_SkColorType == kRGBA_8888_SkColorType ? 0 : 2;
        constexpr size_t blue_byte = 2 - red_byte;
        std::vector<uint8_t> pixels(
            static_cast<size_t>(pending->output_width)
                * pending->output_height * 4);
        for (int output_y = 0; output_y < pending->output_height; ++output_y) {
            int source_y = output_y * height / pending->output_height;
            for (int output_x = 0; output_x < pending->output_width; ++output_x) {
                int source_x = output_x * width / pending->output_width;
                size_t from = (static_cast<size_t>(source_y) * width + source_x) * 4;
                size_t to = (static_cast<size_t>(output_y)
                    * pending->output_width + output_x) * 4;
                pixels[to] = source[from + red_byte];
                pixels[to + 1] = source[from + 1];
                pixels[to + 2] = source[from + blue_byte];
                pixels[to + 3] = source[from + 3];
            }
        }
        std::optional<std::vector<uint8_t>> encoded;
        if (pending->format == "jpeg") {
            SkImageInfo info = SkImageInfo::Make(pending->output_width,
                pending->output_height, kRGBA_8888_SkColorType,
                kUnpremul_SkAlphaType);
            SkPixmap pixmap(info, pixels.data(),
                static_cast<size_t>(pending->output_width) * 4);
            encoded = gfx::JPEGCodec::Encode(pixmap, pending->quality);
        } else {
            encoded = gfx::PNGCodec::Encode(pixels.data(),
                gfx::PNGCodec::FORMAT_RGBA,
                gfx::Size(pending->output_width, pending->output_height),
                pending->output_width * 4, false, {});
        }
        content::WebViewHost* host = View();
        blink::WebFrame* frame = host ? host->getMainFrame() : nullptr;
        if (frame && frame->IsWebLocalFrame()) {
            frame->ToWebLocalFrame()->ExecuteScript(blink::WebScriptSource(
                blink::WebString::FromUTF8(
                    "if(window.__miniElectronCaptureScroll){"
                    "scrollTo(...window.__miniElectronCaptureScroll);"
                    "delete window.__miniElectronCaptureScroll;}")));
        }
        mini_electron_resize(view_, pending->original_width,
            pending->original_height);
        if (!encoded) {
            Reply(pending->request_id, {}, "screenshot encoding failed");
            return;
        }
        base::Value::Dict result;
        result.Set("data", base::Base64Encode(*encoded));
        Reply(pending->request_id, std::move(result));
    }

    void RevokeAuthorizedUploads(
        uint64_t frame_id, const base::Value::List& files)
    {
        base::Value::List tokens;
        for (const base::Value& value : files) {
            const base::Value::Dict* metadata = value.GetIfDict();
            const std::string* token = metadata
                ? metadata->FindString("token") : nullptr;
            if (token && !token->empty())
                tokens.Append(*token);
        }
        if (tokens.empty())
            return;
        base::Value::Dict request;
        request.Set("kind", "file");
        request.Set("operation", "revoke");
        request.Set("tokens", std::move(tokens));
        auto frame = reinterpret_cast<mini_electron_web_frame_handle>(
            static_cast<uintptr_t>(frame_id));
        RequestParent("broker-request", std::move(request),
            [](base::Value::Dict, std::string) {}, frame);
    }

    void MaterializeChosenFiles(uint64_t frame_id, uint64_t document_epoch,
        base::Value::Dict result, std::string error,
        content::RendererFileChooserReply reply)
    {
        if (!error.empty()) {
            PostBlink([this, frame_id, document_epoch,
                          reply = std::move(reply),
                          error = std::move(error)]() mutable {
                if (!IsCurrentFrameDocument(frame_id, document_epoch))
                    error = "file chooser document was replaced";
                reply({}, std::move(error));
            });
            return;
        }
        if (result.FindBool("canceled").value_or(false)) {
            PostBlink([this, frame_id, document_epoch,
                          reply = std::move(reply)]() mutable {
                if (!IsCurrentFrameDocument(frame_id, document_epoch)) {
                    reply({}, "file chooser document was replaced");
                    return;
                }
                reply(base::Value::Dict().Set("canceled", true), {});
            });
            return;
        }
        base::Value::List* response_files = result.FindList("files");
        if (!response_files) {
            PostBlink([this, frame_id, document_epoch,
                          reply = std::move(reply)]() mutable {
                if (!IsCurrentFrameDocument(frame_id, document_epoch)) {
                    reply({}, "file chooser document was replaced");
                    return;
                }
                reply({}, "file chooser returned no file metadata");
            });
            return;
        }
        auto source = std::make_shared<base::Value::List>(
            std::move(*response_files));
        auto output = std::make_shared<base::Value::List>();
        auto index = std::make_shared<size_t>(0);
        auto next = std::make_shared<std::function<void(std::string)>>();
        *next = [this, frame_id, document_epoch, source, output, index, next,
                    reply = std::move(reply)](std::string next_error) mutable {
            if (!IsCurrentFrameDocument(frame_id, document_epoch))
                next_error = "file chooser document was replaced";
            if (!next_error.empty()) {
                RevokeAuthorizedUploads(frame_id, *source);
                auto completed_reply = std::move(reply);
                *next = {};
                completed_reply({}, std::move(next_error));
                return;
            }
            if (*index == source->size()) {
                base::Value::Dict completed;
                completed.Set("canceled", false);
                completed.Set("files", std::move(*output));
                auto completed_reply = std::move(reply);
                *next = {};
                completed_reply(std::move(completed), {});
                return;
            }
            base::Value::Dict* metadata = (*source)[(*index)++].GetIfDict();
            const std::string* token =
                metadata ? metadata->FindString("token") : nullptr;
            std::optional<int> size =
                metadata ? metadata->FindInt("size") : std::nullopt;
            const std::string* name =
                metadata ? metadata->FindString("name") : nullptr;
            const std::string* path =
                metadata ? metadata->FindString("path") : nullptr;
            const std::string* relative_path =
                metadata ? metadata->FindString("relativePath") : nullptr;
            bool valid_token = token && !token->empty() && token->size() <= 256
                && std::all_of(token->begin(), token->end(), [](char c) {
                    return (c >= 'a' && c <= 'z')
                        || (c >= 'A' && c <= 'Z')
                        || (c >= '0' && c <= '9') || c == '-' || c == '_';
                });
            if (!valid_token || !size || *size < 0 || !name || name->empty()
                || !path || path->empty()
                || (relative_path && !relative_path->empty()
                    && !IsSafeRelativePath(*relative_path))) {
                (*next)("file chooser returned invalid file metadata");
                return;
            }
            std::string token_copy = *token;
            std::string name_copy = *name;
            std::string path_copy = *path;
            std::string virtual_path = relative_path && !relative_path->empty()
                ? *relative_path
                : name_copy;
            double last_modified =
                metadata->FindDouble("lastModified").value_or(0);
            ReadAuthorizedUpload(frame_id, *token, *size,
                [this, frame_id, document_epoch, output, next,
                    token = std::move(token_copy),
                    name = std::move(name_copy), path = std::move(path_copy),
                    virtual_path = std::move(virtual_path),
                    size = *size, last_modified](
                    std::vector<uint8_t> bytes,
                    std::string read_error) mutable {
                    PostBlink([this, frame_id, document_epoch, output, next,
                        token = std::move(token),
                        name = std::move(name), path = std::move(path),
                        virtual_path = std::move(virtual_path),
                        size, last_modified, bytes = std::move(bytes),
                        read_error = std::move(read_error)]() mutable {
                        if (!IsCurrentFrameDocument(frame_id, document_epoch)) {
                            (*next)("file chooser document was replaced");
                            return;
                        }
                        if (!read_error.empty()) {
                            (*next)(std::move(read_error));
                            return;
                        }
                        std::string broker_url = GURL(
                            "mini-electron-broker://" + token + "/"
                            + base::EscapePath(virtual_path)).spec();
                        content::RegisterBrokeredFile(broker_url, name,
                            std::move(bytes), path, frame_id);
                        base::Value::Dict item;
                        item.Set("brokerUrl", std::move(broker_url));
                        item.Set("name", std::move(name));
                        item.Set("relativePath", std::move(virtual_path));
                        item.Set("size", size);
                        item.Set("lastModified", last_modified);
                        output->Append(std::move(item));
                        (*next)({});
                    });
                });
        };
        PostBlink([next] { (*next)({}); });
    }


    void ReadAuthorizedUpload(uint64_t frame_id, const std::string& token, int size,
        std::function<void(std::vector<uint8_t>, std::string)> callback)
    {
        auto bytes = std::make_shared<std::vector<uint8_t>>();
        bytes->reserve(static_cast<size_t>(size));
        auto offset = std::make_shared<int>(0);
        auto started = std::make_shared<bool>(false);
        auto read_next =
            std::make_shared<std::function<void(std::string)>>();
        *read_next = [this, frame_id, token, size, bytes, offset, started,
                         read_next, callback = std::move(callback)](
                         std::string error) mutable {
            if (!error.empty()) {
                auto completed_callback = std::move(callback);
                *read_next = {};
                completed_callback({}, std::move(error));
                return;
            }
            if (*offset == size && (size != 0 || *started)) {
                auto completed_callback = std::move(callback);
                auto completed_bytes = std::move(*bytes);
                *read_next = {};
                completed_callback(std::move(completed_bytes), {});
                return;
            }
            constexpr int kChunkSize = 8 * 1024 * 1024;
            int length = std::min(kChunkSize, size - *offset);
            *started = true;
            base::Value::Dict read;
            read.Set("kind", "file");
            read.Set("operation", "read");
            read.Set("token", token);
            read.Set("offset", *offset);
            read.Set("length", length);
            if (!RequestParent("broker-request", std::move(read),
                    [bytes, offset, length, read_next](
                        base::Value::Dict result,
                        std::string read_error) mutable {
                        base::Value::BlobStorage* body =
                            result.FindBlob("data");
                        if (!read_error.empty() || !body
                            || body->size()
                                != static_cast<size_t>(length)) {
                            (*read_next)(read_error.empty()
                                    ? "file broker returned an incomplete upload"
                                    : std::move(read_error));
                            return;
                        }
                        bytes->insert(bytes->end(),
                            body->begin(), body->end());
                        *offset += length;
                        (*read_next)({});
                    }, reinterpret_cast<mini_electron_web_frame_handle>(
                        static_cast<uintptr_t>(frame_id)))) {
                (*read_next)("file broker channel is closed");
            }
        };
        (*read_next)({});
    }

#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
    void AuthorizeUploads(std::vector<std::string> paths,
        content::RendererCdpService::UploadBrokerReply reply)
    {
        auto output = std::make_shared<std::vector<std::string>>();
        auto paths_shared = std::make_shared<std::vector<std::string>>(std::move(paths));
        auto index = std::make_shared<size_t>(0);
        auto next = std::make_shared<std::function<void(std::string)>>();
        *next = [this, output, paths_shared, index, next,
                    reply = std::move(reply)](std::string error) mutable {
            if (!error.empty()) {
                auto completed_reply = std::move(reply);
                *next = {};
                completed_reply({}, std::move(error));
                return;
            }
            if (*index == paths_shared->size()) {
                auto completed_reply = std::move(reply);
                auto completed_files = std::move(*output);
                *next = {};
                completed_reply(std::move(completed_files), {});
                return;
            }
            base::Value::Dict request;
            request.Set("kind", "file");
            request.Set("operation", "authorize-upload");
            request.Set("requestedPath", (*paths_shared)[(*index)++]);
            if (!RequestParent("broker-request", std::move(request),
                [this, output, next](base::Value::Dict authorized,
                    std::string error) {
                    const std::string* token = authorized.FindString("token");
                    std::optional<int> size = authorized.FindInt("size");
                    const std::string* display_name =
                        authorized.FindString("displayName");
                    const std::string* path = authorized.FindString("path");
                    if (!error.empty() || !token || token->empty() || !size
                        || *size < 0 || !display_name || !path) {
                        PostBlink([next, error = std::move(error)]() mutable {
                            (*next)(error.empty()
                                    ? "file broker returned invalid upload metadata"
                                    : std::move(error));
                        });
                        return;
                    }
                    const uint64_t frame_id = static_cast<uint64_t>(
                        reinterpret_cast<uintptr_t>(main_frame_));
                    std::string url = GURL("mini-electron-broker://" + *token
                        + "/" + base::EscapePath(*display_name)).spec();
                    ReadAuthorizedUpload(frame_id, *token, *size,
                        [this, frame_id, output, next, url = std::move(url),
                            name = *display_name, path = *path](
                            std::vector<uint8_t> bytes,
                            std::string read_error) mutable {
                            if (!read_error.empty()) {
                                PostBlink([next,
                                    read_error = std::move(read_error)]() mutable {
                                    (*next)(std::move(read_error));
                                });
                                return;
                            }
                            PostBlink([frame_id, output, next, url = std::move(url),
                                name = std::move(name), path = std::move(path),
                                bytes = std::move(bytes)]() mutable {
                                content::RegisterBrokeredFile(url, name,
                                    std::move(bytes), path, frame_id);
                                output->push_back(std::move(url));
                                (*next)({});
                            });
                        });
                }, main_frame_)) {
                PostBlink([next] {
                    (*next)("file broker channel is closed");
                });
            }
        };
        (*next)({});
    }
#endif

    void CompletePreparation(uint64_t request_id, bool success)
    {
        if (!pending_preparations_.erase(request_id))
            return;
        if (!success || closing_) {
            Reply(request_id, {}, "renderer presentation is unavailable");
            return;
        }
        // Software DrawAck follows PaintUpdated publication and precedes
        // successful presentation feedback. Read only on the publication owner.
        const uint64_t generation = frame_->generation.load(std::memory_order_acquire);
        if (!generation || (generation & 1) || !frame_->data_size) {
            Reply(request_id, {}, "renderer frame publication failed");
            return;
        }
        base::Value::Dict result;
        result.Set("generation", base::NumberToString(generation));
        Reply(request_id, std::move(result));
    }

    void FailPreparations(const char* error)
    {
        auto pending = std::move(pending_preparations_);
        pending_preparations_.clear();
        for (uint64_t request_id : pending)
            Reply(request_id, {}, error);
    }

    void Shutdown()
    {
        if (closing_.exchange(true))
            return;
        FailPreparations("renderer is shutting down");
        dialog_cv_.notify_all();
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
        if (cdp_service_)
            PostBlink([this] { cdp_service_->Detach("target closed"); });
#endif
        if (run_loop_)
            run_loop_->Quit();
    }

    void StartControlThread()
    {
        control_thread_ = std::thread([this] { ControlLoop(); });
    }

    void StopControlThread()
    {
#if BUILDFLAG(IS_WIN)
        if (arguments_.control) {
            ::CloseHandle(arguments_.control);
            arguments_.control = nullptr;
        }
#else
        if (arguments_.control >= 0) {
            ::shutdown(arguments_.control, SHUT_RDWR);
            ::close(arguments_.control);
            arguments_.control = -1;
        }
#endif
        if (control_thread_.joinable())
            control_thread_.join();
    }

    void ControlLoop()
    {
        while (!closing_) {
            renderer_protocol::FrameControlMessage message;
#if BUILDFLAG(IS_WIN)
            DWORD read = 0;
            if (!::ReadFile(arguments_.control, &message, sizeof(message), &read, nullptr)
                || read != sizeof(message))
                return;
            HANDLE handle = reinterpret_cast<HANDLE>(
                static_cast<uintptr_t>(message.frame_handle));
            if (message.magic != renderer_protocol::kControlMagic
                || message.guest_contents_id <= 0
                || message.frame_mapping_size
                    < sizeof(renderer_protocol::FrameBufferHeader)
                || message.frame_mapping_size
                    > sizeof(renderer_protocol::FrameBufferHeader)
                        + renderer_protocol::kMaxFrameBytes) {
                ::CloseHandle(handle);
                continue;
            }
            void* memory = ::MapViewOfFile(handle, FILE_MAP_READ, 0, 0,
                static_cast<size_t>(message.frame_mapping_size));
            if (!memory) {
                ::CloseHandle(handle);
                continue;
            }
#else
            iovec vector { &message, sizeof(message) };
            char ancillary[CMSG_SPACE(sizeof(int))] = {};
            msghdr received {};
            received.msg_iov = &vector;
            received.msg_iovlen = 1;
            received.msg_control = ancillary;
            received.msg_controllen = sizeof(ancillary);
            if (::recvmsg(arguments_.control, &received, 0) != sizeof(message))
                return;
            cmsghdr* header = CMSG_FIRSTHDR(&received);
            if (!header || header->cmsg_level != SOL_SOCKET
                || header->cmsg_type != SCM_RIGHTS)
                continue;
            int handle = -1;
            std::memcpy(&handle, CMSG_DATA(header), sizeof(handle));
            if (message.magic != renderer_protocol::kControlMagic
                || message.guest_contents_id <= 0
                || message.frame_mapping_size
                    < sizeof(renderer_protocol::FrameBufferHeader)
                || message.frame_mapping_size
                    > sizeof(renderer_protocol::FrameBufferHeader)
                        + renderer_protocol::kMaxFrameBytes) {
                ::close(handle);
                continue;
            }
            void* memory = ::mmap(nullptr, message.frame_mapping_size, PROT_READ,
                MAP_SHARED, handle, 0);
            if (memory == MAP_FAILED) {
                ::close(handle);
                continue;
            }
#endif
            std::lock_guard<std::mutex> lock(guest_lock_);
            auto found = guests_.find(message.guest_contents_id);
            if (found == guests_.end()) {
                if (guests_.size() >= kMaxGuestFrames) {
#if BUILDFLAG(IS_WIN)
                    ::UnmapViewOfFile(memory);
                    ::CloseHandle(handle);
#else
                    ::munmap(memory, message.frame_mapping_size);
                    ::close(handle);
#endif
                    continue;
                }
                found = guests_.try_emplace(message.guest_contents_id).first;
            }
            GuestFrame& guest = found->second;
            ReleaseGuestFrame(&guest);
            guest.memory = memory;
            guest.mapping_size =
                static_cast<size_t>(message.frame_mapping_size);
#if BUILDFLAG(IS_WIN)
            guest.handle = handle;
#else
            guest.fd = handle;
#endif
        }
    }

    void ReleaseGuestFrame(GuestFrame* guest)
    {
        if (!guest->memory)
            return;
#if BUILDFLAG(IS_WIN)
        ::UnmapViewOfFile(guest->memory);
        ::CloseHandle(guest->handle);
        guest->handle = nullptr;
#else
        ::munmap(guest->memory, guest->mapping_size);
        ::close(guest->fd);
        guest->fd = -1;
#endif
        guest->memory = nullptr;
    }

    void CleanupGuestFrames()
    {
        std::lock_guard<std::mutex> lock(guest_lock_);
        for (auto& [id, guest] : guests_)
            ReleaseGuestFrame(&guest);
        guests_.clear();
    }

    void CompositeGuests(uint8_t* destination, int width, int height, int stride)
    {
        if (!destination || width <= 0 || height <= 0
            || static_cast<int64_t>(stride)
                < static_cast<int64_t>(width) * 4)
            return;
        std::lock_guard<std::mutex> lock(guest_lock_);
        for (auto& [id, guest] : guests_) {
            if (!guest.attached || !guest.memory || guest.width <= 0
                || guest.height <= 0)
                continue;
            auto* source =
                static_cast<renderer_protocol::FrameBufferHeader*>(guest.memory);
            uint64_t generation =
                source->generation.load(std::memory_order_acquire);
            if (generation & 1)
                continue;
            const uint32_t source_width = source->width;
            const uint32_t source_height = source->height;
            const uint32_t source_stride = source->stride;
            const uint32_t data_size = source->data_size;
            const uint32_t capacity = source->capacity;
            const uint64_t minimum_stride =
                static_cast<uint64_t>(source_width) * 4;
            const uint64_t required_size = source_height
                ? static_cast<uint64_t>(source_stride)
                        * (source_height - 1)
                    + minimum_stride
                : 0;
            if (source->magic != renderer_protocol::kFrameMagic
                || !source_width || !source_height
                || source_stride < minimum_stride || data_size > capacity
                || required_size > data_size
                || static_cast<uint64_t>(sizeof(*source)) + capacity
                    > guest.mapping_size)
                continue;
            guest.snapshot.resize(data_size);
            std::memcpy(guest.snapshot.data(), source + 1, data_size);
            if (source->generation.load(std::memory_order_acquire)
                != generation)
                continue;
            int left = std::max(0, guest.x);
            int top = std::max(0, guest.y);
            int right = static_cast<int>(std::min<int64_t>(width,
                static_cast<int64_t>(guest.x) + guest.width));
            int bottom = static_cast<int>(std::min<int64_t>(height,
                static_cast<int64_t>(guest.y) + guest.height));
            for (int y = top; y < bottom; ++y) {
                uint32_t sy = static_cast<uint32_t>(
                    static_cast<int64_t>(y - guest.y) * source_height
                    / guest.height);
                for (int x = left; x < right; ++x) {
                    uint32_t sx = static_cast<uint32_t>(
                        static_cast<int64_t>(x - guest.x) * source_width
                        / guest.width);
                    std::memcpy(destination + y * stride + x * 4,
                        guest.snapshot.data()
                            + sy * source_stride + sx * 4,
                        4);
                }
            }
        }
    }

    bool RunDialog(const char* type, const char* message,
        const char* default_prompt, std::string* prompt)
    {
        const int dialog_id = next_dialog_id_.fetch_add(
            1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(dialog_lock_);
            if (dialog_active_)
                return false;
            dialog_active_ = true;
            dialog_answered_ = false;
            dialog_accepted_ = false;
            dialog_prompt_.clear();
            active_dialog_id_ = dialog_id;
        }
        base::Value::Dict payload;
        payload.Set("type", type);
        payload.Set("message", message ? message : "");
        payload.Set("defaultPrompt", default_prompt ? default_prompt : "");
        payload.Set("dialogId", dialog_id);
        if (cdp_attached_.load()) {
            base::Value::Dict cdp_payload;
            cdp_payload.Set("method", "Page.javascriptDialogOpening");
            base::Value::Dict cdp_params;
            cdp_params.Set("type", type);
            cdp_params.Set("message", message ? message : "");
            cdp_params.Set("defaultPrompt", default_prompt ? default_prompt : "");
            cdp_params.Set("hasBrowserHandler", true);
            cdp_params.Set("dialogId", dialog_id);
            cdp_payload.Set("params", std::move(cdp_params));
            SendEvent("cdp-message", std::move(cdp_payload));
        }
        if (!RequestParent("javascript-dialog-opening", std::move(payload),
                [this, dialog_id](base::Value::Dict result,
                    std::string error) {
                    bool notify = false;
                    bool accepted = false;
                    std::string user_input;
                    {
                        std::lock_guard<std::mutex> lock(dialog_lock_);
                        if (!dialog_active_ || dialog_answered_
                            || active_dialog_id_ != dialog_id) {
                            return;
                        }
                        bool handled = error.empty()
                            && result.FindBool("handled").value_or(false);
                        if (!handled && cdp_attached_.load())
                            return;
                        if (std::optional<int> echoed =
                                result.FindInt("dialogId");
                            echoed && *echoed != dialog_id) {
                            return;
                        }
                        dialog_accepted_ = handled
                            && result.FindBool("accepted").value_or(false);
                        if (std::string* value =
                                result.FindString("promptText")) {
                            dialog_prompt_ = *value;
                        }
                        dialog_answered_ = true;
                        notify = true;
                        accepted = dialog_accepted_;
                        user_input = dialog_prompt_;
                    }
                    if (notify) {
                        dialog_cv_.notify_all();
                        if (cdp_attached_.load()) {
                            base::Value::Dict closed;
                            closed.Set("method",
                                "Page.javascriptDialogClosed");
                            base::Value::Dict closed_params;
                            closed_params.Set("result", accepted);
                            closed_params.Set("userInput",
                                std::move(user_input));
                            closed.Set("params", std::move(closed_params));
                            SendEvent("cdp-message", std::move(closed));
                        }
                    }
                },
                nullptr, true)) {
            std::lock_guard<std::mutex> lock(dialog_lock_);
            dialog_answered_ = true;
            dialog_accepted_ = false;
            dialog_cv_.notify_all();
        }
        std::unique_lock<std::mutex> lock(dialog_lock_);
        dialog_cv_.wait(lock, [this] {
            return dialog_answered_ || closing_.load();
        });
        bool accepted = dialog_answered_ && dialog_accepted_;
        if (prompt)
            *prompt = dialog_prompt_;
        dialog_active_ = false;
        dialog_answered_ = false;
        active_dialog_id_ = 0;
        return accepted;
    }

    static void MINI_ELECTRON_CALL_TYPE Alert(mini_electron_web_view,
        void* parameter, const utf8* message)
    {
        static_cast<RendererServerImpl*>(parameter)->RunDialog(
            "alert", message, "", nullptr);
    }

    static BOOL MINI_ELECTRON_CALL_TYPE Confirm(mini_electron_web_view,
        void* parameter, const utf8* message)
    {
        return static_cast<RendererServerImpl*>(parameter)->RunDialog(
            "confirm", message, "", nullptr);
    }

    static mini_electron_string_ptr MINI_ELECTRON_CALL_TYPE Prompt(
        mini_electron_web_view, void* parameter, const utf8* message,
        const utf8* default_prompt, BOOL* result)
    {
        std::string prompt;
        bool accepted = static_cast<RendererServerImpl*>(parameter)->RunDialog(
            "prompt", message, default_prompt, &prompt);
        *result = accepted;
        if (!accepted)
            return nullptr;
        return mini_electron_create_string_with_copy(
            prompt.data(), prompt.size());
    }

    static void MINI_ELECTRON_CALL_TYPE DidCreateContext(mini_electron_web_view view,
        void* parameter, mini_electron_web_frame_handle frame, void* context,
        int, int world_id)
    {
        auto* self = static_cast<RendererServerImpl*>(parameter);
        uintptr_t key = reinterpret_cast<uintptr_t>(frame);
        bool is_main_frame = !!mini_electron_is_main_frame(view, frame);
        if (is_main_frame)
            self->main_frame_ = frame;
        uint64_t frame_id = static_cast<uint64_t>(key);
        uint64_t parent_frame_id =
            mini_electron_get_parent_frame_id(view, frame);
        {
            std::lock_guard<std::mutex> lock(self->navigation_lock_);
            self->frame_hierarchy_[frame_id] = {
                parent_frame_id, is_main_frame
            };
        }
        if (self->registered_frames_.insert(frame_id).second) {
            self->frame_document_epochs_[frame_id] = 1;
            base::Value::Dict payload;
            payload.Set("frameId", base::NumberToString(frame_id));
            payload.Set("parentFrameId",
                base::NumberToString(parent_frame_id));
            payload.Set("isMainFrame", is_main_frame);
            self->SendEvent("frame-created", std::move(payload), frame);
        }
        if (!is_main_frame && !self->options_.preload_all_frames)
            return;
        auto found = self->preload_hosts_.find(key);
        if (found != self->preload_hosts_.end()
            && world_id == WorldIDs::MAIN_WORLD_ID) {
            self->preload_hosts_.erase(found);
            found = self->preload_hosts_.end();
        }
        if (found == self->preload_hosts_.end()) {
            auto host = std::make_unique<SandboxPreloadHost>(
                self->options_.contents_id,
                self->options_.context_isolation,
                is_main_frame && self->options_.webview_tag,
                self->options_.preload_all_frames, self->options_.preloads,
                self->options_.process_metadata.Clone(),
                [self, frame](std::string type, base::Value::Dict payload) {
                    self->SendEvent(std::move(type), std::move(payload), frame);
                },
                [self, frame](std::string type, base::Value::Dict payload) {
                    return self->RequestParentWithId(std::move(type),
                        std::move(payload),
                        [self, frame](uint64_t id, base::Value::Dict result,
                            std::string error) {
                            PostBlink([self, frame, id, result = std::move(result),
                                          error = std::move(error)]() mutable {
                                auto found = self->preload_hosts_.find(
                                    reinterpret_cast<uintptr_t>(frame));
                                if (found != self->preload_hosts_.end())
                                    found->second->ResolveInvoke(id,
                                        std::move(result), std::move(error));
                            });
                        }, frame);
                });
            found = self->preload_hosts_.emplace(key, std::move(host)).first;
        }
        found->second->DidCreateScriptContext(view, frame,
            *static_cast<v8::Local<v8::Context>*>(context), world_id);
    }

    static BOOL MINI_ELECTRON_CALL_TYPE Navigation(mini_electron_web_view,
        void* parameter, mini_electron_navigation_type navigation_type,
        const utf8* url, uint64_t frame_id, uint64_t parent_frame_id,
        BOOL is_main_frame, const utf8* method)
    {
        auto* self = static_cast<RendererServerImpl*>(parameter);
        std::string url_string = url ? url : "";
        std::string method_string = method && *method ? method : "GET";
        if (!frame_id || !GURL(url_string).is_valid())
            return FALSE;
        if (self->HasPendingNavigation(frame_id, url_string, method_string))
            return TRUE;
        base::Value::Dict payload;
        payload.Set("frameId", base::NumberToString(frame_id));
        payload.Set("parentFrameId", base::NumberToString(parent_frame_id));
        payload.Set("isMainFrame", !!is_main_frame);
        payload.Set("url", url_string);
        payload.Set("method", method_string);
        payload.Set("navigationType", static_cast<int>(navigation_type));
        base::Value::Dict result;
        std::string error;
        auto frame = reinterpret_cast<mini_electron_web_frame_handle>(
            static_cast<uintptr_t>(frame_id));
        if (!self->RequestParentSync("navigation-started",
                std::move(payload), frame, &result, &error))
            return FALSE;
        std::string* token = result.FindString("navigationToken");
        if (!result.FindBool("allow").value_or(false)
            || !token || token->empty())
            return FALSE;
        self->StorePendingNavigation(frame_id, parent_frame_id,
            !!is_main_frame, std::move(url_string),
            std::move(method_string), *token);
        return TRUE;
    }

    static void MINI_ELECTRON_CALL_TYPE FrameUrlChanged(
        mini_electron_web_view, void* parameter,
        mini_electron_web_frame_handle frame, const utf8* url,
        uint64_t parent_frame_id, BOOL is_main_frame, BOOL is_same_document)
    {
        auto* self = static_cast<RendererServerImpl*>(parameter);
        const uint64_t frame_id = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(frame));
        base::Value::Dict payload;
        payload.Set("frameId", base::NumberToString(frame_id));
        payload.Set("url", url ? url : "");
        payload.Set("parentFrameId", base::NumberToString(parent_frame_id));
        payload.Set("isMainFrame", !!is_main_frame);
        if (is_same_document) {
            self->SendEvent(
                "frame-navigated-in-page", std::move(payload), frame);
            return;
        }
        content::ClearBrokeredFilesForFrame(frame_id);
        uint64_t& document_epoch = self->frame_document_epochs_[frame_id];
        document_epoch = document_epoch == std::numeric_limits<uint64_t>::max()
            ? 1 : document_epoch + 1;
        if (is_main_frame)
            self->ResetNativeDragOnBlink();
        self->text_input_state_.reset();
        self->SendEvent("text-input-state", {});
        self->ScheduleTextInputState();
        {
            std::lock_guard<std::mutex> lock(self->navigation_lock_);
            self->frame_hierarchy_[frame_id] = {
                parent_frame_id, !!is_main_frame
            };
        }
        self->SendEvent("frame-committed", std::move(payload), frame);
    }

    static void MINI_ELECTRON_CALL_TYPE FrameDetached(
        mini_electron_web_view, void* parameter,
        mini_electron_web_frame_handle frame)
    {
        auto* self = static_cast<RendererServerImpl*>(parameter);
        const uint64_t frame_id = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(frame));
        {
            std::lock_guard<std::mutex> lock(self->navigation_lock_);
            self->pending_navigations_.erase(frame_id);
            self->frame_hierarchy_.erase(frame_id);
        }
        if (!self->closing_)
            self->preload_hosts_.erase(static_cast<uintptr_t>(frame_id));
        self->registered_frames_.erase(frame_id);
        self->frame_document_epochs_.erase(frame_id);
        content::ClearBrokeredFilesForFrame(frame_id);
        if (frame == self->main_frame_)
            self->ResetNativeDragOnBlink();
        self->text_input_state_.reset();
        self->SendEvent("text-input-state", {});
        self->ScheduleTextInputState();
        self->SendEvent("frame-detached", {}, frame);
    }

    static void MINI_ELECTRON_CALL_TYPE DocumentReady(mini_electron_web_view,
        void* parameter, mini_electron_web_frame_handle frame)
    {
        static_cast<RendererServerImpl*>(parameter)->SendEvent("dom-ready", {}, frame);
    }

    static void MINI_ELECTRON_CALL_TYPE TitleChanged(mini_electron_web_view,
        void* parameter, const utf8* title)
    {
        base::Value::Dict payload;
        payload.Set("title", title ? title : "");
        static_cast<RendererServerImpl*>(parameter)->SendEvent("title-changed", std::move(payload));
    }

    static void MINI_ELECTRON_CALL_TYPE UrlChanged(mini_electron_web_view,
        void* parameter, const utf8* url, BOOL can_go_back, BOOL can_go_forward)
    {
        base::Value::Dict payload;
        payload.Set("url", url ? url : "");
        payload.Set("canGoBack", !!can_go_back);
        payload.Set("canGoForward", !!can_go_forward);
        auto* self = static_cast<RendererServerImpl*>(parameter);
        self->SendEvent("navigation-committed", std::move(payload), self->main_frame_);
    }

    static mini_electron_web_view MINI_ELECTRON_CALL_TYPE CreateView(
        mini_electron_web_view, void* parameter,
        mini_electron_web_frame_handle opener,
        const utf8* disposition, const utf8* url,
        const utf8* frame_name, const mini_electron_window_features* features)
    {
        auto* self = static_cast<RendererServerImpl*>(parameter);
        base::Value::Dict payload;
        payload.Set("url", url ? url : "about:blank");
        payload.Set("frameName", frame_name ? frame_name : "");
        payload.Set("disposition", disposition ? disposition : "new-window");
        if (features) {
            payload.Set("width", features->width);
            payload.Set("height", features->height);
        }
        self->SendEvent("window-open", std::move(payload), opener);
        return NULL_WEBVIEW;
    }

    static void MINI_ELECTRON_CALL_TYPE LoadingFinished(mini_electron_web_view,
        void* parameter, mini_electron_web_frame_handle frame, const utf8* url,
        mini_electron_loading_result result, const utf8* reason)
    {
        auto* self = static_cast<RendererServerImpl*>(parameter);
        base::Value::Dict payload;
        payload.Set("url", url ? url : "");
        if (result == MINI_ELECTRON_LOADING_SUCCEEDED)
            self->SendEvent("navigation-finished", std::move(payload), frame);
        else {
            payload.Set("error", reason ? reason : "navigation failed");
            self->SendEvent("navigation-failed", std::move(payload), frame);
        }
        base::Value::Dict state;
        state.Set("loading", false);
        self->SendEvent("loading-state", std::move(state), frame);
    }

    static void MINI_ELECTRON_CALL_TYPE DraggableRegionsChanged(
        mini_electron_web_view, void* parameter,
        const mini_electron_draggable_region* rects, int rect_count)
    {
        if (rect_count < 0 || (rect_count && !rects))
            return;
        base::Value::List regions;
        regions.reserve(static_cast<size_t>(rect_count));
        for (int index = 0; index < rect_count; ++index) {
            const mini_electron_draggable_region& source = rects[index];
            const int width = source.bounds.right - source.bounds.left;
            const int height = source.bounds.bottom - source.bounds.top;
            if (width <= 0 || height <= 0)
                continue;
            base::Value::Dict region;
            region.Set("x", static_cast<int>(source.bounds.left));
            region.Set("y", static_cast<int>(source.bounds.top));
            region.Set("width", static_cast<int>(width));
            region.Set("height", static_cast<int>(height));
            region.Set("draggable", !!source.draggable);
            regions.Append(std::move(region));
        }
        base::Value::Dict payload;
        payload.Set("regions", std::move(regions));
        static_cast<RendererServerImpl*>(parameter)->SendEvent(
            "draggable-regions", std::move(payload));
    }

    static void MINI_ELECTRON_CALL_TYPE PaintUpdated(mini_electron_web_view,
        void* parameter, const void* buffer, const mini_electron_rect* dirty,
        int width, int height)
    {
        auto* self = static_cast<RendererServerImpl*>(parameter);
        if (!buffer || !self->frame_ || width <= 0 || height <= 0 || width > 16384 || height > 16384) {
            self->FailPreparations("renderer frame publication failed");
            return;
        }
        self->CompleteScreenshot(buffer, width, height);
        size_t stride = static_cast<size_t>(width) * 4;
        size_t size = stride * static_cast<size_t>(height);
        if (size > self->frame_->capacity || sizeof(*self->frame_) + size > self->arguments_.frame_size) {
            self->FailPreparations("renderer frame publication failed");
            return;
        }
        uint64_t generation = self->frame_->generation.load(std::memory_order_relaxed);
        self->frame_->generation.store(generation + 1, std::memory_order_release);
        auto* destination = reinterpret_cast<uint8_t*>(self->frame_ + 1);
        // Skia's native pixels are RGBA on macOS and BGRA on Windows.
        // The shared frame protocol always carries RGBA.
        if constexpr (kN32_SkColorType == kRGBA_8888_SkColorType) {
            std::memcpy(destination, buffer, size);
        } else {
            const auto* source = static_cast<const uint8_t*>(buffer);
            for (size_t pixel = 0; pixel < size; pixel += 4) {
                destination[pixel] = source[pixel + 2];
                destination[pixel + 1] = source[pixel + 1];
                destination[pixel + 2] = source[pixel];
                destination[pixel + 3] = source[pixel + 3];
            }
        }
        self->CompositeGuests(destination, width, height, static_cast<int>(stride));
        self->frame_->width = width;
        self->frame_->height = height;
        self->frame_->stride = static_cast<uint32_t>(stride);
        self->frame_->data_size = static_cast<uint32_t>(size);
        self->frame_->generation.store(generation + 2, std::memory_order_release);
        base::Value::Dict payload;
        payload.Set("generation", static_cast<double>(generation + 2));
        payload.Set("width", width);
        payload.Set("height", height);
        payload.Set("stride", static_cast<int>(stride));
        base::Value::Dict rect;
        if (dirty) {
            rect.Set("x", dirty->x);
            rect.Set("y", dirty->y);
            rect.Set("width", dirty->w);
            rect.Set("height", dirty->h);
        }
        payload.Set("dirty", std::move(rect));
        self->SendEvent("paint", std::move(payload));
    }

    static void MINI_ELECTRON_CALL_TYPE Console(mini_electron_web_view,
        void* parameter, mini_electron_console_level level, const utf8* message,
        const utf8* source, unsigned line, const utf8*)
    {
        base::Value::Dict payload;
        payload.Set("level", static_cast<int>(level));
        payload.Set("message", message ? message : "");
        payload.Set("source", source ? source : "");
        payload.Set("line", static_cast<int>(line));
        static_cast<RendererServerImpl*>(parameter)->SendEvent("console-message", std::move(payload));
    }

    static BOOL MINI_ELECTRON_CALL_TYPE CloseRequested(mini_electron_web_view,
        void* parameter, void*)
    {
        static_cast<RendererServerImpl*>(parameter)->SendEvent("close", {});
        return TRUE;
    }

    static BOOL MINI_ELECTRON_CALL_TYPE LoadUrlBegin(mini_electron_web_view,
        void* parameter, const char* url_text, void* job)
    {
        auto* self = static_cast<RendererServerImpl*>(parameter);
        GURL url(url_text ? url_text : "");
        if (!url.is_valid())
            return FALSE;
        const bool network = url.SchemeIsHTTPOrHTTPS()
            || url.SchemeIsFile();
        if (!network && !self->custom_schemes_.contains(url.scheme()))
            return FALSE;
        mini_electron_net_hold_job_to_asyn_commit(job);
        mini_electron_net_request_info request_info {};
        if (!mini_electron_net_get_request_info(job, &request_info)
            || !request_info.frame_id) {
            mini_electron_net_cancel_request(job);
            return TRUE;
        }
        std::string initiator =
            request_info.initiator ? request_info.initiator : "";
        if (url.SchemeIs("mini-electron-broker")) {
            if (const blink::RawData* bytes =
                    content::FindBrokeredFileBytes(
                        request_info.frame_id, url.spec())) {
                mini_electron_net_set_http_status(job, 200, "OK");
                mini_electron_net_set_data(job, const_cast<char*>(bytes->data()),
                    static_cast<int>(bytes->size()));
                mini_electron_net_continue_job(job);
                return TRUE;
            }
        }
        auto request_frame = reinterpret_cast<mini_electron_web_frame_handle>(
            static_cast<uintptr_t>(request_info.frame_id));
        const char* raw_method =
            mini_electron_net_get_request_method_string(job);
        std::string method_string = raw_method ? raw_method : "GET";
        base::Value::Dict request;
        request.Set("credentialsMode", request_info.credentials_mode);
        request.Set("initiator", std::move(initiator));
        request.Set("requestMode", request_info.request_mode);
        request.Set("resourceTypeId", request_info.resource_type);
        int canonical_resource_type = static_cast<int>(
            mini_electron_net_get_resource_type(job));
        request.Set("resourceType",
            ResourceTypeName(canonical_resource_type));
        request.Set("destination", request_info.destination);
        request.Set("parentFrameId",
            base::NumberToString(request_info.parent_frame_id));
        request.Set("isMainFrame", !!request_info.is_main_frame);
        bool is_document_request = canonical_resource_type
                == MINI_ELECTRON_RESOURCE_TYPE_MAIN_FRAME
            || canonical_resource_type
                == MINI_ELECTRON_RESOURCE_TYPE_SUB_FRAME;
        request.Set("isDocument", is_document_request);
        if (is_document_request) {
            std::string navigation_token = self->TakePendingNavigation(
                request_info.frame_id, url.spec(), method_string);
            if (navigation_token.empty()) {
                mini_electron_net_cancel_request(job);
                return TRUE;
            }
            request.Set("navigationToken", std::move(navigation_token));
        }
        if (url.SchemeIs("mini-electron-broker")) {
            request.Set("kind", "file");
            request.Set("operation", "read");
            request.Set("token", url.host());
            request.Set("offset", 0);
            request.Set("length", 16 * 1024 * 1024);
        } else if (network) {
            request.Set("kind", "network");
            request.Set("operation", "request");
            request.Set("url", url.spec());
            request.Set("method", method_string);
            const char* referrer = mini_electron_net_get_referrer(job);
            request.Set("referrer", referrer ? referrer : "");
            base::Value::Dict headers;
            for (const mini_electron_slist* item =
                    mini_electron_net_get_raw_http_head_in_blink_thread(job);
                 item; item = item->next) {
                std::string line = item->data ? item->data : "";
                size_t separator = line.find(':');
                if (separator == std::string::npos)
                    continue;
                size_t value_start = separator + 1;
                while (value_start < line.size()
                    && (line[value_start] == ' ' || line[value_start] == '\t'))
                    ++value_start;
                headers.Set(line.substr(0, separator),
                    line.substr(value_start));
            }
            request.Set("headers", std::move(headers));
            std::vector<uint8_t> body;
            if (mini_electron_post_body_elements* elements =
                    mini_electron_net_get_post_body(job)) {
                bool unsupported = false;
                for (size_t index = 0; index < elements->elementSize; ++index) {
                    mini_electron_post_body_element* element =
                        elements->element[index];
                    if (!element)
                        continue;
                    if (element->type
                            != mini_electron_http_body_element_type_data
                        || !element->data
                        || (element->data->length > 0
                            && !element->data->data)
                        || element->data->length
                            > renderer_protocol::kMaxMessageBytes
                                - body.size()) {
                        unsupported = true;
                        break;
                    }
                    if (element->data->length > 0) {
                        const auto* first =
                            static_cast<const uint8_t*>(element->data->data);
                        body.insert(body.end(), first,
                            first + element->data->length);
                    }
                }
                mini_electron_net_free_post_body_elements(elements);
                if (unsupported) {
                    mini_electron_net_cancel_request(job);
                    return TRUE;
                }
            }
            request.Set("body", base::Value(std::move(body)));
        } else {
            request.Set("kind", "protocol");
            request.Set("url", url.spec());
            request.Set("method", "GET");
            request.Set("headers", base::Value::Dict());
        }
        const int job_id =
            static_cast<mini_electron::WebURLLoaderInternal*>(job)->m_id;
        if (!self->RequestParent("broker-request", std::move(request),
                [job_id](base::Value::Dict result,
                    std::string error) mutable {
                PostBlink([job_id, result = std::move(result),
                                    error = std::move(error)]() mutable {
                    mini_electron::AutoLockJob lock(
                        mini_electron::WebURLLoaderManager::sharedInstance(), job_id);
                    mini_electron::WebURLLoaderInternal* job = lock.lock();
                    if (!job || job->isCancelled())
                        return;
                    if (!error.empty()) {
                        mini_electron_net_cancel_request(job);
                        return;
                    }
                    int status = result.FindInt("status").value_or(
                        result.FindInt("statusCode").value_or(200));
                    const std::string* status_text =
                        result.FindString("statusText");
                    mini_electron_net_set_http_status(job, status,
                        status_text ? status_text->c_str() : "OK");
                    if (std::string* final_url =
                            result.FindString("finalUrl")) {
                        mini_electron_net_set_response_url(job,
                            final_url->c_str());
                    }
                    if (std::string* mime = result.FindString("mimeType"))
                        mini_electron_net_set_mime_type(job, mime->c_str());
                    if (base::Value::Dict* headers =
                            result.FindDict("headers")) {
                        for (const auto& item : *headers) {
                            if (const std::string* text =
                                    item.second.GetIfString()) {
                                mini_electron_net_set_http_header_field_utf8(
                                    job, item.first.c_str(),
                                    text->c_str(), TRUE);
                            }
                        }
                    }
                    base::Value::BlobStorage* body =
                        result.FindBlob("body");
                    if (!body)
                        body = result.FindBlob("data");
                    if (body) {
                        mini_electron_net_set_data(job, body->data(),
                            static_cast<int>(body->size()));
                    } else {
                        mini_electron_net_set_data(job, nullptr, 0);
                    }
                    mini_electron_net_continue_job(job);
                });
            }, request_frame)) {
            mini_electron_net_cancel_request(job);
        }
        return TRUE;
    }

    ParsedArguments arguments_;
    ServerOptions options_;
    renderer_protocol::FrameBufferHeader* frame_;
    static constexpr size_t kMaxGuestFrames = 256;
    mini_electron_web_view view_ = NULL_WEBVIEW;
    mini_electron_web_frame_handle main_frame_ = nullptr;
    // Accessed only on the Blink thread. Browser generations invalidate stale
    // materialization completions and preserve enter/over/drop ordering.
    uint64_t native_drag_generation_ = 0;
    bool native_drag_ready_ = false;
    blink::WebDragData native_drag_data_;
    std::vector<base::Value::Dict> native_drag_pending_;
    std::vector<std::string> native_drag_broker_urls_;
    // Accessed only on Blink's sequence.
    bool text_input_pending_ = false;
    std::optional<TextInputSnapshot> text_input_state_;
    uint64_t text_input_serial_ = 0;
    scoped_refptr<base::SequencedTaskRunner> owner_runner_;
    base::WeakPtr<RendererServerImpl> owner_weak_;
    // Renderer owner sequence only; compositor callbacks marshal back here.
    std::set<uint64_t> pending_preparations_;
    std::unique_ptr<base::RunLoop> run_loop_;
    std::thread reader_;
    std::thread control_thread_;
    std::mutex write_lock_;
    std::mutex viewport_lock_;
    int viewport_width_ = 0;
    int viewport_height_ = 0;
    float viewport_scale_ = 1.f;
    bool viewport_pending_ = false;
    std::atomic<bool> closing_ { false };
    bool ignore_menu_shortcuts_ = false;
    std::atomic<uint64_t> next_parent_request_ { 1 };
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
    std::atomic<int> next_cdp_command_ { 1 };
#endif
    std::mutex parent_reply_lock_;
    std::map<uint64_t, ParentReplyEntry> parent_replies_;
    std::mutex cancel_lock_;
    std::set<uint64_t> cancelled_;
    std::set<std::string> custom_schemes_;
    std::mutex navigation_lock_;
    std::map<uint64_t, PendingNavigation> pending_navigations_;
    std::map<uint64_t, std::pair<uint64_t, bool>> frame_hierarchy_;
    std::map<uint64_t, uint64_t> frame_document_epochs_;
    std::set<uint64_t> registered_frames_;
    std::map<uintptr_t, std::unique_ptr<SandboxPreloadHost>> preload_hosts_;
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
    std::unique_ptr<content::RendererCdpService> cdp_service_;
#endif
    std::mutex guest_lock_;
    std::map<int, GuestFrame> guests_;
    std::mutex screenshot_lock_;
    std::unique_ptr<PendingScreenshot> pending_screenshot_;
    std::mutex dialog_lock_;
    std::condition_variable dialog_cv_;
    bool dialog_active_ = false;
    bool dialog_answered_ = false;
    bool dialog_accepted_ = false;
    std::atomic<int> next_dialog_id_ { 1 };
    int active_dialog_id_ = 0;
    std::atomic<bool> cdp_attached_ { false };
    std::string dialog_prompt_;
    base::WeakPtrFactory<RendererServerImpl> weak_factory_ { this };
};

template <typename Char>
int RunRenderer(int argc, Char* argv[])
{
    ParsedArguments arguments;
    if (!ParseArguments(argc, argv, &arguments))
        return 120;
    std::string sandbox_error;
    if (!EnterRendererSandbox(&sandbox_error))
        return 121;
    std::string error;
    WireMessage hello;
    if (!renderer_protocol::ReadMessage(arguments.read_pipe, arguments.auth_high,
            arguments.auth_low, &hello, &error)
        || hello.kind != MessageKind::kHello)
        return 122;
    ServerOptions options;
    if (!ParseOptions(std::move(hello.payload), &options))
        return 123;
#if BUILDFLAG(IS_WIN)
    void* frame_memory = ::MapViewOfFile(arguments.frame, FILE_MAP_WRITE, 0, 0,
        arguments.frame_size);
#else
    void* frame_memory = ::mmap(nullptr, arguments.frame_size,
        PROT_READ | PROT_WRITE, MAP_SHARED, arguments.frame, 0);
    if (frame_memory == MAP_FAILED)
        frame_memory = nullptr;
#endif
    if (!frame_memory)
        return 124;
    int result = 0;
    {
        RendererServerImpl server(arguments, std::move(options), frame_memory);
        if (!server.Start(&error))
            result = 125;
        else
            server.Run();
    }
#if BUILDFLAG(IS_WIN)
    ::UnmapViewOfFile(frame_memory);
#else
    ::munmap(frame_memory, arguments.frame_size);
#endif
    renderer_protocol::ClosePipe(arguments.read_pipe);
    renderer_protocol::ClosePipe(arguments.write_pipe);
    return result;
}

} // namespace

bool IsRendererProcess(int argc, const char* const* argv)
{
    for (int i = 1; i < argc; ++i) {
        if (Equals(argv[i], "--type=renderer"))
            return true;
    }
    return false;
}

bool IsRendererProcess(int argc, const wchar_t* const* argv)
{
    for (int i = 1; i < argc; ++i) {
        if (Equals(argv[i], "--type=renderer"))
            return true;
    }
    return false;
}

int RunRendererProcess(int argc, char* argv[])
{
    return RunRenderer(argc, argv);
}

int RunRendererProcess(int argc, wchar_t* argv[])
{
    return RunRenderer(argc, argv);
}

} // namespace atom
