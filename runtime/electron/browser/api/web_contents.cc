#include "runtime/electron/browser/api/web_contents.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "net/base/filename_util.h"
#include "runtime/electron/browser/api/app.h"
#include "runtime/electron/browser/api/session.h"
#include "runtime/electron/browser/api/window_interface.h"
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
#include "runtime/electron/browser/devtools_gateway.h"
#endif
#include "runtime/electron/common/api/native_image.h"
#include "runtime/electron/common/asar/asar_util.h"
#include "runtime/electron/common/gin_helper/arguments.h"
#include "runtime/electron/common/gin_helper/converter.h"
#include "runtime/electron/common/gin_helper/object_template_builder.h"
#include "runtime/electron/common/gin_helper/promise.h"
#include "runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "runtime/electron/common/gin_helper/wrappable.h"
#include "runtime/electron/common/id_live_detect.h"
#include "runtime/electron/common/node_register_help.h"
#include "runtime/electron/node_bindings.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace atom {
namespace {

std::unordered_map<int, WebContents*>& AllContents()
{
    static std::unordered_map<int, WebContents*> contents;
    return contents;
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
    unsigned flags, int delta)
{
    base::Value::Dict event;
    event.Set("type", type);
    event.Set("nativeMessage", static_cast<int>(message));
    event.Set("x", x);
    event.Set("y", y);
    event.Set("modifiers", static_cast<int>(flags));
    if (delta)
        event.Set("deltaY", delta);
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
    builder.SetMethod("_respond", &WebContents::respondApi);
    builder.SetMethod("_attachGuest", &WebContents::attachGuestApi);
    builder.SetMethod("_authorizeFileSelection", &WebContents::authorizeFileSelectionApi);
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
    builder.SetProperty("mainFrame", &WebContents::getMainFrameApi);

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
    return self;
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
    m_web_preferences.Set("contextIsolation", context_isolation);
    options.GetBydefaultVal("nodeIntegrationInSubFrames", false, &preload_all_frames);
    m_web_preferences.Set("webviewTag", webview_tag);
    m_web_preferences.Set("webSecurity", web_security);
    m_web_preferences.Set("allowRunningInsecureContent", allow_insecure);
    m_web_preferences.Set("nodeIntegrationInSubFrames", preload_all_frames);
    m_web_preferences.Set("sandbox", true);
    m_web_preferences.Set("nodeIntegration", false);
    if (App* app = App::getInstance())
        addApplicationResourceRoot(
            base::FilePath::FromUTF8Unsafe(app->getAppPath()));

    RendererLaunchOptions launch;
    launch.contents_id = m_id;
    launch.context_isolation = context_isolation;
    launch.webview_tag = webview_tag;
    launch.web_security = web_security;
    launch.allow_running_insecure_content = allow_insecure;
    launch.preload_all_frames = preload_all_frames;
    launch.transparent = transparent;
    launch.preload_process_metadata.Set("sandboxed", true);
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
        m_destroyed = true;
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

void WebContents::unregisterAndNotify()
{
    auto found = AllContents().find(m_id);
    if (found == AllContents().end())
        return;
    AllContents().erase(found);
    std::set<WebContentsObserver*> observers = m_observers;
    m_observers.clear();
    for (WebContentsObserver* observer : observers)
        observer->onWebContentsDeleted(this);
}

void WebContents::destroyed()
{
    if (!m_destroy_event_emitted) {
        m_destroy_event_emitted = true;
        mate::EventEmitter<WebContents>::emit("destroyed");
    }
    destroyApi();
}

void WebContents::destroyApi()
{
    if (m_destroyed)
        return;
    m_destroyed = true;
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
    return !isAlive();
}

void WebContents::addObserver(WebContentsObserver* observer)
{
    if (observer)
        m_observers.insert(observer);
}

void WebContents::removeObserver(WebContentsObserver* observer)
{
    m_observers.erase(observer);
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
    if (!observed_document && !approved_intrinsic) {
        if (!frame.committed_url.empty()
            && SameDocumentUrl(url, frame.committed_url)) {
            frame.committed_url = target.spec();
            return true;
        }
        return false;
    }
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
    m_file_chooser_frames.erase(chooser);
    std::string error;
    base::Value::List files =
        session->authorizeSelectedFiles(m_id, paths, &error);
    if (!error.empty()) {
        info.GetIsolate()->ThrowException(v8::Exception::Error(
            gin_helper::StringToV8(info.GetIsolate(), error)));
        return;
    }
    info.GetReturnValue().Set(
        gin_helper::ConvertToV8(info.GetIsolate(), files));
}

void WebContents::getMainFrameApi(const v8::FunctionCallbackInfo<v8::Value>& info) const
{
    // JavaScript augments this descriptor with send()/framesInSubtree. Keeping
    // the native identity data here avoids exposing renderer pointers.
    base::Value::Dict frame;
    frame.Set("processId", getProcessIdApi());
    frame.Set("routingId", m_id);
    frame.Set("detached", isDestroyedApi());
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
    m_isLoading = true;
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
    base::Value::Dict params;
    params.Set("width", width);
    params.Set("height", height);
    params.Set("scaleFactor", scale_factor);
    m_renderer->Send("setViewport", std::move(params));
}

void WebContents::setFocus(bool focused)
{
    m_focused = focused;
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

#if defined(_WIN32)
void WebContents::sendWindowsMouseEvent(unsigned message, int x, int y, unsigned flags, int delta)
{
    if (!isAlive())
        return;
    const char* type = "mouseMove";
    switch (message) {
    case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN: type = "mouseDown"; break;
    case WM_LBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONUP: type = "mouseUp"; break;
    case WM_MOUSEWHEEL: type = "mouseWheel"; break;
    case WM_CONTEXTMENU: type = "contextMenu"; break;
    default: break;
    }
    sendInput(InputFromWindows(type, message, x, y, flags, delta));
}
#endif

void WebContents::sendWindowsKeyEvent(const char* type, unsigned key_code, unsigned flags)
{
#if BUILDFLAG(IS_WIN)
    if (::GetKeyState(VK_CONTROL) & 0x8000)
        flags |= MINI_ELECTRON_CONTROL;
    if (::GetKeyState(VK_SHIFT) & 0x8000)
        flags |= MINI_ELECTRON_SHIFT;
#endif
    base::Value::Dict input;
    input.Set("type", type);
    input.Set("keyCode", static_cast<int>(key_code));
    input.Set("modifiers", static_cast<int>(flags));
    sendInput(std::move(input));
}

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
    int64_t frame_id = 0;
    bool all_frames = false;
    std::string channel;
    base::Value::List values;
    if (!args.GetNext(&frame_id) || !args.GetNext(&all_frames)
        || !args.GetNext(&channel) || !args.GetRemaining(&values)) {
        args.ThrowError();
        return false;
    }
    base::Value::Dict params;
    params.Set("frameId", static_cast<double>(frame_id));
    params.Set("allFrames", all_frames);
    params.Set("channel", channel);
    params.Set("args", std::move(values));
    return isAlive() && m_renderer->Send("ipc-message", std::move(params));
}

bool WebContents::sendCommandApi(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    std::string method;
    base::Value::Dict params;
    if (info.Length() < 1 || !gin_helper::ConvertFromV8(info.GetIsolate(), info[0], &method))
        return false;
    if (info.Length() > 1 && !gin_helper::ConvertFromV8(info.GetIsolate(), info[1], &params))
        return false;
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

bool WebContents::copyFrame(RendererFrameSnapshot* snapshot) const
{
    return snapshot && isAlive() && m_renderer->CopyFrame(snapshot);
}

#if defined(_WIN32)
bool WebContents::paintFrame(HDC target, int dest_x, int dest_y, int src_x, int src_y,
    int width, int height) const
{
    RendererFrameSnapshot frame;
    if (!target || !copyFrame(&frame) || frame.width <= 0 || frame.height <= 0
        || frame.stride < frame.width * 4)
        return false;
    src_x = std::max(0, src_x);
    src_y = std::max(0, src_y);
    width = std::min(width, frame.width - src_x);
    height = std::min(height, frame.height - src_y);
    if (width <= 0 || height <= 0)
        return false;

    // GDI's 32-bit BI_RGB byte order is BGRA. CopyFrame owns this snapshot, so
    // convert in place without another frame-sized allocation.
    for (int row = 0; row < frame.height; ++row) {
        uint8_t* pixel = frame.rgba.data() + static_cast<size_t>(row) * frame.stride;
        for (int column = 0; column < frame.width; ++column, pixel += 4)
            std::swap(pixel[0], pixel[2]);
    }

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = frame.width;
    info.bmiHeader.biHeight = -frame.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    return ::StretchDIBits(target, dest_x, dest_y, width, height,
               src_x, src_y, width, height, frame.rgba.data(), &info,
               DIB_RGB_COLORS, SRCCOPY)
        != GDI_ERROR;
}
#endif

void WebContents::capturePageApi(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    auto* promise = new gin_helper::Promise<v8::Local<v8::Object>>(info.GetIsolate());
    info.GetReturnValue().Set(promise->GetHandle());
    RendererFrameSnapshot frame;
    if (!copyFrame(&frame) || frame.width <= 0 || frame.height <= 0
        || frame.stride < frame.width * 4) {
        promise->Resolve(NativeImage::createEmpty(info.GetIsolate()));
        delete promise;
        return;
    }

    int x = 0;
    int y = 0;
    int width = frame.width;
    int height = frame.height;
    if (info.Length() > 0 && info[0]->IsObject()) {
        gin_helper::Dictionary rect(info.GetIsolate(), info[0].As<v8::Object>());
        rect.GetBydefaultVal("x", 0, &x);
        rect.GetBydefaultVal("y", 0, &y);
        rect.GetBydefaultVal("width", width, &width);
        rect.GetBydefaultVal("height", height, &height);
    }
    x = std::max(0, std::min(x, frame.width));
    y = std::max(0, std::min(y, frame.height));
    width = std::max(0, std::min(width, frame.width - x));
    height = std::max(0, std::min(height, frame.height - y));
    if (!width || !height) {
        promise->Resolve(NativeImage::createEmpty(info.GetIsolate()));
        delete promise;
        return;
    }

    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    for (int row = 0; row < height; ++row) {
        const uint8_t* source = frame.rgba.data() + static_cast<size_t>(y + row) * frame.stride + x * 4;
        std::memcpy(rgba.data() + static_cast<size_t>(row) * width * 4, source,
            static_cast<size_t>(width) * 4);
    }
    promise->Resolve(NativeImage::createFromRGBA(info.GetIsolate(), rgba.data(), width,
        height, width * 4));
    delete promise;
}

void WebContents::invalidateApi()
{
    if (isAlive())
        m_renderer->Send("invalidate", {});
    for (WebContentsObserver* observer : m_observers)
        observer->onWebContentsPaint(this);
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
    const std::string type = ReadString(event, "type");
    const base::Value::Dict& payload = PayloadFrom(event);
    const uint64_t request_id = RequestIdFrom(event);

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
            if (!commitRendererFrame(frame_id, ReadString(payload, "url")))
                return;
            const std::string committed_url =
                m_renderer_frames.at(frame_id).committed_url;
            if (*main_frame) {
                m_url = committed_url;
                mate::EventEmitter<WebContents>::emit("did-navigate", m_url);
                if (!isAlive())
                    return;
            }
            mate::EventEmitter<WebContents>::emit("did-frame-navigate",
                committed_url, 200, "", *main_frame, getProcessIdApi(), m_id);
        }
        return;
    }
    if (type == "frame-detached") {
        uint64_t frame_id = 0;
        if (ReadFrameIdentity(event, "frameId", &frame_id)) {
            m_renderer_frames.erase(frame_id);
            if (m_main_frame_id == frame_id)
                m_main_frame_id = 0;
        }
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
            context.approved_navigation_url = navigation_url;
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
        if (!prevented) {
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
        if (*main_frame)
            m_isLoading = true;
        mate::EventEmitter<WebContents>::emit("did-start-navigation", url,
            false, *main_frame, getProcessIdApi(), m_id);
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
        frame.navigation = {};
        frame.response_url.clear();
        if (main_frame) {
            m_isLoading = false;
            m_pending_main_navigation = {};
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
            for (WebContentsObserver* observer : m_observers)
                observer->onWebContentsDraggableRegions(this, *regions);
        }
    } else if (type == "paint") {
        for (WebContentsObserver* observer : m_observers)
            observer->onWebContentsPaint(this);
        if (!m_ready_to_show_emitted) {
            m_ready_to_show_emitted = true;
            for (WebContentsObserver* observer : m_observers)
                observer->onWebContentsReadyToShow(this);
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
