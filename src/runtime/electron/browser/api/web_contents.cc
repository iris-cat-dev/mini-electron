#include "src/runtime/electron/browser/api/web_contents.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "net/base/filename_util.h"
#if defined(_WIN32)
#include "src/runtime/electron/browser/api/app.h"
#endif
#include "src/runtime/electron/browser/api/session.h"
#include "src/runtime/electron/browser/api/window_interface.h"
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
#include "src/runtime/electron/browser/devtools_gateway.h"
#endif
#include "src/runtime/electron/common/api/native_image.h"
#include "src/runtime/electron/common/asar/asar_util.h"
#include "src/runtime/electron/common/gin_helper/arguments.h"
#include "src/runtime/electron/common/gin_helper/converter.h"
#include "src/runtime/electron/common/gin_helper/object_template_builder.h"
#include "src/runtime/electron/common/gin_helper/promise.h"
#include "src/runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "src/runtime/electron/common/gin_helper/wrappable.h"
#include "src/runtime/electron/common/id_live_detect.h"
#include "src/runtime/electron/common/node_register_help.h"
#include "src/runtime/electron/common/renderer_server.h"
#include "src/runtime/electron/node_bindings.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "url/gurl.h"
#include "url/origin.h"
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
#include "base/strings/utf_string_conversions.h"
#include "src/platform/clipboard.h"
#endif

#if BUILDFLAG(IS_WIN)
#include "src/platform/windows/dcomp_presenter.h"
#include "ui/gfx/geometry/rect_conversions.h"
#include "src/runtime/electron/common/renderer_protocol.h"
#endif

namespace atom {
namespace {

std::unordered_map<int, WebContents*>& AllContents()
{
    static std::unordered_map<int, WebContents*> contents;
    return contents;
}

std::string& ApplicationRoot()
{
    static std::string root;
    return root;
}

uint64_t RequestIdFrom(const base::Value::Dict& event)
{
    if (std::optional<int> id = event.FindInt("requestId"))
        return static_cast<uint64_t>(*id);
    if (std::optional<double> id = event.FindDouble("requestId"))
        return static_cast<uint64_t>(*id);
    return 0;
}

const base::Value::Dict& PayloadFrom(const base::Value::Dict& event)
{
    const base::Value::Dict* payload = event.FindDict("payload");
    static const base::Value::Dict empty;
    return payload ? *payload : empty;
}

std::string ReadString(const base::Value::Dict& value, const char* key)
{
    const std::string* result = value.FindString(key);
    return result ? *result : std::string();
}

bool ReadFrameIdentity(const base::Value::Dict& value, const char* key,
    uint64_t* id, bool allow_zero = false)
{
    const std::string* text = value.FindString(key);
    return text && base::StringToUint64(*text, id)
        && base::NumberToString(*id) == *text && (*id || allow_zero);
}

bool SameDocumentUrl(const std::string& left, const std::string& right)
{
    GURL first(left);
    GURL second(right);
    return first.is_valid() && second.is_valid()
        && first.GetWithoutRef() == second.GetWithoutRef();
}

bool IsUnbrokeredDocument(const GURL& url)
{
    return url.IsAboutBlank() || url.IsAboutSrcdoc()
        || url.SchemeIs("data") || url.SchemeIs("blob");
}

std::string NavigationToken()
{
    std::array<uint8_t, 16> bytes;
    base::RandBytes(bytes);
    return base::HexEncode(bytes);
}
bool IsKnownPermission(const std::string& permission)
{
    static const char* const allowed[] = {
        "accessibility-events", "background-sync", "bluetooth",
        "camera-pan-tilt-zoom", "clipboard-read",
        "clipboard-sanitized-write", "display-capture", "fullscreen",
        "geolocation", "hid", "idle-detection", "keyboardLock", "media",
        "mediaKeySystem", "midi", "midiSysex", "notifications",
        "openExternal", "pointerLock", "serial", "storage-access",
        "top-level-storage-access", "usb", "window-management",
        "persistent-storage", "sensors", "payment-handler",
        "background-fetch", "periodic-background-sync", "screen-wake-lock",
        "system-wake-lock", "nfc", "local-fonts", "captured-surface-control",
        "speaker-selection", "web-app-installation"
    };
    return std::find(std::begin(allowed), std::end(allowed), permission)
        != std::end(allowed);
}


bool ReadFile(const std::string& filename, std::string* source)
{
    return !filename.empty()
        && asar::readFileToString(base::FilePath::FromUTF8Unsafe(filename), source);
}

#if defined(_WIN32)
base::Value::Dict InputFromWindows(const char* type, unsigned message, int x, int y,
    double screenX, double screenY, unsigned flags, int delta)
{
    base::Value::Dict event;
    event.Set("type", type);
    event.Set("nativeMessage", static_cast<int>(message));
    event.Set("x", x);
    event.Set("y", y);
    event.Set("screenX", screenX);
    event.Set("screenY", screenY);
    event.Set("modifiers", static_cast<int>(flags));
    if (delta)
        event.Set(message == WM_MOUSEHWHEEL ? "deltaX" : "deltaY", delta);
    return event;
}
#endif

} // namespace

struct WindowOpenHandlerResult {
    bool isDeny = false;
    base::Value::Dict overrideBrowserWindowOptions;
};

void WebContents::init(v8::Isolate* isolate, v8::Local<v8::Object> target, node::Environment*)
{
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Local<v8::FunctionTemplate> prototype = v8::FunctionTemplate::New(isolate, newFunction);
    prototype->SetClassName(v8::String::NewFromUtf8(isolate, "WebContents").ToLocalChecked());

    gin_helper::ObjectTemplateBuilder builder(isolate, prototype->InstanceTemplate());
    builder.SetMethod("getId", &WebContents::getIdApi);
    builder.SetMethod("getProcessId", &WebContents::getProcessIdApi);
    builder.SetMethod("_loadURL", &WebContents::loadURLApi);
    builder.SetMethod("downloadURL", &WebContents::downloadURLApi);
    builder.SetMethod("_getURL", &WebContents::getURLApi);
    builder.SetMethod("getTitle", &WebContents::getTitleApi);
    builder.SetMethod("isLoading", &WebContents::isLoadingApi);
    builder.SetMethod("isLoadingMainFrame", &WebContents::isLoadingMainFrameApi);
    builder.SetMethod("isWaitingForResponse", &WebContents::isWaitingForResponseApi);
    builder.SetMethod("stop", &WebContents::stopApi);
    builder.SetMethod("goBack", &WebContents::goBackApi);
    builder.SetMethod("goForward", &WebContents::goForwardApi);
    builder.SetMethod("goToOffset", &WebContents::goToOffsetApi);
    builder.SetMethod("goToIndex", &WebContents::goToIndexApi);
    builder.SetMethod("reload", &WebContents::reloadApi);
    builder.SetMethod("reloadIgnoringCache", &WebContents::reloadIgnoringCacheApi);
    builder.SetMethod("isCrashed", &WebContents::isCrashedApi);
    builder.SetMethod("setUserAgent", &WebContents::setUserAgentApi);
    builder.SetMethod("getUserAgent", &WebContents::getUserAgentApi);
    builder.SetMethod("focus", &WebContents::focusApi);
    builder.SetMethod("isFocused", &WebContents::isFocusedApi);
    builder.SetMethod("_send", &WebContents::sendApi);
    builder.SetMethod("_request", &WebContents::requestApi);
    builder.SetMethod("_sendCommand", &WebContents::sendCommandApi);
    builder.SetMethod("sendInputEvent", &WebContents::sendInputEventApi);
    builder.SetMethod("capturePage", &WebContents::capturePageApi);
    builder.SetMethod("prepareToShow", &WebContents::prepareToShowApi);
    builder.SetMethod("_respond", &WebContents::respondApi);
    builder.SetMethod("_attachGuest", &WebContents::attachGuestApi);
    builder.SetMethod("_authorizeFileSelection", &WebContents::authorizeFileSelectionApi);
    builder.SetMethod("_getFrame", &WebContents::getFrameApi);
#if BUILDFLAG(IS_WIN)
    builder.SetMethod("_getCompositorInfo", &WebContents::getCompositorInfoApi);
#endif
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
    builder.SetMethod("_debuggerAttach", &WebContents::debuggerAttachApi);
    builder.SetMethod("_debuggerDetach", &WebContents::debuggerDetachApi);
    builder.SetMethod("_debuggerIsAttached", &WebContents::debuggerIsAttachedApi);
    builder.SetMethod("_debuggerSendCommand", &WebContents::debuggerSendCommandApi);
    builder.SetMethod("_openDevTools", &WebContents::openDevToolsApi);
    builder.SetMethod("_closeDevTools", &WebContents::closeDevToolsApi);
    builder.SetMethod("_isDevToolsOpened", &WebContents::isDevToolsOpenedApi);
    builder.SetMethod("_inspectElement", &WebContents::inspectElementApi);
#endif
    builder.SetMethod("invalidate", &WebContents::invalidateApi);
    builder.SetMethod("isGuest", &WebContents::isGuestApi);
    builder.SetMethod("isOffscreen", &WebContents::isOffscreenApi);
    builder.SetMethod("getType", &WebContents::getTypeApi);
    builder.SetMethod("getWebPreferences", &WebContents::getWebPreferencesApi);
    builder.SetMethod("getOwnerBrowserWindow", &WebContents::getOwnerBrowserWindowApi);
    builder.SetMethod("isDestroyed", &WebContents::isDestroyedApi);
    builder.SetMethod("setWindowOpenHandler", &WebContents::setWindowOpenHandlerApi);
    builder.SetMethod("setIgnoreMenuShortcuts", &WebContents::setIgnoreMenuShortcutsApi);
    builder.SetMethod("_destroy", &WebContents::destroyApi);
    builder.SetMethod("_setZoomLevel", &WebContents::setZoomLevelApi);
    builder.SetMethod("_getZoomLevel", &WebContents::getZoomLevelApi);
    builder.SetMethod("setZoomFactor", &WebContents::setZoomFactorApi);
    builder.SetMethod("getZoomFactor", &WebContents::getZoomFactorApi);
    builder.SetMethod("_canGoBack", &WebContents::canGoBackApi);
    builder.SetMethod("_canGoForward", &WebContents::canGoForwardApi);
    builder.SetProperty("id", &WebContents::getIdApi);
    builder.SetProperty("session", &WebContents::getSessionApi);

    v8::Local<v8::Function> constructor = prototype->GetFunction(context).ToLocalChecked();
    gin_helper::Dictionary web_contents_class(isolate, constructor);
    web_contents_class.SetMethod("getFocusedWebContents", &WebContents::getFocusedWebContentsApi);
    web_contents_class.SetMethod("getAllWebContents", &WebContents::getAllWebContentsApi);
    web_contents_class.SetMethod("fromId", &WebContents::fromIdApi);
    web_contents_class.SetMethod("_createGuest", &WebContents::createGuestApi);

    s_constructor.Reset(isolate, constructor);
    target->Set(context, gin_helper::StringToV8(isolate, "WebContents"), constructor).Check();
}

WebContents* WebContents::create(v8::Isolate* isolate, gin_helper::Dictionary options,
    WindowInterface* owner)
{
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Local<v8::Function> constructor = v8::Local<v8::Function>::New(isolate, s_constructor);
    v8::Local<v8::Value> argv[] = { gin_helper::ConvertToV8(isolate, options) };
    v8::MaybeLocal<v8::Object> maybe_object = constructor->NewInstance(context, 1, argv);
    if (maybe_object.IsEmpty())
        return nullptr;
    v8::Local<v8::Object> object = maybe_object.ToLocalChecked();
    WebContents* self = static_cast<WebContents*>(WrappableBase::GetNativePtr(object, &kWrapperInfo));
    if (!self)
        return nullptr;
    self->m_owner = owner;
    self->m_live_self.Reset(isolate, object);
#if defined(_WIN32)
    v8::TryCatch creation_handler_exception(isolate);
    if (App::getInstance())
        App::getInstance()->emit("web-contents-created", object);
    if (creation_handler_exception.HasCaught()) {
        if (!self->isDestroyedApi()) {
            // The original handler exception owns constructor failure.  Catch
            // teardown-listener failures separately so they cannot replace it.
            v8::TryCatch teardown_exception(isolate);
            self->destroyApi();
            if (teardown_exception.HasCaught())
                teardown_exception.Reset();
        }
        creation_handler_exception.ReThrow();
        return nullptr;
    }
#endif
    return self->isDestroyedApi() ? nullptr : self;
}

void WebContents::newFunction(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    if (!args.IsConstructCall()) {
        args.GetIsolate()->ThrowException(v8::Exception::TypeError(
            gin_helper::StringToV8(args.GetIsolate(), "WebContents must be constructed")));
        return;
    }
    gin_helper::Dictionary options = gin_helper::Dictionary::CreateEmpty(args.GetIsolate());
    if (args.Length() && args[0]->IsObject())
        options = gin_helper::Dictionary(args.GetIsolate(), args[0].As<v8::Object>());
    new WebContents(args.GetIsolate(), args.This(), options);
    args.GetReturnValue().Set(args.This());
}

WebContents::WebContents(v8::Isolate* isolate, v8::Local<v8::Object> wrapper,
    const gin_helper::Dictionary& options)
{
    gin_helper::Wrappable<WebContents>::InitWith(isolate, wrapper);
    m_id = IdLiveDetect::get()->constructed(this);
    AllContents()[m_id] = this;

    options.GetBydefaultVal("partition", "", &m_partition);
    if (m_partition.empty())
        options.GetBydefaultVal("session", "", &m_partition);
    options.GetBydefaultVal("type", "window", &m_type);
    // BrowserViews are unattached at construction and must not wait for a
    // browser-side import before becoming hidden. Windows retain one bootstrap
    // publication; non-window readiness remains an internal first-paint state.
    m_ready_to_show_armed = m_type != "window";
    m_initial_paint_pending = m_type != "browserView";

    bool context_isolation = true;
    bool webview_tag = false;
    bool web_security = true;
    bool allow_insecure = false;
    bool preload_all_frames = false;
    bool transparent = false;
    options.GetBydefaultVal("contextIsolation", true, &context_isolation);
    options.GetBydefaultVal("webviewTag", false, &webview_tag);
    options.GetBydefaultVal("webSecurity", true, &web_security);
    options.GetBydefaultVal("allowRunningInsecureContent", false, &allow_insecure);
    options.GetBydefaultVal("transparent", false, &transparent);
    options.GetBydefaultVal("backgroundThrottling", true, &m_background_throttling);
    m_web_preferences.Set("contextIsolation", context_isolation);
    options.GetBydefaultVal("nodeIntegrationInSubFrames", false, &preload_all_frames);
    m_web_preferences.Set("webviewTag", webview_tag);
    m_web_preferences.Set("webSecurity", web_security);
    m_web_preferences.Set("allowRunningInsecureContent", allow_insecure);
    m_web_preferences.Set("nodeIntegrationInSubFrames", preload_all_frames);
    m_web_preferences.Set("backgroundThrottling", m_background_throttling);
    m_web_preferences.Set("sandbox", !IsSingleProcessMode());
    m_web_preferences.Set("nodeIntegration", false);
    if (!ApplicationRoot().empty())
        addApplicationResourceRoot(
            base::FilePath::FromUTF8Unsafe(ApplicationRoot()));

    RendererLaunchOptions launch;
    launch.contents_id = m_id;
#if BUILDFLAG(IS_WIN)
    if (App* app = App::getInstance())
        launch.crash_dump_directory = app->getPathApi("crashDumps");
#endif
    launch.context_isolation = context_isolation;
    launch.webview_tag = webview_tag;
    launch.web_security = web_security;
    launch.allow_running_insecure_content = allow_insecure;
    launch.preload_all_frames = preload_all_frames;
    launch.transparent = transparent;
    launch.initially_visible = m_type != "browserView";
    options.GetBydefaultVal("backgroundColor", "", &launch.background_color);
    launch.preload_process_metadata.Set("sandboxed", !IsSingleProcessMode());
    launch.preload_process_metadata.Set("type", "renderer");
    launch.preload_process_metadata.Set("isGuest", m_type == "webview");
    launch.preload_process_metadata.Set("preloadAllFrames", preload_all_frames);
    gin_helper::Dictionary global(isolate, isolate->GetCurrentContext()->Global());
    gin_helper::Dictionary process(isolate);
    if (global.Get("process", &process)) {
        std::string platform;
        if (process.Get("platform", &platform))
            launch.preload_process_metadata.Set("platform", std::move(platform));
        base::Value::Dict environment;
        if (process.Get("env", &environment))
            launch.preload_process_metadata.Set("env", std::move(environment));
    }

    ApiSession* session = SessionMgr::get()->findOrCreateSession(isolate, m_partition, true);
    if (session) {
        launch.privileged_schemes = session->getPrivilegedSchemes();
        for (const std::string& filename : session->getPreloadsApi()) {
            std::string source;
            if (ReadFile(filename, &source))
                launch.preloads.push_back({ filename, std::move(source) });
        }
    }
    std::string preload;
    options.GetBydefaultVal("preload", "", &preload);
    if (!preload.empty()) {
        std::string source;
        if (ReadFile(preload, &source))
            launch.preloads.push_back({ preload, std::move(source) });
    }

    m_renderer = RendererClient::Create(std::move(launch),
        [this](base::Value::Dict event) { onRendererEvent(std::move(event)); });
    if (!m_renderer) {
        m_crashed = true;
    }
}

WebContents::~WebContents()
{
    closeRenderer();
    unregisterAndNotify();
    if (m_host)
        m_host->m_guest_ids.erase(m_id);
    m_live_self.Reset();
    IdLiveDetect::get()->deconstructed(m_id);
}

void WebContents::setCreateWindowParam(
    BrowserWindowConstructorOptions* options)
{
    m_createWindowParam = options;
    if (options && m_type == "window")
        setVisible(options->isShow);
}

void WebContents::unregisterAndNotify()
{
    auto found = AllContents().find(m_id);
    if (found == AllContents().end())
        return;
    AllContents().erase(found);
    m_observers.Notify(&WebContentsObserver::onWebContentsDeleted, this);
    m_observers.Clear();
}

void WebContents::destroyed()
{
    destroyApi();
}

void WebContents::destroyApi()
{
    if (m_destroyed)
        return;
    m_destroyed = true;
    // Destruction can be reentrant from web-contents-created.  Drop the
    // prospective owner before its constructor can unwind and become collectible.
    m_owner = nullptr;
    std::set<int> guests = m_guest_ids;
    for (int id : guests) {
        WebContents* guest = fromId(id);
        if (guest)
            guest->destroyed();
    }
    m_guest_ids.clear();
    closeRenderer();
    unregisterAndNotify();
    if (m_host) {
        m_host->m_guest_ids.erase(m_id);
        m_host = nullptr;
    }
    if (!m_destroy_event_emitted) {
        m_destroy_event_emitted = true;
        mate::EventEmitter<WebContents>::emit("destroyed");
    }
    m_live_self.Reset();
}

void WebContents::closeRenderer()
{
    cancelPreparedFrames("Renderer closed before native presentation");
    m_observers.Notify(&WebContentsObserver::onWebContentsTextInputState, this, base::Value::Dict());
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
    m_devtools_gateway.reset();
    m_debugger_attached = false;
#endif
    ApiSession* session = SessionMgr::get()->findOrCreateSession(
        isolate(), m_partition, false);
    if (session)
        session->onWebContentsDestroyed(m_id);
    m_renderer_frames.clear();
    m_file_chooser_frames.clear();
    m_main_frame_id = 0;
    m_pending_main_navigation = {};
#if defined(_WIN32)
    m_windowsPaintPixelsGeneration = 0;
#endif
    if (m_renderer) {
        m_renderer->Close();
        m_renderer.reset();
    }
}

bool WebContents::isAlive() const
{
    return !m_destroyed && m_renderer && m_renderer->IsAlive();
}

bool WebContents::isDestroyedApi() const
{
    return m_destroyed;
}

void WebContents::addObserver(WebContentsObserver* observer)
{
    if (observer && !m_observers.HasObserver(observer))
        m_observers.AddObserver(observer);
}

void WebContents::removeObserver(WebContentsObserver* observer)
{
    if (observer)
        m_observers.RemoveObserver(observer);
    if (std::none_of(m_observers.begin(), m_observers.end(),
            [](const WebContentsObserver& remaining) { return remaining.supportsFramePreparation(); }))
        cancelPreparedFrames("Native presentation host was removed");
}

WebContents* WebContents::fromId(int id)
{
    auto found = AllContents().find(id);
    return found == AllContents().end() || found->second->isDestroyedApi() ? nullptr : found->second;
}

bool WebContents::sendRendererMessage(int contents_id,
    const std::string& method, base::Value::Dict params)
{
    WebContents* contents = fromId(contents_id);
    if (!contents || !contents->isAlive() || !contents->m_renderer)
        return false;
    return contents->m_renderer->Send(method, std::move(params));
}

void WebContents::setApplicationRoot(const std::string& path)
{
    ApplicationRoot() = path;
}

void WebContents::addApplicationResourceRoot(const base::FilePath& path)
{
    base::FilePath canonical = base::MakeAbsoluteFilePath(path);
    if (canonical.empty() || !base::DirectoryExists(canonical))
        return;
    for (const base::FilePath& root : m_application_resource_roots) {
        if (root == canonical || root.IsParent(canonical))
            return;
    }
    m_application_resource_roots.push_back(std::move(canonical));
}

bool WebContents::isRendererFrameCurrent(uint64_t frame_id) const
{
    for (size_t depth = 0; depth <= m_renderer_frames.size(); ++depth) {
        auto found = m_renderer_frames.find(frame_id);
        if (found == m_renderer_frames.end())
            return false;
        const RendererFrameState& frame = found->second;
        if (!frame.parent_id)
            return frame.main_frame && frame_id == m_main_frame_id;
        auto parent = m_renderer_frames.find(frame.parent_id);
        if (parent == m_renderer_frames.end()
            || parent->second.generation != frame.parent_generation)
            return false;
        frame_id = frame.parent_id;
    }
    return false;
}

bool WebContents::registerRendererFrame(
    uint64_t frame_id, uint64_t parent_id, bool main_frame)
{
    if (!frame_id || frame_id == parent_id)
        return false;
    auto existing = m_renderer_frames.find(frame_id);
    if (existing != m_renderer_frames.end()) {
        return existing->second.parent_id == parent_id
            && existing->second.main_frame == main_frame
            && isRendererFrameCurrent(frame_id);
    }
    if (main_frame) {
        if (parent_id || (m_main_frame_id && m_main_frame_id != frame_id))
            return false;
    } else if (!parent_id || !isRendererFrameCurrent(parent_id)) {
        return false;
    }
    // The same 1,000-frame bound is enforced by Blink's Page.
    if (m_renderer_frames.size() >= 1000)
        return false;
    RendererFrameState state;
    state.parent_id = parent_id;
    state.main_frame = main_frame;
    state.generation = ++m_frame_generation;
    if (parent_id)
        state.parent_generation = m_renderer_frames.at(parent_id).generation;
    if (main_frame)
        m_main_frame_id = frame_id;
    m_renderer_frames.emplace(frame_id, std::move(state));
    return true;
}

std::string WebContents::approveFrameNavigation(uint64_t frame_id,
    uint64_t parent_id, bool main_frame, const std::string& url,
    const std::string& method)
{
    GURL target(url);
    if (!target.is_valid() || method.empty()
        || !registerRendererFrame(frame_id, parent_id, main_frame))
        return {};
    RendererFrameState& frame = m_renderer_frames.at(frame_id);
    if (main_frame && !m_pending_main_navigation.token.empty()
        && SameDocumentUrl(url, m_pending_main_navigation.url)
        && method == m_pending_main_navigation.method) {
        frame.navigation = std::move(m_pending_main_navigation);
        m_pending_main_navigation = {};
    } else {
        RendererNavigation navigation;
        navigation.token = NavigationToken();
        navigation.url = target.spec();
        navigation.method = method;
        navigation.initiator_origin = frame.committed_origin;
        if (navigation.initiator_origin.empty() && parent_id)
            navigation.initiator_origin =
                m_renderer_frames.at(parent_id).committed_origin;
        frame.navigation = std::move(navigation);
    }
    frame.response_url.clear();
    return frame.navigation.token;
}

void WebContents::retireRendererFrameSockets(uint64_t frame_id)
{
    ApiSession* session = SessionMgr::get()->findOrCreateSession(
        isolate(), m_partition, false);
    if (!session)
        return;
    // A parent commit invalidates descendants before their detach notifications
    // arrive. Retire those opening generations while their ancestry still exists.
    for (const auto& entry : m_renderer_frames) {
        uint64_t ancestor = entry.first;
        for (size_t depth = 0; depth < m_renderer_frames.size(); ++depth) {
            if (ancestor == frame_id) {
                session->onRendererFrameRetired(
                    m_id, entry.first, entry.second.generation);
                break;
            }
            auto found = m_renderer_frames.find(ancestor);
            if (found == m_renderer_frames.end() || !found->second.parent_id)
                break;
            ancestor = found->second.parent_id;
        }
    }
}

bool WebContents::commitRendererFrame(
    uint64_t frame_id, const std::string& url)
{
    if (!isRendererFrameCurrent(frame_id))
        return false;
    RendererFrameState& frame = m_renderer_frames.at(frame_id);
    GURL target(url);
    if (!target.is_valid())
        return false;
    if (frame.main_frame && IsUnbrokeredDocument(target)
        && !m_pending_main_navigation.token.empty()
        && SameDocumentUrl(url, m_pending_main_navigation.url)) {
        frame.navigation = std::move(m_pending_main_navigation);
        m_pending_main_navigation = {};
    }
    bool observed_document = !frame.response_url.empty()
        && SameDocumentUrl(url, frame.response_url);
    bool approved_intrinsic = IsUnbrokeredDocument(target)
        && ((!frame.navigation.token.empty()
                && SameDocumentUrl(url, frame.navigation.url))
            || (frame.committed_url.empty()
                && (target.IsAboutBlank() || target.IsAboutSrcdoc())));
    if (!observed_document && !approved_intrinsic)
        return false;
    url::Origin origin = url::Origin::Create(target);
    std::string inherited_origin = frame.parent_id
        ? m_renderer_frames.at(frame.parent_id).committed_origin
        : frame.committed_origin;
    if (target.SchemeIs("blob") && !frame.navigation.browser_initiated
        && (origin.opaque() || origin.Serialize() != inherited_origin))
        return false;
    frame.committed_origin =
        (target.IsAboutBlank() || target.IsAboutSrcdoc())
        ? std::move(inherited_origin)
        : (origin.opaque() ? std::string() : origin.Serialize());
    frame.committed_url = target.spec();
    retireRendererFrameSockets(frame_id);
    if (frame.main_frame)
        cancelPreparedFrames("Document replaced before native presentation");
    frame.generation = ++m_frame_generation;
    frame.response_url.clear();
    frame.navigation = {};
    return true;
}

void WebContents::fromIdApi(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    int id = 0;
    if (info.Length() != 1 || !gin_helper::ConvertFromV8(info.GetIsolate(), info[0], &id))
        return;
    WebContents* contents = fromId(id);
    info.GetReturnValue().Set(contents
            ? contents->getWrapper().As<v8::Value>()
            : v8::Null(info.GetIsolate()).As<v8::Value>());
}

void WebContents::getAllWebContentsApi(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    v8::Local<v8::Array> result = v8::Array::New(info.GetIsolate());
    uint32_t index = 0;
    for (const auto& item : AllContents()) {
        if (!item.second->isDestroyedApi())
            result->Set(info.GetIsolate()->GetCurrentContext(), index++, item.second->getWrapper()).Check();
    }
    info.GetReturnValue().Set(result);
}

void WebContents::getFocusedWebContentsApi(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    for (const auto& item : AllContents()) {
        if (item.second->m_focused && !item.second->isDestroyedApi()) {
            info.GetReturnValue().Set(item.second->getWrapper());
            return;
        }
    }
    info.GetReturnValue().Set(v8::Null(info.GetIsolate()));
}

void WebContents::createGuestApi(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    if (info.Length() < 2 || !info[0]->IsObject())
        return;
    int host_id = 0;
    if (!gin_helper::ConvertFromV8(info.GetIsolate(), info[1], &host_id))
        return;
    WebContents* host = fromId(host_id);
    if (!host)
        return;

    gin_helper::Dictionary options(info.GetIsolate(), info[0].As<v8::Object>());
    options.Set("type", "webview");
    WebContents* guest = create(info.GetIsolate(), options, nullptr);
    if (!guest || guest->isDestroyedApi()) {
        if (guest)
            guest->destroyApi();
        info.GetReturnValue().Set(v8::Null(info.GetIsolate()));
        return;
    }
    guest->m_host = host;
    host->m_guest_ids.insert(guest->m_id);
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
    guest->setVSyncDisplayId(host->m_vsync_display_id);
    guest->setVisible(false);
#endif
    info.GetReturnValue().Set(guest->getWrapper());
}

void WebContents::getSessionApi(const v8::FunctionCallbackInfo<v8::Value>& info) const
{
    ApiSession* session = SessionMgr::get()->findOrCreateSession(info.GetIsolate(), m_partition, true);
    info.GetReturnValue().Set(session
            ? session->getWrapper().As<v8::Value>()
            : v8::Null(info.GetIsolate()).As<v8::Value>());
}

void WebContents::authorizeFileSelectionApi(
    const v8::FunctionCallbackInfo<v8::Value>& info)
{
    std::vector<std::string> paths;
    uint64_t request_id = 0;
    if (info.Length() != 2
        || !gin_helper::ConvertFromV8(info.GetIsolate(), info[0], &paths)
        || !gin_helper::ConvertFromV8(info.GetIsolate(), info[1], &request_id)) {
        info.GetIsolate()->ThrowException(v8::Exception::TypeError(
            gin_helper::StringToV8(info.GetIsolate(),
                "File selection requires an array of selected paths")));
        return;
    }
    ApiSession* session = SessionMgr::get()->findOrCreateSession(
        info.GetIsolate(), m_partition, false);
    auto chooser = m_file_chooser_frames.find(request_id);
    if (!isAlive() || !session || chooser == m_file_chooser_frames.end()
        || !isRendererFrameCurrent(chooser->second.frame_id)
        || m_renderer_frames.at(chooser->second.frame_id).generation
            != chooser->second.generation) {
        info.GetIsolate()->ThrowException(v8::Exception::Error(
            gin_helper::StringToV8(info.GetIsolate(),
                "Owning WebContents is unavailable for file selection")));
        return;
    }
    RendererFrameEpoch authorized_frame = chooser->second;
    m_file_chooser_frames.erase(chooser);
    std::string error;
    base::Value::List files = session->authorizeSelectedFiles(m_id,
        authorized_frame.frame_id, authorized_frame.generation, paths, &error);
    if (!error.empty()) {
        info.GetIsolate()->ThrowException(v8::Exception::Error(
            gin_helper::StringToV8(info.GetIsolate(), error)));
        return;
    }
    info.GetReturnValue().Set(
        gin_helper::ConvertToV8(info.GetIsolate(), files));
}

void WebContents::getFrameApi(const v8::FunctionCallbackInfo<v8::Value>& info) const
{
    uint64_t frame_id = m_main_frame_id;
    if (info.Length() > 0 && !info[0]->IsUndefined()) {
        std::string identity;
        if (!gin_helper::ConvertFromV8(info.GetIsolate(), info[0], &identity)
            || !base::StringToUint64(identity, &frame_id)
            || base::NumberToString(frame_id) != identity)
            frame_id = 0;
    }
    if (m_destroyed || !isRendererFrameCurrent(frame_id)) {
        info.GetReturnValue().Set(v8::Null(info.GetIsolate()));
        return;
    }
    const RendererFrameState& state = m_renderer_frames.at(frame_id);
    base::Value::Dict frame;
    frame.Set("frameId", base::NumberToString(frame_id));
    frame.Set("generation", base::NumberToString(state.generation));
    frame.Set("processId", getProcessIdApi());
    frame.Set("routingId", static_cast<double>(state.generation));
    frame.Set("url", state.committed_url);
    info.GetReturnValue().Set(gin_helper::ConvertToV8(info.GetIsolate(), frame));
}

void WebContents::loadURLApi(const std::string& url)
{
    if (!isAlive() || url.empty())
        return;
    base::Value::Dict params;
    params.Set("url", url);
    GURL target(url);
    if (!target.is_valid())
        return;
    cancelPreparedFrames("Navigation superseded native presentation");
    if (target.SchemeIsFile()) {
        base::FilePath file;
        if (net::FileURLToFilePath(target, &file))
            addApplicationResourceRoot(file.DirName());
    }
    RendererNavigation navigation;
    navigation.token = NavigationToken();
    navigation.url = target.spec();
    navigation.method = "GET";
    navigation.browser_initiated = true;
    auto current = m_renderer_frames.find(m_main_frame_id);
    if (current != m_renderer_frames.end())
        navigation.initiator_origin = current->second.committed_origin;
    if (!m_ready_to_show_emitted && m_type == "window") {
        // The renderer eagerly publishes about:blank. Keep window readiness
        // disarmed until the exact first requested document commits so that
        // nested single-process view initialization cannot consume the event.
        m_ready_to_show_armed = false;
        m_ready_to_show_navigation_token = navigation.token;
        // A hidden renderer may have released its blank startup lease already.
        // Reacquire it through the requested document's post-commit paint.
        m_initial_paint_pending = true;
        updateVisibility();
    }
    params.Set("navigationToken", navigation.token);
    if (current != m_renderer_frames.end()) {
        current->second.navigation = navigation;
        current->second.response_url.clear();
    }
    m_pending_main_navigation = std::move(navigation);
    m_renderer->Send("navigate", std::move(params));
}

void WebContents::downloadURLApi(const std::string& url)
{
    if (!isAlive())
        return;
    ApiSession* session = SessionMgr::get()->findOrCreateSession(
        isolate(), m_partition, false);
    if (session)
        session->downloadURL(this, url);
}

bool WebContents::requestDownloadSource(std::string method,
    base::Value::Dict params,
    std::function<void(base::Value::Dict, std::string)> reply)
{
    return isAlive() && m_renderer->Request(std::move(method), std::move(params), std::move(reply));
}

int WebContents::getProcessIdApi() const
{
    return m_renderer ? m_renderer->GetProcessId() : 0;
}

void WebContents::stopApi()
{
    if (isAlive())
        m_renderer->Send("stop", {});
}

void WebContents::goBackApi()
{
    if (isAlive())
        m_renderer->Send("goBack", {});
}

void WebContents::goForwardApi()
{
    if (isAlive())
        m_renderer->Send("goForward", {});
}

void WebContents::goToOffsetApi(int offset)
{
    base::Value::Dict params;
    params.Set("offset", offset);
    if (isAlive())
        m_renderer->Send("goToOffset", std::move(params));
}

void WebContents::goToIndexApi(int index)
{
    base::Value::Dict params;
    params.Set("index", index);
    if (isAlive())
        m_renderer->Send("goToIndex", std::move(params));
}

void WebContents::reloadApi()
{
    base::Value::Dict params;
    params.Set("ignoreCache", false);
    if (isAlive())
        m_renderer->Send("reload", std::move(params));
}

void WebContents::reloadIgnoringCacheApi()
{
    base::Value::Dict params;
    params.Set("ignoreCache", true);
    if (isAlive())
        m_renderer->Send("reload", std::move(params));
}

void WebContents::setUserAgentApi(const std::string& user_agent)
{
    m_user_agent = user_agent;
    base::Value::Dict params;
    params.Set("userAgent", user_agent);
    if (isAlive())
        m_renderer->Send("setUserAgent", std::move(params));
}

void WebContents::setZoomLevelApi(double level)
{
    m_zoom_level = level;
    base::Value::Dict params;
    params.Set("level", level);
    if (isAlive())
        m_renderer->Send("setZoomLevel", std::move(params));
}

void WebContents::setZoomFactorApi(double factor)
{
    setZoomLevelApi(std::log(factor) / std::log(1.2));
}

double WebContents::getZoomFactorApi() const
{
    return std::pow(1.2, m_zoom_level);
}

void WebContents::setIgnoreMenuShortcutsApi(bool ignore)
{
    m_ignore_menu_shortcuts = ignore;
    base::Value::Dict params;
    params.Set("ignore", ignore);
    if (isAlive())
        m_renderer->Send("setIgnoreMenuShortcuts", std::move(params));
}

void WebContents::resize(int width, int height, double scale_factor)
{
    if (!isAlive() || width <= 0 || height <= 0)
        return;
    if (width == m_viewport_width && height == m_viewport_height
        && scale_factor == m_viewport_scale)
        return;
    base::Value::Dict params;
    params.Set("width", width);
    params.Set("height", height);
    params.Set("scaleFactor", scale_factor);
    if (m_renderer->Send("setViewport", std::move(params))) {
        m_viewport_width = width;
        m_viewport_height = height;
        m_viewport_scale = scale_factor;
    }
}
#if BUILDFLAG(IS_WIN)
void WebContents::setScreenRects(const gfx::Rect& widget_screen_rect,
    const gfx::Rect& window_screen_rect)
{
    if (!isAlive() || widget_screen_rect.IsEmpty() || window_screen_rect.IsEmpty())
        return;
    if (!m_screen_rects_valid || widget_screen_rect != m_widget_screen_rect
        || window_screen_rect != m_window_screen_rect) {
        base::Value::Dict params;
        params.Set("widgetX", widget_screen_rect.x());
        params.Set("widgetY", widget_screen_rect.y());
        params.Set("widgetWidth", widget_screen_rect.width());
        params.Set("widgetHeight", widget_screen_rect.height());
        params.Set("windowX", window_screen_rect.x());
        params.Set("windowY", window_screen_rect.y());
        params.Set("windowWidth", window_screen_rect.width());
        params.Set("windowHeight", window_screen_rect.height());
        if (!m_renderer->Send("setScreenRects", std::move(params)))
            return;
        m_widget_screen_rect = widget_screen_rect;
        m_window_screen_rect = window_screen_rect;
        m_screen_rects_valid = true;
    }

    for (int guest_id : m_guest_ids) {
        WebContents* guest = fromId(guest_id);
        const auto bounds = m_guest_bounds.find(guest_id);
        if (!guest || guest->m_host != this || bounds == m_guest_bounds.end()
            || bounds->second.IsEmpty()) {
            continue;
        }
        const gfx::Rect& logical = bounds->second;
        guest->setScreenRects(
            gfx::Rect(widget_screen_rect.x() + logical.x(),
                widget_screen_rect.y() + logical.y(),
                logical.width(), logical.height()),
            window_screen_rect);
    }
}
#endif


#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
void WebContents::setVSyncDisplayId(int64_t display_id)
{
    if (!isAlive())
        return;
#if BUILDFLAG(IS_MAC)
    if (display_id == m_vsync_display_id)
        return;
#endif
    base::Value::Dict params;
#if BUILDFLAG(IS_WIN)
    params.Set("displayId", base::NumberToString(display_id));
#else
    params.Set("displayId", static_cast<double>(display_id));
#endif
    if (!m_renderer->Send("setVSyncDisplayId", std::move(params)))
        return;
    m_vsync_display_id = display_id;

#if BUILDFLAG(IS_WIN)
    const std::set<int> guest_ids = m_guest_ids;
    for (int guest_id : guest_ids) {
        WebContents* guest = fromId(guest_id);
        if (guest && guest->m_host == this)
            guest->setVSyncDisplayId(display_id);
    }
#endif
}
#endif

void WebContents::setVisible(bool visible)
{
    m_visible = visible;
    updateVisibility();
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
    for (int id : m_guest_ids) {
        WebContents* guest = fromId(id);
        const auto bounds = m_guest_bounds.find(id);
        if (guest && guest->m_host == this)
            guest->setVisible(visible && bounds != m_guest_bounds.end()
                && !bounds->second.IsEmpty());
    }
#endif
}

void WebContents::updateVisibility()
{
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
    // A hidden renderer gets one compositor publication for startup or for its
    // first requested document. Readiness and renderer scheduling are separate:
    // consuming either state must not keep the other alive indefinitely.
    const bool visible = m_visible || !m_background_throttling
        || m_initial_paint_pending || m_visibility_preparations;
    if (!isAlive() || m_renderer_visible == visible)
        return;
    base::Value::Dict params;
    params.Set("visible", visible);
    if (m_renderer->Send("setVisibility", std::move(params)))
        m_renderer_visible = visible;
#endif
}

void WebContents::setFocus(bool focused)
{
    m_focused = focused;
    if (!focused) {
        m_observers.Notify(&WebContentsObserver::onWebContentsTextInputState, this, base::Value::Dict());
    }
    base::Value::Dict params;
    params.Set("focused", focused);
    if (isAlive())
        m_renderer->Send("setFocus", std::move(params));
}

bool WebContents::sendInput(base::Value::Dict event)
{
    if (!isAlive())
        return false;
    base::Value::Dict params;
    params.Set("event", std::move(event));
    return m_renderer->Send("input", std::move(params));
}

bool WebContents::sendNativeFileDrag(base::Value::Dict event,
    const std::vector<std::string>& paths)
{
    const std::string* type = event.FindString("type");
    if (!isAlive() || !type)
        return false;
    if (*type == "dragEnter") {
        ApiSession* session = SessionMgr::get()->findOrCreateSession(
            isolate(), m_partition, false);
        auto target_frame = m_renderer_frames.find(m_main_frame_id);
        if (!session || target_frame == m_renderer_frames.end()
            || !isRendererFrameCurrent(m_main_frame_id))
            return false;
        if (m_native_drag_active) {
            base::Value::Dict leave;
            leave.Set("type", "dragLeave");
            leave.Set("generation", base::NumberToString(
                m_native_drag_generation));
            leave.Set("x", event.FindInt("x").value_or(0));
            leave.Set("y", event.FindInt("y").value_or(0));
            leave.Set("screenX", event.FindInt("screenX").value_or(0));
            leave.Set("screenY", event.FindInt("screenY").value_or(0));
            leave.Set("modifiers", event.FindInt("modifiers").value_or(0));
            sendInput(std::move(leave));
        }
        for (const std::string& token : m_native_drag_tokens)
            session->revokeFile(token);
        m_native_drag_tokens.clear();
        if (++m_native_drag_generation == 0)
            ++m_native_drag_generation;
        m_native_drag_operation_known = false;
        m_native_drag_operation_accepted = false;
        std::string error;
        base::Value::List files = session->authorizeDroppedFiles(
            m_id, m_main_frame_id, target_frame->second.generation,
            paths, &error);
        if (!error.empty() || files.empty()) {
            m_native_drag_active = false;
            return false;
        }
        for (const base::Value& file : files) {
            if (const std::string* token = file.GetDict().FindString("token"))
                m_native_drag_tokens.push_back(*token);
        }
        event.Set("generation", base::NumberToString(
            m_native_drag_generation));
        event.Set("files", std::move(files));
        m_native_drag_active = sendInput(std::move(event));
        if (!m_native_drag_active) {
            for (const std::string& token : m_native_drag_tokens)
                session->revokeFile(token);
            m_native_drag_tokens.clear();
        }
        return m_native_drag_active;
    }
    if ((*type != "dragOver" && *type != "dragLeave" && *type != "drop")
        || !m_native_drag_active || !paths.empty()) {
        return false;
    }
    const bool over = *type == "dragOver";
    const bool explicit_leave = *type == "dragLeave";
    const bool dropping = *type == "drop";
    const bool rejected_drop = dropping
        && (!m_native_drag_operation_known
            || !m_native_drag_operation_accepted);
    if (rejected_drop)
        event.Set("type", "dragLeave");
    event.Set("generation", base::NumberToString(m_native_drag_generation));
    const bool leaving = explicit_leave || rejected_drop;
    const bool terminal = leaving || dropping;
    bool sent = sendInput(std::move(event));
    if (!sent && terminal) {
        if (ApiSession* session = SessionMgr::get()->findOrCreateSession(
                isolate(), m_partition, false)) {
            for (const std::string& token : m_native_drag_tokens)
                session->revokeFile(token);
        }
        m_native_drag_tokens.clear();
    }
    if (terminal)
        m_native_drag_active = false;
    if (rejected_drop)
        return false;
    return sent && (!over || !m_native_drag_operation_known
        || m_native_drag_operation_accepted);
}

#if defined(_WIN32)
void WebContents::sendWindowsMouseEvent(unsigned message, int x, int y,
    double screenX, double screenY, unsigned flags, int delta)
{
    if (!isAlive())
        return;
    const char* type = "mouseMove";
    switch (message) {
    case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN:
    case WM_LBUTTONDBLCLK: case WM_MBUTTONDBLCLK: case WM_RBUTTONDBLCLK:
        type = "mouseDown";
        break;
    case WM_LBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONUP: type = "mouseUp"; break;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: type = "mouseWheel"; break;
    case WM_CONTEXTMENU: type = "contextMenu"; break;
    default: break;
    }
    sendInput(InputFromWindows(type, message, x, y, screenX, screenY, flags, delta));
}
#endif

#if BUILDFLAG(IS_WIN)
void WebContents::sendWindowsKeyEvent(const char* type, unsigned key_code,
    unsigned flags, unsigned message, LPARAM l_param)
{
    if (::GetKeyState(VK_CONTROL) & 0x8000)
        flags |= MINI_ELECTRON_CONTROL;
    if (::GetKeyState(VK_SHIFT) & 0x8000)
        flags |= MINI_ELECTRON_SHIFT;
    if (::GetKeyState(VK_MENU) & 0x8000)
        flags |= MINI_ELECTRON_ALT;
    if ((::GetKeyState(VK_LWIN) | ::GetKeyState(VK_RWIN)) & 0x8000)
        flags |= MINI_ELECTRON_META;
    const bool rightAlt = (::GetKeyState(VK_RMENU) & 0x8000)
        || (key_code == VK_MENU
            && (static_cast<uintptr_t>(l_param) & (uintptr_t { 1 } << 24)));
    if (rightAlt && (flags & MINI_ELECTRON_CONTROL))
        flags |= MINI_ELECTRON_ALT_GRAPH;

    if (::GetKeyState(VK_CAPITAL) & 0x0001)
        flags |= MINI_ELECTRON_CAPS_LOCK;
    if (::GetKeyState(VK_NUMLOCK) & 0x0001)
        flags |= MINI_ELECTRON_NUM_LOCK;

    base::Value::Dict input;
    input.Set("type", type);
    input.Set("keyCode", static_cast<int>(key_code));
    if (message == WM_CHAR || message == WM_SYSCHAR)
        input.Set("charCode", static_cast<int>(key_code));
    input.Set("modifiers", static_cast<int>(flags));
    input.Set("nativeMessage", static_cast<int>(message));
    input.Set("nativeLParam",
        static_cast<int>(static_cast<uint32_t>(l_param)));
    input.Set("keyboardLayout", base::NumberToString(
        reinterpret_cast<uintptr_t>(::GetKeyboardLayout(0))));
    input.Set("systemKey", !(flags & MINI_ELECTRON_ALT_GRAPH)
        && (message == WM_SYSKEYDOWN || message == WM_SYSKEYUP
            || message == WM_SYSCHAR));
    sendInput(std::move(input));
}
#endif

void WebContents::sendInputEventApi(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    base::Value::Dict input;
    if (info.Length() != 1 || !gin_helper::ConvertFromV8(info.GetIsolate(), info[0], &input))
        return;
    sendInput(std::move(input));
}

bool WebContents::sendApi(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    gin_helper::Arguments args(info);
    std::string frame_identity;
    bool all_frames = false;
    std::string channel;
    base::Value::List values;
    if (!args.GetNext(&frame_identity) || !args.GetNext(&all_frames)
        || !args.GetNext(&channel) || !args.GetRemaining(&values)) {
        args.ThrowError();
        return false;
    }
    if (!isAlive())
        return false;

    uint64_t frame_id = 0;
    if (!all_frames) {
        // Electron exposes routingId as a Number. Resolve the browser-assigned
        // live frame generation, while internal sends stay lossless.
        if (frame_identity.empty()) {
            frame_id = m_main_frame_id;
        } else if (frame_identity.rfind("routing:", 0) == 0) {
            double routing_id = 0;
            if (!base::StringToDouble(frame_identity.substr(8), &routing_id)
                || !std::isfinite(routing_id) || routing_id <= 0
                || std::floor(routing_id) != routing_id) {
                return false;
            }
            for (const auto& entry : m_renderer_frames) {
                if (static_cast<double>(entry.second.generation) != routing_id
                    || !isRendererFrameCurrent(entry.first)) {
                    continue;
                }
                if (frame_id)
                    return false;
                frame_id = entry.first;
            }
            if (!frame_id)
                return false;
        } else if (!base::StringToUint64(frame_identity, &frame_id)
            || base::NumberToString(frame_id) != frame_identity || !frame_id) {
            return false;
        }
        if (!isRendererFrameCurrent(frame_id))
            return false;
    }

    base::Value::Dict params;
    params.Set("frameId", base::NumberToString(frame_id));
    params.Set("allFrames", all_frames);
    params.Set("channel", channel);
    params.Set("args", std::move(values));
    return m_renderer->Send("ipc-message", std::move(params));
}

bool WebContents::sendCommandApi(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    std::string method;
    base::Value::Dict params;
    if (info.Length() < 1 || !gin_helper::ConvertFromV8(info.GetIsolate(), info[0], &method))
        return false;
    if (info.Length() > 1 && !gin_helper::ConvertFromV8(info.GetIsolate(), info[1], &params))
        return false;
    #if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
    if (method == "setBackgroundThrottling") {
        const std::optional<bool> allowed = params.FindBool("allowed");
        if (!allowed || !isAlive())
            return false;
        m_background_throttling = *allowed;
        m_web_preferences.Set("backgroundThrottling", *allowed);
        updateVisibility();
        return true;
    }
    if (method == "guest.attach" || method == "guest.bounds" || method == "guest.detach") {
        const int id = params.FindInt("guestContentsId").value_or(0);
        if (method == "guest.detach" || m_guest_ids.contains(id)) {
            if (method == "guest.detach")
                m_guest_bounds.erase(id);
            else {
                m_guest_bounds[id] = gfx::Rect(
                    std::clamp(params.FindInt("x").value_or(0), -(1 << 20), 1 << 20),
                    std::clamp(params.FindInt("y").value_or(0), -(1 << 20), 1 << 20),
                    std::clamp(params.FindInt("width").value_or(0), 0, 1 << 20),
                    std::clamp(params.FindInt("height").value_or(0), 0, 1 << 20));
            }
            WebContents* guest = fromId(id);
            const auto bounds = m_guest_bounds.find(id);
            if (guest && guest->m_host == this)
                guest->setVisible(m_visible && bounds != m_guest_bounds.end()
                    && !bounds->second.IsEmpty());
#if BUILDFLAG(IS_WIN)
            if (m_screen_rects_valid)
                setScreenRects(m_widget_screen_rect, m_window_screen_rect);
#endif
            m_observers.Notify(&WebContentsObserver::onWebContentsPaint, this, gfx::Rect());
        }
    }
    #endif
    return isAlive() && m_renderer->Send(std::move(method), std::move(params));
}

bool WebContents::respondApi(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    uint64_t request_id = 0;
    base::Value::Dict result;
    std::string error;
    if (info.Length() < 1
        || !gin_helper::ConvertFromV8(info.GetIsolate(), info[0], &request_id))
        return false;
    if (info.Length() > 1 && !info[1]->IsNullOrUndefined()
        && !gin_helper::ConvertFromV8(info.GetIsolate(), info[1], &result))
        return false;
    if (info.Length() > 2 && !info[2]->IsNullOrUndefined()
        && !gin_helper::ConvertFromV8(info.GetIsolate(), info[2], &error))
        return false;
    m_file_chooser_frames.erase(request_id);
    return isAlive() && m_renderer->Respond(request_id, std::move(result), std::move(error));
}

bool WebContents::attachGuestApi(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    int guest_id = 0;
    base::Value::Dict params;
    if (info.Length() != 2
        || !gin_helper::ConvertFromV8(info.GetIsolate(), info[0], &guest_id)
        || !gin_helper::ConvertFromV8(info.GetIsolate(), info[1], &params))
        return false;
    WebContents* guest = fromId(guest_id);
    if (!guest || guest->m_host != this || !isAlive() || !guest->isAlive())
        return false;
    if (!guest->m_renderer->ShareFrameBufferWith(*m_renderer, guest_id))
        return false;
    params.Set("guestContentsId", guest_id);
    #if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
    m_guest_bounds[guest_id] = gfx::Rect(
        params.FindInt("x").value_or(0), params.FindInt("y").value_or(0),
        params.FindInt("width").value_or(1), params.FindInt("height").value_or(1));
    guest->setVisible(m_visible && !m_guest_bounds[guest_id].IsEmpty());
#if BUILDFLAG(IS_WIN)
    if (m_screen_rects_valid)
        setScreenRects(m_widget_screen_rect, m_window_screen_rect);
#endif
    #endif
    return m_renderer->Send("guest.attach", std::move(params));
}

#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
void WebContents::debuggerAttachApi(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    std::string version = "1.3";
    if (info.Length() && !info[0]->IsUndefined()
        && !gin_helper::ConvertFromV8(info.GetIsolate(), info[0], &version))
        return;
    if (version != "" && version != "1.3") {
        info.GetIsolate()->ThrowException(v8::Exception::Error(
            gin_helper::StringToV8(info.GetIsolate(),
                "Unsupported debugger protocol version")));
        return;
    }
    if (m_debugger_attached) {
        info.GetIsolate()->ThrowException(v8::Exception::Error(
            gin_helper::StringToV8(info.GetIsolate(),
                "Debugger is already attached")));
        return;
    }
    base::Value::Dict params;
    params.Set("protocolVersion", version);
    if (!isAlive() || !m_renderer->Send("cdp.attach", std::move(params))) {
        info.GetIsolate()->ThrowException(v8::Exception::Error(
            gin_helper::StringToV8(info.GetIsolate(),
                "Failed to attach debugger")));
        return;
    }
    m_debugger_attached = true;
}

void WebContents::debuggerDetachApi()
{
    if (!m_debugger_attached)
        return;
    m_debugger_attached = false;
    if (isAlive())
        m_renderer->Send("cdp.detach", {});
}

bool WebContents::debuggerIsAttachedApi() const
{
    return m_debugger_attached && isAlive();
}

void WebContents::debuggerSendCommandApi(
    const v8::FunctionCallbackInfo<v8::Value>& info)
{
    v8::Isolate* isolate = info.GetIsolate();
    auto* promise = new gin_helper::Promise<base::Value::Dict>(isolate);
    info.GetReturnValue().Set(promise->GetHandle());
    std::string method;
    base::Value::Dict command_params;
    if (!m_debugger_attached || info.Length() < 1
        || !gin_helper::ConvertFromV8(isolate, info[0], &method)
        || (info.Length() > 1
            && !gin_helper::ConvertFromV8(
                isolate, info[1], &command_params))) {
        promise->RejectWithErrorMessage(
            m_debugger_attached ? "Invalid debugger command"
                                : "Debugger is not attached");
        delete promise;
        return;
    }
    if (method == "DOM.setFileInputFiles") {
        const base::Value::List* files = command_params.FindList("files");
        std::vector<std::string> paths;
        if (!files) {
            promise->RejectWithErrorMessage(
                "DOM.setFileInputFiles requires a files array");
            delete promise;
            return;
        }
        paths.reserve(files->size());
        for (const base::Value& file : *files) {
            if (!file.is_string()) {
                promise->RejectWithErrorMessage(
                    "DOM.setFileInputFiles files must be strings");
                delete promise;
                return;
            }
            paths.push_back(file.GetString());
        }
        ApiSession* session = SessionMgr::get()->findOrCreateSession(
            isolate, m_partition, false);
        std::string authorization_error;
        if (!session ||
            !session->authorizeUploadPaths(
                m_id, paths, &authorization_error)) {
            promise->RejectWithErrorMessage(
                authorization_error.empty()
                    ? "Owning Session could not authorize uploads"
                    : authorization_error);
            delete promise;
            return;
        }
    }
    base::Value::Dict params;
    params.Set("method", method);
    params.Set("params", std::move(command_params));
    if (!isAlive() || !m_renderer->Request(
            "cdp.dispatch", std::move(params),
            [promise](base::Value::Dict result, std::string error) {
                if (error.empty())
                    promise->Resolve(result);
                else
                    promise->RejectWithErrorMessage(error);
                delete promise;
            })) {
        promise->RejectWithErrorMessage(
            "Debugger command could not be queued");
        delete promise;
    }
}

std::string WebContents::openDevToolsApi(const base::Value::Dict&)
{
    if (!m_devtools_gateway)
        m_devtools_gateway = std::make_unique<DevToolsGateway>(this);
    std::string frontend_url;
    std::string error;
    if (!m_devtools_gateway->Start(&frontend_url, &error)) {
        m_devtools_gateway.reset();
        isolate()->ThrowException(v8::Exception::Error(
            gin_helper::StringToV8(isolate(), error)));
        return std::string();
    }
    return frontend_url;
}

void WebContents::closeDevToolsApi()
{
    m_devtools_gateway.reset();
}

bool WebContents::isDevToolsOpenedApi() const
{
    return m_devtools_gateway && m_devtools_gateway->IsRunning();
}

bool WebContents::inspectElementApi(int x, int y)
{
    if (!isDevToolsOpenedApi() || !m_renderer)
        return false;
    base::Value::Dict params;
    params.Set("x", x);
    params.Set("y", y);
    return m_renderer->Send("cdp.inspectElement", std::move(params));
}
#endif

void WebContents::requestApi(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    v8::Isolate* isolate = info.GetIsolate();
    auto* promise = new gin_helper::Promise<base::Value::Dict>(isolate);
    info.GetReturnValue().Set(promise->GetHandle());

    std::string method;
    base::Value::Dict params;
    if (info.Length() < 1 || !gin_helper::ConvertFromV8(isolate, info[0], &method)
        || (info.Length() > 1 && !gin_helper::ConvertFromV8(isolate, info[1], &params))) {
        promise->RejectWithErrorMessage("Invalid renderer request");
        delete promise;
        return;
    }
    if (!isAlive()) {
        promise->RejectWithErrorMessage("WebContents is destroyed");
        delete promise;
        return;
    }
    if (!m_renderer->Request(std::move(method), std::move(params),
            [promise](base::Value::Dict result, std::string error) {
                if (error.empty())
                    promise->Resolve(result);
                else
                    promise->RejectWithErrorMessage(error);
                delete promise;
            })) {
        promise->RejectWithErrorMessage("Renderer request could not be queued");
        delete promise;
    }
}

void WebContents::prepareToShowApi(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    struct Preparation {
        explicit Preparation(v8::Isolate* isolate) : promise(isolate) { }
        gin_helper::Promise<void> promise;
        bool completed = false;
    };
    auto preparation = std::make_shared<Preparation>(info.GetIsolate());
    info.GetReturnValue().Set(preparation->promise.GetHandle());
    if (!isAlive()) {
        preparation->promise.RejectWithErrorMessage("WebContents is destroyed");
        return;
    }
    const int contents_id = m_id;
    const uint64_t presentation_epoch = m_presentation_epoch;
    if (std::none_of(m_observers.begin(), m_observers.end(),
            [](const WebContentsObserver& observer) { return observer.supportsFramePreparation(); })) {
        preparation->promise.RejectWithErrorMessage("Native frame preparation is unavailable for this WebContents");
        return;
    }
    ++m_visibility_preparations;
    updateVisibility();
    auto complete = [contents_id, preparation](const std::string& error) {
        // A failed queue can also deliver a deferred transport-failure reply.
        if (preparation->completed)
            return;
        preparation->completed = true;
        if (WebContents* contents = WebContents::fromId(contents_id)) {
            --contents->m_visibility_preparations;
            contents->updateVisibility();
        }
        if (error.empty())
            preparation->promise.Resolve();
        else
            preparation->promise.RejectWithErrorMessage(error);
    };
    if (!m_renderer->Request("prepare-to-show", {},
        [contents_id, presentation_epoch, complete](base::Value::Dict result, std::string error) {
            WebContents* contents = WebContents::fromId(contents_id);
            uint64_t generation = 0;
            const std::string* token = result.FindString("generation");
            if (error.empty() && (!contents || !contents->isAlive()))
                error = "WebContents closed before presentation";
            if (error.empty() && contents->m_presentation_epoch != presentation_epoch)
                error = "Document changed before native presentation";
            if (error.empty() && (!token || !base::StringToUint64(*token, &generation) || !generation))
                error = "Renderer did not publish a prepared frame";
            if (!error.empty()) {
                complete(error);
                return;
            }
            contents->m_prepared_frames.push_back({generation, complete});
            contents->completePreparedFrames();
        })) {
        complete("Renderer preparation could not be queued");
    }
}

void WebContents::completePreparedFrames()
{
    if (m_prepared_frames.empty())
        return;
    // Detach before native drawing or resolving promises: either can reenter.
    auto pending = std::move(m_prepared_frames);
    m_prepared_frames.clear();
    for (auto& frame : pending) {
        uint64_t drawn = 0;
        for (WebContentsObserver& observer : m_observers)
            drawn = std::max(drawn, observer.prepareWebContentsFrame(this, frame.generation));
        if (drawn >= frame.generation)
            frame.complete({});
        else if (!isAlive())
            frame.complete("Renderer closed before native presentation");
        else if (std::none_of(m_observers.begin(), m_observers.end(),
                     [](const WebContentsObserver& observer) { return observer.supportsFramePreparation(); }))
            frame.complete("Native presentation host is unavailable");
        else
            m_prepared_frames.push_back(std::move(frame));
    }
}

void WebContents::cancelPreparedFrames(const std::string& error)
{
    ++m_presentation_epoch;
    auto pending = std::move(m_prepared_frames);
    m_prepared_frames.clear();
    for (auto& frame : pending)
        frame.complete(error);
}

bool WebContents::copyFrame(RendererFrameSnapshot* snapshot, bool include_native_pixels) const
{
    return snapshot && isAlive() && m_renderer->CopyFrame(snapshot, include_native_pixels);
}

#if defined(_WIN32)
void WebContents::getCompositorInfoApi(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    auto promise = std::make_shared<gin_helper::Promise<base::Value::Dict>>(info.GetIsolate());
    info.GetReturnValue().Set(promise->GetHandle());
    if (!isAlive()) {
        promise->RejectWithErrorMessage("WebContents is destroyed");
        return;
    }
    const int id = m_id;
    base::Value::Dict params;
    params.Set("displayId", base::NumberToString(m_vsync_display_id));
    if (!m_renderer->Request("getCompositorInfo", std::move(params),
            [id, promise](base::Value::Dict result, std::string error) mutable {
                WebContents* contents = fromId(id);
                if (!contents || !contents->isAlive())
                    error = "WebContents closed before compositor diagnostics";
                if (!error.empty()) {
                    promise->RejectWithErrorMessage(error);
                    return;
                }
                WindowInterface* owner = contents->m_owner;
                for (WebContents* host = contents->m_host; !owner && host; host = host->m_host)
                    owner = host->m_owner;
                const HWND window = owner ? owner->getHWND() : nullptr;
                const bool native = window && ::GetPropW(window, L"MiniElectronDirectComposition");
                result.Set("presentation", native ? "directcomposition" : "software");
                result.Set("importedFrames", contents->m_windowsPaintFrame.gpu_frame
                    ? static_cast<double>(contents->m_windowsPaintFrame.gpu_frame->imported_frames()) : 0.0);
                promise->Resolve(result);
            })) {
        promise->RejectWithErrorMessage("Compositor diagnostics could not be queued");
    }
}

void WebContents::appendDCompLayers(std::vector<mini_electron::win::DCompLayer>& layers,
    const RECT& bounds, const RECT& clip, unsigned depth) const
{
    if (depth >= renderer_protocol::kMaxValueDepth)
        return;
    copyFrame(&m_windowsPaintFrame);
    const auto& snapshot = m_windowsPaintFrame;
    if (!snapshot.generation || snapshot.width <= 0 || snapshot.height <= 0)
        return;
    mini_electron::win::DCompLayer layer;
    layer.id = m_id;
    layer.frame = snapshot.gpu_frame;
    layer.bounds = bounds;
    layer.clip = clip;
    layer.width = snapshot.width;
    layer.height = snapshot.height;
    layer.stride = snapshot.stride;
    layer.generation = snapshot.generation;
    layer.rgba = snapshot.rgba.empty() ? nullptr : snapshot.rgba.data();
    layers.push_back(std::move(layer));
    const float pixel_scale = snapshot.output_format ? snapshot.native_scale_factor
        : static_cast<float>(m_viewport_scale > 0 ? m_viewport_scale : 1.0);
    if (!std::isfinite(pixel_scale) || pixel_scale <= 0)
        return;
    const double scale_x = static_cast<double>(pixel_scale)
        * (static_cast<double>(bounds.right) - bounds.left) / snapshot.width;
    const double scale_y = static_cast<double>(pixel_scale)
        * (static_cast<double>(bounds.bottom) - bounds.top) / snapshot.height;
    // Metadata comes from a sandbox. Saturation also keeps RECT differences
    // representable when a forged scale places a guest far outside the client.
    const auto coordinate = [](double value, bool ceil) {
        constexpr double limit = std::numeric_limits<LONG>::max() / 2;
        return static_cast<LONG>(std::clamp(ceil ? std::ceil(value) : std::floor(value), -limit, limit));
    };
    for (int id : m_guest_ids) {
        WebContents* guest = fromId(id);
        auto found = m_guest_bounds.find(id);
        if (!guest || guest->m_host != this || found == m_guest_bounds.end())
            continue;
        const gfx::Rect& logical = found->second;
        RECT guest_bounds = {
            coordinate(bounds.left + logical.x() * scale_x, false),
            coordinate(bounds.top + logical.y() * scale_y, false),
            coordinate(bounds.left + logical.right() * scale_x, true),
            coordinate(bounds.top + logical.bottom() * scale_y, true)
        };
        RECT guest_clip;
        if (!::IntersectRect(&guest_clip, &guest_bounds, &clip))
            continue;
        guest->appendDCompLayers(layers, guest_bounds, guest_clip, depth + 1);
    }
}

bool WebContents::paintFrame(HDC target, int dest_x, int dest_y, int src_x, int src_y,
    int width, int height, const gfx::Size& target_size) const
{
    if (!target)
        return false;
    // A busy producer must not make native controls blend over their own last
    // presentation. Restore the webpage from the last validated source instead.
    if (!copyFrame(&m_windowsPaintFrame))
        m_windowsPaintNeedsFullRepaint = true;
    const RendererFrameSnapshot& frame = m_windowsPaintFrame;
    if (!frame.generation || frame.width <= 0 || frame.height <= 0
        || frame.stride < frame.width * 4)
        return false;
    src_x = std::max(0, src_x);
    src_y = std::max(0, src_y);
    width = std::min(width, target_size.width() - src_x);
    height = std::min(height, target_size.height() - src_y);
    if (width <= 0 || height <= 0)
        return false;

    const bool stretch = frame.width != target_size.width()
        || frame.height != target_size.height();
    const int copy_width = stretch ? frame.width : width;
    const int copy_height = stretch ? frame.height : height;
    // Resize retention needs the complete previous frame. Reuse that BGRA
    // conversion across native paints until a new publication arrives.
    if (!stretch || m_windowsPaintPixelsGeneration != frame.generation) {
        m_windowsPaintPixels.resize(static_cast<size_t>(copy_width) * copy_height * 4u);
        uint8_t* output = m_windowsPaintPixels.data();
        for (int row = 0; row < copy_height; ++row) {
            const uint8_t* pixel = frame.rgba.data()
                + static_cast<size_t>((stretch ? 0 : src_y) + row) * frame.stride
                + static_cast<size_t>(stretch ? 0 : src_x) * 4;
            for (int column = 0; column < copy_width; ++column, pixel += 4, output += 4) {
                output[0] = pixel[2];
                output[1] = pixel[1];
                output[2] = pixel[0];
                output[3] = pixel[3];
            }
        }
        m_windowsPaintPixelsGeneration = stretch ? frame.generation : 0;
    }

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = copy_width;
    info.bmiHeader.biHeight = -copy_height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    if (stretch) {
        const int saved = ::SaveDC(target);
        if (!saved)
            return false;
        ::IntersectClipRect(target, dest_x, dest_y, dest_x + width, dest_y + height);
        ::SetStretchBltMode(target, COLORONCOLOR);
        const int result = ::StretchDIBits(target, dest_x - src_x, dest_y - src_y,
            target_size.width(), target_size.height(), 0, 0, copy_width, copy_height,
            m_windowsPaintPixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
        ::RestoreDC(target, saved);
        return result != GDI_ERROR;
    }
    return ::StretchDIBits(target, dest_x, dest_y, width, height,
               0, 0, width, height, m_windowsPaintPixels.data(), &info,
               DIB_RGB_COLORS, SRCCOPY)
        != GDI_ERROR;
}
#endif

#if BUILDFLAG(IS_MAC)
void WebContents::captureForSoftwareHost()
{
    m_host_capture_dirty = true;
    if (m_host_capture_pending || !m_host || !m_host->isAlive()
        || m_host->m_native_context_id || !isAlive())
        return;
    m_host_capture_dirty = false;
    m_host_capture_pending = true;
    const int contents_id = m_id;
    if (!m_renderer->Request("capturePage", {},
            [contents_id](base::Value::Dict, std::string error) {
                WebContents* contents = fromId(contents_id);
                if (!contents)
                    return;
                contents->m_host_capture_pending = false;
                if (!error.empty()) {
                    contents->m_host_capture_dirty = false;
                    return;
                }
                if (contents->m_host && contents->m_host->isAlive()
                    && !contents->m_host->m_native_context_id)
                    contents->m_host->m_renderer->Send("invalidate", {});
                // Only a real intervening paint requests another capture.
                if (contents->m_host_capture_dirty)
                    contents->captureForSoftwareHost();
            })) {
        m_host_capture_pending = false;
        m_host_capture_dirty = false;
    }
}
#endif

#if BUILDFLAG(IS_WIN)
namespace {

bool CompositeCapturedGuest(RendererFrameSnapshot* root,
    const RendererFrameSnapshot& guest, const gfx::Rect& logical_bounds)
{
    if (!root || root->width <= 0 || root->height <= 0
        || root->stride < root->width * 4
        || root->rgba.size()
            < static_cast<size_t>(root->stride) * root->height
        || guest.width <= 0 || guest.height <= 0
        || guest.stride < guest.width * 4
        || guest.rgba.size()
            < static_cast<size_t>(guest.stride) * guest.height
        || !std::isfinite(root->native_scale_factor)
        || root->native_scale_factor <= 0)
        return false;
    const double scale = root->native_scale_factor;
    const auto coordinate = [](double value, bool ceil) {
        constexpr double limit =
            std::numeric_limits<int32_t>::max() / 2.0;
        return static_cast<int64_t>(std::clamp(
            ceil ? std::ceil(value) : std::floor(value), -limit, limit));
    };
    const int64_t left_bound =
        coordinate(logical_bounds.x() * scale, false);
    const int64_t top_bound =
        coordinate(logical_bounds.y() * scale, false);
    const int64_t right_bound =
        coordinate(logical_bounds.right() * scale, true);
    const int64_t bottom_bound =
        coordinate(logical_bounds.bottom() * scale, true);
    const int64_t bound_width = right_bound - left_bound;
    const int64_t bound_height = bottom_bound - top_bound;
    if (bound_width <= 0 || bound_height <= 0)
        return true;
    const int left = static_cast<int>(std::max<int64_t>(0, left_bound));
    const int top = static_cast<int>(std::max<int64_t>(0, top_bound));
    const int right = static_cast<int>(
        std::min<int64_t>(root->width, right_bound));
    const int bottom = static_cast<int>(
        std::min<int64_t>(root->height, bottom_bound));
    for (int y = top; y < bottom; ++y) {
        const int source_y = static_cast<int>(
            (static_cast<int64_t>(y) - top_bound) * guest.height
            / bound_height);
        for (int x = left; x < right; ++x) {
            const int source_x = static_cast<int>(
                (static_cast<int64_t>(x) - left_bound) * guest.width
                / bound_width);
            uint8_t* target = root->rgba.data()
                + static_cast<size_t>(y) * root->stride
                + static_cast<size_t>(x) * 4;
            const uint8_t* source = guest.rgba.data()
                + static_cast<size_t>(source_y) * guest.stride
                + static_cast<size_t>(source_x) * 4;
            if (source[3] == 255) {
                std::memcpy(target, source, 4);
            } else if (source[3]) {
                const unsigned inverse = 255 - source[3];
                for (int channel = 0; channel < 4; ++channel) {
                    target[channel] = static_cast<uint8_t>(std::min(255u,
                        source[channel]
                            + (target[channel] * inverse + 127) / 255));
                }
            }
        }
    }
    return true;
}

} // namespace

void WebContents::requestCapture(
    std::function<void(RendererFrameSnapshot, std::string)> complete,
    unsigned depth)
{
    if (!isAlive()) {
        complete({}, "WebContents is destroyed");
        return;
    }
    if (depth >= renderer_protocol::kMaxValueDepth) {
        complete({}, "guest capture tree is too deep");
        return;
    }
    struct CaptureState {
        size_t remaining = 0;
        RendererFrameSnapshot root;
        std::map<int, RendererFrameSnapshot> guests;
        std::map<int, gfx::Rect> bounds;
        std::string error;
        std::function<void(RendererFrameSnapshot, std::string)> complete;
    };
    auto state = std::make_shared<CaptureState>();
    state->complete = std::move(complete);
    std::vector<int> guests;
    for (int id : m_guest_ids) {
        WebContents* guest = fromId(id);
        auto found = m_guest_bounds.find(id);
        if (guest && guest->m_host == this && guest->isAlive()
            && found != m_guest_bounds.end()) {
            guests.push_back(id);
            state->bounds.emplace(id, found->second);
        }
    }
    state->remaining = guests.size() + 1;
    auto finish_one = [state](int id, RendererFrameSnapshot frame,
                          std::string error) mutable {
        if (state->remaining == 0)
            return;
        if (state->error.empty() && !error.empty())
            state->error = std::move(error);
        if (id)
            state->guests.insert_or_assign(id, std::move(frame));
        else
            state->root = std::move(frame);
        if (--state->remaining)
            return;
        if (state->error.empty()) {
            for (auto& [guest_id, guest] : state->guests) {
                auto bounds = state->bounds.find(guest_id);
                if (bounds == state->bounds.end()
                    || !CompositeCapturedGuest(&state->root, guest,
                        bounds->second)) {
                    state->error = "guest capture pixels are invalid";
                    break;
                }
            }
        }
        state->complete(std::move(state->root), std::move(state->error));
    };
    for (int id : guests) {
        WebContents* guest = fromId(id);
        guest->requestCapture(
            [finish_one, id](RendererFrameSnapshot frame,
                std::string error) mutable {
                finish_one(id, std::move(frame), std::move(error));
            },
            depth + 1);
    }
    auto once = std::make_shared<bool>(false);
    auto root_complete = [finish_one, once](RendererFrameSnapshot frame,
                             std::string error) mutable {
        if (std::exchange(*once, true))
            return;
        finish_one(0, std::move(frame), std::move(error));
    };
    if (!m_renderer->RequestCapture(root_complete))
        root_complete({}, "Renderer capture could not be queued");
}
#else
void WebContents::requestCapture(std::function<void(std::string)> complete)
{
    if (!isAlive()) {
        complete("WebContents is destroyed");
        return;
    }
    const int contents_id = m_id;
    auto completion = std::make_shared<std::function<void(std::string)>>(std::move(complete));
    auto finish = [completion](std::string error) {
        if (!*completion)
            return;
        auto callback = std::move(*completion);
        callback(std::move(error));
    };
    auto capture_host = [contents_id, finish](std::string error) {
        WebContents* contents = WebContents::fromId(contents_id);
        if (!error.empty()) {
            finish(std::move(error));
            return;
        }
        if (!contents || !contents->isAlive()) {
            finish("WebContents closed before capture");
            return;
        }
        if (!contents->m_renderer->Request("capturePage", {},
                [finish](base::Value::Dict, std::string failure) {
                    finish(std::move(failure));
                }))
            finish("Renderer capture could not be queued");
    };
    std::vector<int> guests;
    for (int id : m_guest_ids) {
        WebContents* guest = fromId(id);
        if (guest && guest->m_host == this && guest->isAlive())
            guests.push_back(id);
    }
    if (guests.empty()) {
        capture_host({});
        return;
    }
    // The host renderer owns guest placement; refresh each browser-authorized
    // mapping before it composites the request's root readback.
    auto remaining = std::make_shared<size_t>(guests.size());
    auto failure = std::make_shared<std::string>();
    for (int id : guests) {
        WebContents* guest = fromId(id);
        guest->requestCapture([remaining, failure, capture_host](std::string error) {
            if (failure->empty() && !error.empty())
                *failure = std::move(error);
            if (--*remaining == 0)
                capture_host(std::move(*failure));
        });
    }
}
#endif

void WebContents::capturePageApi(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    auto promise = std::make_shared<gin_helper::Promise<v8::Local<v8::Object>>>(info.GetIsolate());
    info.GetReturnValue().Set(promise->GetHandle());
    int x = 0;
    int y = 0;
    int width = -1;
    int height = -1;
    bool has_width = false;
    bool has_height = false;
    if (info.Length() > 0 && info[0]->IsObject()) {
        gin_helper::Dictionary rect(info.GetIsolate(), info[0].As<v8::Object>());
        rect.GetBydefaultVal("x", 0, &x);
        rect.GetBydefaultVal("y", 0, &y);
        has_width = rect.Get("width", &width);
        has_height = rect.Get("height", &height);
    }
#if !BUILDFLAG(IS_WIN)
    const int contents_id = m_id;
#endif
    v8::Isolate* isolate = info.GetIsolate();
    auto resolve_capture = [isolate, promise, x, y, width, height,
                               has_width, has_height](
                               RendererFrameSnapshot frame,
                               std::string error) mutable {
        if (!error.empty()) {
            promise->RejectWithErrorMessage(error);
            return;
        }
        if (frame.width <= 0 || frame.height <= 0
            || frame.stride < frame.width * 4 || frame.rgba.empty()) {
            promise->Resolve(NativeImage::createEmpty(isolate));
            return;
        }
        if (!has_width)
            width = frame.width;
        if (!has_height)
            height = frame.height;
        x = std::max(0, std::min(x, frame.width));
        y = std::max(0, std::min(y, frame.height));
        width = std::max(0, std::min(width, frame.width - x));
        height = std::max(0, std::min(height, frame.height - y));
        if (!width || !height) {
            promise->Resolve(NativeImage::createEmpty(isolate));
            return;
        }
        // Transport already contains premultiplied RGBA. createFromRGBA would
        // multiply translucent colors a second time.
        SkBitmap bitmap;
        if (!bitmap.tryAllocPixels(SkImageInfo::MakeN32Premul(width, height))) {
            promise->Resolve(NativeImage::createEmpty(isolate));
            return;
        }
        const SkPixmap source(SkImageInfo::Make(frame.width, frame.height,
            kRGBA_8888_SkColorType, kPremul_SkAlphaType),
            frame.rgba.data(), frame.stride);
        if (!source.readPixels(bitmap.pixmap(), x, y)) {
            promise->Resolve(NativeImage::createEmpty(isolate));
            return;
        }
        bitmap.setImmutable();
        promise->Resolve(NativeImage::createFromBitmap(isolate, bitmap));
    };
#if BUILDFLAG(IS_WIN)
    requestCapture(std::move(resolve_capture));
#else
    requestCapture([contents_id,
                       resolve_capture = std::move(resolve_capture)](
                       std::string error) mutable {
        RendererFrameSnapshot frame;
        WebContents* contents = WebContents::fromId(contents_id);
        if (error.empty()
            && (!contents || !contents->copyFrame(&frame, true)))
            error = "WebContents closed before capture";
        resolve_capture(std::move(frame), std::move(error));
    });
#endif
}

void WebContents::invalidateApi()
{
    if (isAlive())
        m_renderer->Send("invalidate", {});
    m_observers.Notify(&WebContentsObserver::onWebContentsPaint, this, gfx::Rect());
}

v8::Local<v8::Value> WebContents::getOwnerBrowserWindowApi()
{
    return m_owner
        ? m_owner->getWrapper().As<v8::Value>()
        : v8::Null(isolate()).As<v8::Value>();
}

void WebContents::setWindowOpenHandlerApi(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    if (info.Length() != 1 || !info[0]->IsFunction()) {
        info.GetIsolate()->ThrowException(v8::Exception::TypeError(
            gin_helper::StringToV8(info.GetIsolate(), "setWindowOpenHandler requires a function")));
        return;
    }
    m_window_open_handler.Reset(info.GetIsolate(), info[0].As<v8::Function>());
}

std::unique_ptr<WindowOpenHandlerResult> WebContents::onWindowOpenHandler(
    v8::Local<v8::Context> context, const base::Value::Dict& details)
{
    if (m_window_open_handler.IsEmpty())
        return nullptr;
    v8::Local<v8::Function> callback = m_window_open_handler.Get(isolate());
    v8::Local<v8::Value> argv[] = { gin_helper::ConvertToV8(isolate(), details) };
    v8::MaybeLocal<v8::Value> maybe_result = callback->Call(
        context, v8::Undefined(isolate()), 1, argv);
    if (maybe_result.IsEmpty() || !maybe_result.ToLocalChecked()->IsObject())
        return nullptr;
    gin_helper::Dictionary result(isolate(), maybe_result.ToLocalChecked().As<v8::Object>());
    auto parsed = std::make_unique<WindowOpenHandlerResult>();
    std::string action;
    if (result.Get("action", &action))
        parsed->isDeny = action == "deny";
    result.Get("overrideBrowserWindowOptions", &parsed->overrideBrowserWindowOptions);
    return parsed;
}

void WebContents::emitRendererEvent(const std::string& type,
    const base::Value::Dict& payload, uint64_t request_id)
{
    mate::EventEmitter<WebContents>::emit("_renderer-event", type, payload, request_id);
}

void WebContents::onRendererEvent(base::Value::Dict event)
{
    if (m_destroyed)
        return;
    // A native observer can synchronously destroy this contents and release its
    // persistent wrapper. Keep it alive until dispatch bookkeeping has unwound.
    v8::HandleScope handle_scope(isolate());
    [[maybe_unused]] const v8::Local<v8::Object> keep_alive = GetWrapper(isolate());
    const std::string type = ReadString(event, "type");
    const base::Value::Dict& payload = PayloadFrom(event);
    const uint64_t request_id = RequestIdFrom(event);
    if (type == "context-menu") {
        uint64_t frame_id = 0;
        base::Value::Dict* params = event.FindDict("payload");
        if (!params || !ReadFrameIdentity(event, "frameId", &frame_id)
            || !isRendererFrameCurrent(frame_id))
            return;
        // Frame identity and document URLs belong to the authenticated
        // transport, not to the renderer's hit-test payload.
        params->Set("pageURL", m_url);
        params->Set("frameURL", m_renderer_frames.at(frame_id).committed_url);
        emitRendererEvent(type, *params, request_id);
        return;
    }
    if (type == "text-input-state") {
        // Renderer snapshots are asynchronous; never let a late snapshot
        // restore a text client after native focus has already left.
        if (m_focused) {
            m_observers.Notify(&WebContentsObserver::onWebContentsTextInputState, this, payload);
        }
        return;
    }
    if (type == "native-drag-materialized") {
        const std::string* generation_text = payload.FindString("generation");
        std::optional<bool> success = payload.FindBool("success");
        uint64_t generation = 0;
        if (generation_text && success
            && base::StringToUint64(*generation_text, &generation)
            && generation == m_native_drag_generation
            && base::NumberToString(generation) == *generation_text) {
            if (ApiSession* session =
                    SessionMgr::get()->findOrCreateSession(
                        isolate(), m_partition, false)) {
                for (const std::string& token : m_native_drag_tokens)
                    session->revokeFile(token);
            }
            m_native_drag_tokens.clear();
            if (!*success)
                m_native_drag_active = false;
        }
        return;
    }
    if (type == "native-drag-operation") {
        const std::string* generation_text = payload.FindString("generation");
        std::optional<bool> accepted = payload.FindBool("accepted");
        uint64_t generation = 0;
        if (generation_text && accepted
            && base::StringToUint64(*generation_text, &generation)
            && generation == m_native_drag_generation
            && base::NumberToString(generation) == *generation_text) {
            m_native_drag_operation_known = true;
            m_native_drag_operation_accepted = *accepted;
        }
        return;
    }



    if (type == "frame-navigated-in-page") {
        uint64_t frame_id = 0;
        uint64_t parent_id = 0;
        std::optional<bool> main_frame = payload.FindBool("isMainFrame");
        if (!ReadFrameIdentity(event, "frameId", &frame_id)
            || !ReadFrameIdentity(payload, "parentFrameId", &parent_id, true)
            || !main_frame || !isRendererFrameCurrent(frame_id))
            return;
        RendererFrameState& frame = m_renderer_frames.at(frame_id);
        GURL target(ReadString(payload, "url"));
        if (!target.is_valid() || frame.committed_url.empty()
            || frame.parent_id != parent_id || frame.main_frame != *main_frame)
            return;
        url::Origin target_origin = url::Origin::Create(target);
        if (!target.IsAboutBlank() && !target.IsAboutSrcdoc()
            && (frame.committed_origin.empty()
                    ? !target_origin.opaque()
                    : target_origin.opaque()
                        || target_origin.Serialize() != frame.committed_origin))
            return;
        const std::string url = target.spec();
        mate::EventEmitter<WebContents>::emit("did-start-navigation", url,
            true, *main_frame, getProcessIdApi(), m_id);
        if (!isAlive())
            return;
        frame.committed_url = url;
        if (*main_frame)
            m_url = url;
        mate::EventEmitter<WebContents>::emit("did-navigate-in-page", url,
            *main_frame, getProcessIdApi(), m_id);
        return;
    }

    if (type == "frame-created" || type == "frame-committed") {
        uint64_t frame_id = 0;
        uint64_t parent_id = 0;
        std::optional<bool> main_frame = payload.FindBool("isMainFrame");
        if (!ReadFrameIdentity(event, "frameId", &frame_id)
            || !ReadFrameIdentity(payload, "parentFrameId", &parent_id, true)
            || !main_frame
            || !registerRendererFrame(frame_id, parent_id, *main_frame))
            return;
        if (type == "frame-committed") {
            RendererFrameState& frame = m_renderer_frames.at(frame_id);
            const std::string committed_url = ReadString(payload, "url");
            const GURL committed_target(committed_url);
            const bool frame_ready_navigation =
                frame.navigation.token == m_ready_to_show_navigation_token
                && (SameDocumentUrl(frame.navigation.url, committed_url)
                    || SameDocumentUrl(frame.response_url, committed_url));
            // Unbrokered data/about documents transfer the pending navigation
            // into the frame inside commitRendererFrame(), so recognize the
            // same token and URL before that move as well.
            const bool pending_ready_navigation =
                m_pending_main_navigation.token
                    == m_ready_to_show_navigation_token
                && SameDocumentUrl(
                    m_pending_main_navigation.url, committed_url);
            const bool expected_ready_navigation = *main_frame
                && !m_ready_to_show_emitted
                && !m_ready_to_show_navigation_token.empty()
                && (frame_ready_navigation || pending_ready_navigation);
            const bool first_renderer_navigation = *main_frame
                && !m_ready_to_show_emitted
                && m_ready_to_show_navigation_token.empty()
                && committed_target.is_valid()
                && !committed_target.IsAboutBlank()
                && !committed_target.IsAboutSrcdoc();
            const uint64_t previous_generation = frame.generation;
            if (!commitRendererFrame(frame_id, committed_url))
                return;
            if (expected_ready_navigation || first_renderer_navigation) {
                m_ready_to_show_armed = true;
                m_ready_to_show_navigation_token.clear();
                if (m_type == "window") {
                    m_initial_paint_pending = true;
                    updateVisibility();
                }
            }
            ApiSession* session = SessionMgr::get()->findOrCreateSession(
                isolate(), m_partition, false);
            if (session) {
                session->revokeFilesForFrame(
                    m_id, frame_id, previous_generation);
            }
            const std::string& canonical_url =
                m_renderer_frames.at(frame_id).committed_url;
            if (*main_frame) {
                m_native_drag_tokens.clear();
                m_native_drag_active = false;
                m_url = canonical_url;
                mate::EventEmitter<WebContents>::emit("did-navigate", m_url);
                if (!isAlive())
                    return;
            }
            mate::EventEmitter<WebContents>::emit("did-frame-navigate",
                canonical_url, 200, "", *main_frame, getProcessIdApi(), m_id);
        }
        return;
    }
    if (type == "frame-detached") {
        uint64_t frame_id = 0;
        if (ReadFrameIdentity(event, "frameId", &frame_id)) {
            auto frame = m_renderer_frames.find(frame_id);
            if (frame != m_renderer_frames.end()) {
                retireRendererFrameSockets(frame_id);
                if (ApiSession* session = SessionMgr::get()->findOrCreateSession(
                        isolate(), m_partition, false)) {
                    session->revokeFilesForFrame(
                        m_id, frame_id, frame->second.generation);
                }
                m_renderer_frames.erase(frame);
            }
            if (m_main_frame_id == frame_id) {
                m_native_drag_tokens.clear();
                m_native_drag_active = false;
                m_main_frame_id = 0;
            }
            mate::EventEmitter<WebContents>::emit(
                "_renderer-frame-detached", base::NumberToString(frame_id));
        }
        return;
    }

    if (type == "ipc-invoke" || type == "ipc-message" || type == "ipc-message-sync") {
        uint64_t frame_id = 0;
        if (!ReadFrameIdentity(event, "frameId", &frame_id)
            || !isRendererFrameCurrent(frame_id)) {
            if (request_id && m_renderer)
                m_renderer->Respond(request_id, {}, "IPC source frame is unavailable");
            return;
        }
        // Use the transport's source frame, never payload-supplied identity.
        v8::Local<v8::Object> ipc_event =
            mate::internal::createJSEvent(isolate(), getWrapper());
        gin_helper::Dictionary descriptor(isolate(), ipc_event);
        descriptor.Set("frameId", base::NumberToString(frame_id));
        descriptor.Set("frameGeneration",
            base::NumberToString(m_renderer_frames.at(frame_id).generation));
        mate::EventEmitter<WebContents>::emitCustomEvent(
            "_renderer-event", ipc_event, type, payload, request_id);
        return;
    }

    if (type == "broker-request") {
        ApiSession* session = SessionMgr::get()->findOrCreateSession(
            isolate(), m_partition, false);
        if (!session || !request_id || !m_renderer) {
            if (request_id && m_renderer)
                m_renderer->Respond(request_id, {},
                    "Owning Session is unavailable for broker request");
            return;
        }
        uint64_t frame_id = 0;
        if (!ReadFrameIdentity(event, "frameId", &frame_id)) {
            m_renderer->Respond(request_id, {}, "Invalid broker frame identity");
            return;
        }
        const std::string url = ReadString(payload, "url");
        const std::string method = ReadString(payload, "method");
        const std::string token = ReadString(payload, "navigationToken");
        const bool document_request =
            payload.FindBool("isDocument").value_or(false);
        if (document_request && !m_pending_main_navigation.token.empty()
            && token == m_pending_main_navigation.token
            && SameDocumentUrl(url, m_pending_main_navigation.url)
            && method == m_pending_main_navigation.method) {
            if (!registerRendererFrame(frame_id, 0, true)) {
                m_renderer->Respond(request_id, {},
                    "Main navigation frame identity changed");
                return;
            }
            RendererFrameState& main = m_renderer_frames.at(frame_id);
            main.navigation = std::move(m_pending_main_navigation);
            m_pending_main_navigation = {};
        }
        if (!isRendererFrameCurrent(frame_id)) {
            m_renderer->Respond(request_id, {},
                "Broker frame is unknown or belongs to a replaced document");
            return;
        }
        RendererFrameState& frame = m_renderer_frames.at(frame_id);
        ApiSession::BrokerFrameContext context;
        context.frame_id = frame_id;
        context.frame_generation = frame.generation;
        context.committed_url = frame.committed_url;
        context.committed_origin = frame.committed_origin;
        auto top = m_renderer_frames.find(m_main_frame_id);
        if (top != m_renderer_frames.end())
            context.top_origin = top->second.committed_origin;
        context.web_security =
            m_web_preferences.FindBool("webSecurity").value_or(true);
        context.allow_running_insecure_content = m_web_preferences
            .FindBool("allowRunningInsecureContent").value_or(false);
        context.application_resource_roots = m_application_resource_roots;
        std::string navigation_token;
        std::string navigation_url;
        if (document_request) {
            if (token.empty() || token != frame.navigation.token
                || frame.navigation.consumed
                || !SameDocumentUrl(url, frame.navigation.url)
                || method != frame.navigation.method) {
                m_renderer->Respond(request_id, {},
                    "Document request has no matching navigation authorization");
                return;
            }
            navigation_token = frame.navigation.token;
            frame.navigation.consumed = true;
            navigation_url = frame.navigation.url;
            // Scope downstream approval to the exact resource URL validated
            // above; the navigation URL may also contain a document fragment.
            context.approved_navigation_url = url;
            context.approved_navigation_method = frame.navigation.method;
            context.navigation_initiator_origin =
                frame.navigation.initiator_origin;
            context.browser_approved_navigation = true;
            context.browser_initiated_navigation =
                frame.navigation.browser_initiated;
        } else if (frame.committed_url.empty()) {
            m_renderer->Respond(request_id, {},
                "Frame has not committed an authorized document");
            return;
        }
#if BUILDFLAG(IS_MAC) || BUILDFLAG(IS_WIN)
        if (ReadString(payload, "kind") == "clipboard") {
            base::Value::Dict result;
            const std::string operation = ReadString(payload, "operation");
            if (operation == "sequence") {
                const mini_electron::ClipboardSequence sequence =
                    mini_electron::GetClipboardSequence();
                result.Set("high", base::NumberToString(sequence.high));
                result.Set("low", base::NumberToString(sequence.low));
            } else if (operation == "readText") {
                result.Set("value", base::UTF16ToUTF8(
                    mini_electron::ReadClipboardText()));
            } else if (operation == "readHtml") {
                const mini_electron::ClipboardHtml html =
                    mini_electron::ReadClipboardHtml();
                result.Set("markup", base::UTF16ToUTF8(html.markup));
                result.Set("sourceUrl", html.source_url);
                result.Set("fragmentStart",
                    static_cast<int>(html.fragment_start));
                result.Set("fragmentEnd",
                    static_cast<int>(html.fragment_end));
            } else if (operation == "readPng") {
                std::vector<uint8_t> png =
                    mini_electron::ReadClipboardPng();
                result.Set("data", base::Value(std::move(png)));
            } else if (operation == "availableFormats") {
                base::Value::List formats;
                for (const auto& format :
                    mini_electron::AvailableClipboardFormats()) {
                    formats.Append(base::UTF16ToUTF8(format));
                }
                result.Set("formats", std::move(formats));
            } else if (operation == "write") {
                std::optional<std::u16string> text;
                std::optional<std::u16string> html;
                std::optional<std::string> html_source_url;
                std::optional<std::vector<uint8_t>> png;
                if (const std::string* value = payload.FindString("text"))
                    text = base::UTF8ToUTF16(*value);
                if (const std::string* value = payload.FindString("html"))
                    html = base::UTF8ToUTF16(*value);
                if (const std::string* value =
                    payload.FindString("htmlSourceUrl")) {
                    html_source_url = *value;
                }
                if (const base::Value::BlobStorage* value =
                    payload.FindBlob("png")) {
                    if (value->empty()
                        || value->size()
                            > mini_electron::kMaximumClipboardImageBytes) {
                        m_renderer->Respond(request_id, {},
                            "Invalid clipboard image payload");
                        return;
                    }
                    png = *value;
                }
                mini_electron::ClipboardContent content;
                content.text = text ? &*text : nullptr;
                content.html = html ? &*html : nullptr;
                content.html_source_url = html_source_url
                    ? &*html_source_url : nullptr;
                content.png = png ? &*png : nullptr;
                result.Set("value",
                    mini_electron::WriteClipboard(content));
            } else {
                m_renderer->Respond(request_id, {},
                    "Unsupported clipboard operation");
                return;
            }
            if (!m_renderer->Respond(request_id, std::move(result), {})) {
                m_renderer->Respond(request_id, {},
                    "Clipboard response exceeded the IPC limit");
            }
            return;
        }
#endif
        const int contents_id = m_id;
        const uint64_t generation = frame.generation;
        session->handleBrokerRequest(contents_id, context, payload,
            [contents_id, frame_id, generation, request_id,
                navigation_token = std::move(navigation_token),
                navigation_url = std::move(navigation_url)](
                base::Value::Dict result, std::string error) mutable {
                WebContents* contents = WebContents::fromId(contents_id);
                if (!contents || !contents->m_renderer)
                    return;
                auto found = contents->m_renderer_frames.find(frame_id);
                if (!contents->isRendererFrameCurrent(frame_id)
                    || found->second.generation != generation) {
                    contents->m_renderer->Respond(request_id, {},
                        "Broker frame document was replaced");
                    return;
                }
                if (!navigation_token.empty()) {
                    RendererFrameState& current = found->second;
                    if (current.navigation.token != navigation_token
                        || !current.navigation.consumed
                        || !SameDocumentUrl(
                            current.navigation.url, navigation_url)) {
                        contents->m_renderer->Respond(request_id, {},
                            "Navigation was superseded");
                        return;
                    }
                    if (error.empty()) {
                        const std::string* final_url =
                            result.FindString("finalUrl");
                        current.response_url =
                            final_url ? *final_url : navigation_url;
                    }
                }
                contents->m_renderer->Respond(
                    request_id, std::move(result), std::move(error));
            });
        return;
    }
    if (type == "native-window-command") {
        uint64_t frame_id = 0;
        const bool current = ReadFrameIdentity(event, "frameId", &frame_id)
            && isRendererFrameCurrent(frame_id);
        const std::string command = ReadString(payload, "command");
        const int contents_id = m_id;
        const bool granted = current && m_owner && !m_owner->isClosed()
            && m_owner->handleNativeWindowCommand(command, payload);
        WebContents* contents = WebContents::fromId(contents_id);
        if (request_id && contents && contents->m_renderer) {
            base::Value::Dict result;
            result.Set("granted", granted);
            contents->m_renderer->Respond(request_id, std::move(result), {});
        }
        return;
    }
    if (type == "file-chooser") {
        uint64_t frame_id = 0;
        if (!request_id || !ReadFrameIdentity(event, "frameId", &frame_id)
            || !isRendererFrameCurrent(frame_id)
            || m_renderer_frames.at(frame_id).committed_url.empty()) {
            if (request_id && m_renderer)
                m_renderer->Respond(request_id, {}, "File chooser frame is unavailable");
            return;
        }
        m_file_chooser_frames[request_id] = {
            frame_id, m_renderer_frames.at(frame_id).generation
        };
        emitRendererEvent(type, payload, request_id);
        return;
    }
    if (type == "permission-request") {
        ApiSession* session = SessionMgr::get()->findOrCreateSession(
            isolate(), m_partition, false);
        const std::string permission = ReadString(payload, "permission");
        const bool is_request = payload.FindBool("isRequest").value_or(false);
        const int contents_id = m_id;
        uint64_t frame_id = 0;
        const bool known_frame = ReadFrameIdentity(event, "frameId", &frame_id)
            && isRendererFrameCurrent(frame_id)
            && !m_renderer_frames.at(frame_id).committed_url.empty();
        const uint64_t generation = known_frame
            ? m_renderer_frames.at(frame_id).generation : 0;
        auto respond = [contents_id, request_id, frame_id, generation](bool granted) {
            WebContents* contents = WebContents::fromId(contents_id);
            if (!contents || !contents->m_renderer || !request_id)
                return;
            base::Value::Dict result;
            granted = granted && contents->isRendererFrameCurrent(frame_id)
                && contents->m_renderer_frames.at(frame_id).generation == generation;
            result.Set("granted", granted);
            contents->m_renderer->Respond(request_id, std::move(result), {});
        };
        if (!session || !request_id || !known_frame || !IsKnownPermission(permission)) {
            respond(false);
            return;
        }
        const RendererFrameState& frame = m_renderer_frames.at(frame_id);
        base::Value::Dict details = payload.Clone();
        const std::string origin = frame.committed_origin.empty()
            ? "null" : frame.committed_origin;
        details.Set("requestingUrl", frame.committed_url);
        details.Set("requestingOrigin", origin);
        details.Set("securityOrigin", origin);
        details.Set("embeddingOrigin",
            m_renderer_frames.at(m_main_frame_id).committed_origin);
        details.Set("isMainFrame", frame.main_frame);
        if (is_request)
            session->requestPermission(contents_id, permission, details, std::move(respond));
        else
            respond(session->checkPermission(contents_id, permission, details));
        return;
    }
    if (type == "javascript-dialog-opening") {
        const std::string dialog_type = ReadString(payload, "type");
        const bool known = dialog_type == "alert" || dialog_type == "confirm"
            || dialog_type == "prompt" || dialog_type == "beforeunload";
        bool handled = !known;
        bool accepted = false;
        std::string prompt_text;
        if (known) {
            v8::Local<v8::Object> dialog_event = mate::internal::createJSEvent(
                isolate(), getWrapper());
            gin_helper::Dictionary dialog_event_dict(isolate(), dialog_event);
            dialog_event_dict.Set("type", dialog_type);
            dialog_event_dict.Set("message", ReadString(payload, "message"));
            dialog_event_dict.Set("defaultPrompt",
                ReadString(payload, "defaultPrompt"));
            if (std::optional<int> dialog_id = payload.FindInt("dialogId"))
                dialog_event_dict.Set("dialogId", *dialog_id);
            else if (std::optional<double> dialog_id = payload.FindDouble("dialogId"))
                dialog_event_dict.Set("dialogId", *dialog_id);
            mate::EventEmitter<WebContents>::emitCustomEvent(
                "javascript-dialog-opening", dialog_event, payload);
            v8::Local<v8::Value> return_value;
            if (dialog_event->Get(isolate()->GetCurrentContext(),
                    gin_helper::StringToV8(isolate(), "returnValue"))
                    .ToLocal(&return_value)) {
                if (return_value->IsBoolean()) {
                    handled = true;
                    accepted = return_value->BooleanValue(isolate());
                } else if (return_value->IsObject()) {
                    gin_helper::Dictionary response(
                        isolate(), return_value.As<v8::Object>());
                    handled = response.Get("accepted", &accepted);
                    response.Get("promptText", &prompt_text);
                }
            }
        }
        if (request_id && m_renderer) {
            base::Value::Dict result;
            result.Set("handled", handled);
            result.Set("accepted", accepted);
            result.Set("promptText", prompt_text);
            if (const base::Value* dialog_id = payload.Find("dialogId"))
                result.Set("dialogId", dialog_id->Clone());
            m_renderer->Respond(request_id, std::move(result), {});
        }
        return;
    }



    if (type == "navigation-started") {
        const std::string url = ReadString(payload, "url");
        const std::string method = ReadString(payload, "method");
        uint64_t frame_id = 0;
        uint64_t parent_id = 0;
        std::optional<bool> main_frame = payload.FindBool("isMainFrame");
        bool prevented = !request_id
            || !ReadFrameIdentity(event, "frameId", &frame_id)
            || !ReadFrameIdentity(payload, "parentFrameId", &parent_id, true)
            || !main_frame;
        // Programmatic loadURL navigations are already approved by the browser.
        // Only renderer-initiated navigations emit cancellable will-* events.
        const bool browser_initiated = main_frame.value_or(false)
            && !m_pending_main_navigation.token.empty()
            && m_pending_main_navigation.browser_initiated
            && SameDocumentUrl(url, m_pending_main_navigation.url)
            && method == m_pending_main_navigation.method;
        if (!prevented && !browser_initiated) {
            v8::Local<v8::Object> frame_event = mate::internal::createJSEvent(
                isolate(), getWrapper());
            gin_helper::Dictionary frame_event_dict(isolate(), frame_event);
            frame_event_dict.Set("url", url);
            frame_event_dict.Set("isMainFrame", *main_frame);
            prevented = mate::EventEmitter<WebContents>::emitCustomEvent(
                "will-frame-navigate", frame_event, url, false, *main_frame,
                getProcessIdApi(), m_id);
            if (*main_frame) {
                v8::Local<v8::Object> navigation_event =
                    mate::internal::createJSEvent(isolate(), getWrapper());
                gin_helper::Dictionary navigation_event_dict(
                    isolate(), navigation_event);
                navigation_event_dict.Set("url", url);
                prevented = mate::EventEmitter<WebContents>::emitCustomEvent(
                    "will-navigate", navigation_event, url) || prevented;
            }
        }
        std::string token;
        if (!prevented && isAlive())
            token = approveFrameNavigation(
                frame_id, parent_id, *main_frame, url, method);
        if (token.empty())
            prevented = true;
        if (request_id && m_renderer) {
            base::Value::Dict result;
            result.Set("allow", !prevented);
            if (!prevented)
                result.Set("navigationToken", token);
            m_renderer->Respond(request_id, std::move(result), {});
        }
        if (prevented) {
            if (SameDocumentUrl(url, m_pending_main_navigation.url))
                m_pending_main_navigation = {};
            return;
        }
        const bool started_loading = *main_frame && !m_isLoading;
        if (started_loading)
            m_isLoading = true;
        mate::EventEmitter<WebContents>::emit("did-start-navigation", url,
            false, *main_frame, getProcessIdApi(), m_id);
        if (started_loading && isAlive())
            mate::EventEmitter<WebContents>::emit("did-start-loading");
    } else if (type == "navigation-redirect") {
        const std::string url = ReadString(payload, "url");
        v8::Local<v8::Object> redirect_event = mate::internal::createJSEvent(
            isolate(), getWrapper());
        gin_helper::Dictionary redirect_event_dict(isolate(), redirect_event);
        redirect_event_dict.Set("url", url);
        const bool prevented = mate::EventEmitter<WebContents>::emitCustomEvent(
            "will-redirect", redirect_event, url, false, true,
            getProcessIdApi(), m_id);
        if (request_id && m_renderer) {
            base::Value::Dict result;
            result.Set("allow", !prevented);
            m_renderer->Respond(request_id, std::move(result), {});
        }
        if (prevented)
            return;
    } else if (type == "navigation-committed") {
        uint64_t frame_id = 0;
        if (!ReadFrameIdentity(event, "frameId", &frame_id)
            || !isRendererFrameCurrent(frame_id)
            || frame_id != m_main_frame_id)
            return;
        m_canGoBack = payload.FindBool("canGoBack").value_or(false);
        m_canGoForward = payload.FindBool("canGoForward").value_or(false);
    } else if (type == "navigation-finished") {
        uint64_t frame_id = 0;
        if (!ReadFrameIdentity(event, "frameId", &frame_id)
            || !isRendererFrameCurrent(frame_id))
            return;
        const RendererFrameState& frame = m_renderer_frames.at(frame_id);
        if (!SameDocumentUrl(ReadString(payload, "url"), frame.committed_url)
            || (frame.main_frame && !m_pending_main_navigation.token.empty()))
            return;
        const bool main_frame = frame.main_frame;
        mate::EventEmitter<WebContents>::emit("did-frame-finish-load", main_frame);
        if (main_frame && isAlive()) {
            m_isLoading = false;
            mate::EventEmitter<WebContents>::emit("did-finish-load");
            if (isAlive())
                mate::EventEmitter<WebContents>::emit("did-stop-loading");
        }
    } else if (type == "navigation-failed") {
        uint64_t frame_id = 0;
        if (!ReadFrameIdentity(event, "frameId", &frame_id)
            || !isRendererFrameCurrent(frame_id))
            return;
        RendererFrameState& frame = m_renderer_frames.at(frame_id);
        const std::string failed_url = ReadString(payload, "url");
        if (!SameDocumentUrl(failed_url, frame.navigation.url)
            && !SameDocumentUrl(failed_url, frame.committed_url))
            return;
        const bool main_frame = frame.main_frame;
        const bool failed_ready_navigation = main_frame
            && !m_ready_to_show_navigation_token.empty()
            && (frame.navigation.token == m_ready_to_show_navigation_token
                || m_pending_main_navigation.token
                    == m_ready_to_show_navigation_token);
        frame.navigation = {};
        frame.response_url.clear();
        if (main_frame) {
            m_isLoading = false;
            m_pending_main_navigation = {};
            if (failed_ready_navigation) {
                m_ready_to_show_armed = false;
                m_ready_to_show_navigation_token.clear();
                m_initial_paint_pending = false;
                updateVisibility();
            }
        }
        mate::EventEmitter<WebContents>::emit("did-fail-load",
            payload.FindInt("errorCode").value_or(-2), ReadString(payload, "error"),
            failed_url, main_frame);
        if (main_frame && isAlive())
            mate::EventEmitter<WebContents>::emit("did-stop-loading");
    } else if (type == "loading-state") {
        uint64_t frame_id = 0;
        if (!ReadFrameIdentity(event, "frameId", &frame_id)
            || !isRendererFrameCurrent(frame_id)
            || frame_id != m_main_frame_id)
            return;
        const bool loading = payload.FindBool("loading").value_or(false);
        if (loading == m_isLoading
            || (!loading && !m_pending_main_navigation.token.empty()))
            return;
        m_isLoading = loading;
        mate::EventEmitter<WebContents>::emit(loading ? "did-start-loading" : "did-stop-loading");
    } else if (type == "title-changed") {
        m_title = ReadString(payload, "title");
        mate::EventEmitter<WebContents>::emit("page-title-updated", m_title);
    } else if (type == "target-url") {
        mate::EventEmitter<WebContents>::emit("update-target-url", ReadString(payload, "url"));
    } else if (type == "favicon-urls") {
        std::vector<std::string> urls;
        if (const auto* list = payload.FindList("urls")) {
            urls.reserve(list->size());
            for (const auto& value : *list) {
                if (value.is_string())
                    urls.push_back(value.GetString());
            }
        }
        mate::EventEmitter<WebContents>::emit("page-favicon-updated", urls);
    } else if (type == "dom-ready") {
        uint64_t frame_id = 0;
        if (!ReadFrameIdentity(event, "frameId", &frame_id)
            || !isRendererFrameCurrent(frame_id)
            || frame_id != m_main_frame_id)
            return;
        mate::EventEmitter<WebContents>::emit("dom-ready");
    } else if (type == "console-message") {
        mate::EventEmitter<WebContents>::emit("console-message",
            payload.FindInt("level").value_or(0), ReadString(payload, "message"),
            payload.FindInt("line").value_or(0), ReadString(payload, "sourceId"));
    } else if (type == "before-input-event") {
        bool prevented = mate::EventEmitter<WebContents>::emit("before-input-event", payload);
        base::Value::Dict result;
        result.Set("preventDefault", prevented);
        if (request_id && m_renderer)
            m_renderer->Respond(request_id, std::move(result), {});
    } else if (type == "window-open") {
        uint64_t frame_id = 0;
        if (!ReadFrameIdentity(event, "frameId", &frame_id)
            || !isRendererFrameCurrent(frame_id)
            || m_renderer_frames.at(frame_id).committed_url.empty())
            return;
        std::unique_ptr<WindowOpenHandlerResult> result = onWindowOpenHandler(
            isolate()->GetCurrentContext(), payload);
        const bool denied = result && result->isDeny;
        base::Value::Dict response;
        response.Set("action", denied ? "deny" : "allow");
        if (result && !result->overrideBrowserWindowOptions.empty())
            response.Set("overrideBrowserWindowOptions",
                result->overrideBrowserWindowOptions.Clone());
        if (request_id && m_renderer)
            m_renderer->Respond(request_id, response.Clone(), {});
        base::Value::Dict forwarded = payload.Clone();
        forwarded.Set("action", denied ? "deny" : "allow");
        if (result && !result->overrideBrowserWindowOptions.empty())
            forwarded.Set("overrideBrowserWindowOptions",
                result->overrideBrowserWindowOptions.Clone());
        emitRendererEvent(type, forwarded, request_id);
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
    } else if (type == "cdp-message") {
        if (m_devtools_gateway) {
            const std::string* method = payload.FindString("method");
            const base::Value::Dict* params = payload.FindDict("params");
            if (method) {
                base::Value::Dict empty_params;
                m_devtools_gateway->ForwardProtocolMessage(
                    *method, params ? *params : empty_params);
            }
        }
        emitRendererEvent(type, payload, request_id);
    } else if (type == "cdp-detached") {
        if (m_devtools_gateway) {
            const std::string* reason = payload.FindString("reason");
            m_devtools_gateway->OnProtocolDetached(
                reason ? *reason : "target closed");
        }
        m_debugger_attached = false;
        emitRendererEvent(type, payload, request_id);
#endif
    } else if (type == "draggable-regions") {
        const base::Value::List* regions = payload.FindList("regions");
        if (regions) {
            m_observers.Notify(&WebContentsObserver::onWebContentsDraggableRegions, this, *regions);
        }
    } else if (type == "paint") {
        const bool release_initial_paint = m_initial_paint_pending
            && (m_ready_to_show_armed
                || m_ready_to_show_navigation_token.empty());
        if (release_initial_paint)
            m_initial_paint_pending = false;
        gfx::Rect damage;
        if (const base::Value::Dict* dirty = payload.FindDict("dirty")) {
            const auto x = dirty->FindInt("x");
            const auto y = dirty->FindInt("y");
            const auto width = dirty->FindInt("width");
            const auto height = dirty->FindInt("height");
            if (x && y && width && height && *x >= 0 && *y >= 0
                && *width > 0 && *height > 0)
                damage = gfx::Rect(*x, *y, *width, *height);
        }
#if defined(_WIN32)
        // Import/release even when hidden or unattached: the renderer's GPU
        // swap cannot advance until this authenticated consumer acknowledgement.
        copyFrame(&m_windowsPaintFrame);
        if (m_host && m_host->isAlive()) {
            WebContents* host = m_host;
            while (host->m_host && host->m_host->isAlive())
                host = host->m_host;
            const HWND window = host->m_owner
                ? host->m_owner->getHWND() : nullptr;
            const bool native_composition = window
                && ::GetPropW(window, L"MiniElectronDirectComposition");
            if (!native_composition && !m_windowsPaintFrame.gpu_frame) {
                m_host->m_renderer->Send("guest.invalidate",
                    base::Value::Dict().Set("guestContentsId", m_id));
            }
            host->m_observers.Notify(&WebContentsObserver::onWebContentsPaint, host, gfx::Rect());
        }
        // A prior stale-source repaint may have missed damage from an older
        // generation. Refresh all pixels once a new publication is available.
        if (m_windowsPaintNeedsFullRepaint) {
            damage = {};
            m_windowsPaintNeedsFullRepaint = false;
        }
#endif
#if BUILDFLAG(IS_MAC)
        const double native_context = payload.FindDouble("nativeContextId").value_or(0.0);
        m_native_context_id = std::isfinite(native_context) && native_context > 0
            && native_context <= std::numeric_limits<uint32_t>::max()
            ? static_cast<uint32_t>(native_context) : 0;
        if (m_host && m_host->isAlive() && m_host->m_native_context_id) {
            const int width = payload.FindInt("width").value_or(0);
            const int height = payload.FindInt("height").value_or(0);
            const double scale = payload.FindDouble("nativeScaleFactor").value_or(1.0);
            // Remote contexts animate independently. Update the embedding only
            // for new context/geometry; CPU fallback guests need pixel refreshes.
            if (!m_native_context_id || m_guest_context_id != m_native_context_id
                || m_guest_native_width != width || m_guest_native_height != height
                || m_guest_native_scale != scale) {
                m_guest_context_id = m_native_context_id;
                m_guest_native_width = width;
                m_guest_native_height = height;
                m_guest_native_scale = scale;
                m_host->m_renderer->Send("guest.invalidate",
                    base::Value::Dict().Set("guestContentsId", m_id));
            }
        }
        else if (m_host && m_host->isAlive() && m_native_context_id) {
            // A CPU-fallback embedder consumes actual guest pixels. Keep one
            // explicit readback in flight rather than an unbounded request queue.
            captureForSoftwareHost();
        }
        else if (m_host && m_host->isAlive()) {
            m_host->m_renderer->Send("guest.invalidate",
                base::Value::Dict().Set("guestContentsId", m_id));
        }
#endif
        m_observers.Notify(&WebContentsObserver::onWebContentsPaint, this, damage);
        completePreparedFrames();
        if (!m_ready_to_show_emitted && m_ready_to_show_armed) {
            m_ready_to_show_emitted = true;
            m_ready_to_show_armed = false;
            m_ready_to_show_navigation_token.clear();
            updateVisibility();
            m_observers.Notify(&WebContentsObserver::onWebContentsReadyToShow, this);
        } else if (release_initial_paint) {
            updateVisibility();
        }
    } else if (type == "close") {
        if (m_owner)
            m_owner->close();
        else
            destroyed();
    } else if (type == "crashed") {
        m_crashed = true;
        closeRenderer();
        mate::EventEmitter<WebContents>::emit("render-process-gone", payload);
    } else {
        emitRendererEvent(type, payload, request_id);
    }
}

gin_helper::WrapperInfo WebContents::kWrapperInfo = { gin_helper::GinEmbedder::kEmbedderNativeGin };
v8::Persistent<v8::Function> WebContents::s_constructor;

static void initializeWebContentApi(v8::Local<v8::Object> target,
    v8::Local<v8::Value>, v8::Local<v8::Context> context, const NodeNative*)
{
    WebContents::init(context->GetIsolate(), target, nullptr);
}

static const char WebContentsScript[] = "exports = {};";
static NodeNative nativeBrowserWebContentsNative {
    "WebContents", WebContentsScript, sizeof(WebContentsScript) - 1
};
NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_MANUAL(
    electron_browser_web_contents, initializeWebContentApi, &nativeBrowserWebContentsNative)

} // namespace atom

namespace gin_helper {
v8::Local<v8::Value> ConvertToV8(v8::Isolate* isolate, const atom::WebContents& contents)
{
    return const_cast<atom::WebContents&>(contents).GetWrapper(isolate);
}
} // namespace gin_helper
