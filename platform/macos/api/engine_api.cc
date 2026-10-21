// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "runtime/engine/public/engine_api.h"
#include "runtime/engine/api/internal_api.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/task/single_thread_task_runner.h"
#include "runtime/engine/bindings/js_value.h"
#include "runtime/engine/browser/web_view_host.h"
#include "runtime/engine/common/live_id_detect.h"
#include "runtime/engine/common/thread_call.h"
#include "runtime/engine/renderer/render_thread_impl.h"
#include "runtime/engine/renderer/renderer_blink_platform_impl.h"
#include "runtime/network/loader/web_url_loader_internal.h"
#include "runtime/network/loader/web_url_loader_manager.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/web/web_css_origin.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "runtime/engine/renderer/web_local_frame_client_impl.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/renderer/bindings/core/v8/to_v8_traits.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_blob.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_element.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_file.h"
#include "third_party/blink/renderer/core/fileapi/file.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "third_party/blink/renderer/platform/bindings/script_state.h"
#include "ui/display/screen_info.h"


namespace blink {
LocalFrame* FromFrameTokenHash(const size_t& frame_token_hash);
}

namespace {
// Strings returned through the C API stay valid until the owning WebViewHost
// finishes its current Blink task and calls content::freeTempCharStrings().
std::vector<std::unique_ptr<std::string>>& TemporaryStrings() {
  static std::vector<std::unique_ptr<std::string>> strings;
  return strings;
}
}  // namespace

namespace content {
void freeTempCharStrings() {
  TemporaryStrings().clear();
}
}  // namespace content

bool checkThreadCallIsValid(const char*) {
  return true;
}

const char* createTempCharString(const char* str, size_t length) {
  if (!str || !length)
    return "";
  TemporaryStrings().push_back(std::make_unique<std::string>(str, length));
  return TemporaryStrings().back()->c_str();
}
namespace content {
void runJsOnBlinkThread(mini_electron_web_view webview_handle,
                        mini_electron_web_frame_handle frame_id,
                        int world_id,
                        std::string* script,
                        BOOL is_in_closure,
                        mini_electron_run_js_callback callback,
                        void* parameter);
}

namespace mini_electron {
void onNetSetData(mini_electron_net_job job, void* buffer, int length);
void onNetSetMIMEType(mini_electron_net_job job, const char* type);
void onNetSetHTTPHeaderFieldCommon(mini_electron_net_job job,
                                   const utf8* key,
                                   const utf8* value,
                                   BOOL response);
}
namespace {

std::unique_ptr<base::AtExitManager> g_at_exit_manager;
bool g_initialized = false;

content::WebViewHost* ViewFor(mini_electron_web_view handle) {
  return static_cast<content::WebViewHost*>(
      common::LiveIdDetect::getWebViewIds()->getPtr(
          static_cast<int64_t>(handle)));
}

WPARAM MouseFlags(unsigned int flags) {
  constexpr WPARAM kLeft = 0x0001;
  constexpr WPARAM kRight = 0x0002;
  constexpr WPARAM kShift = 0x0004;
  constexpr WPARAM kControl = 0x0008;
  constexpr WPARAM kMiddle = 0x0010;
  WPARAM result = 0;
  if (flags & MINI_ELECTRON_LBUTTON)
    result |= kLeft;
  if (flags & MINI_ELECTRON_RBUTTON)
    result |= kRight;
  if (flags & MINI_ELECTRON_SHIFT)
    result |= kShift;
  if (flags & MINI_ELECTRON_CONTROL)
    result |= kControl;
  if (flags & MINI_ELECTRON_MBUTTON)
    result |= kMiddle;
  return result;
}

}  // namespace

mini_electron_web_view mini_electron_create_web_view_with_background(
    unsigned background_color, bool transparent) {
  auto* view = new content::WebViewHost(false, background_color, transparent);
  const mini_electron_web_view handle = view->getWebviewHandle();
  std::fprintf(stderr, "[mini-electron] mini_electron_create_web_view(%lld) -> Blink WebView task\n",
               static_cast<long long>(handle));
  content::ThreadCall::callBlinkThreadAsync(
      FROM_HERE, [view] {
        view->createWebWindowOrViewInBlinkThread(nullptr, nullptr, false,
                                                 view->isTransparent());
        std::fprintf(stderr,
                     "[mini-electron] WebViewHost -> WebViewImpl/LocalFrame/Page created\n");
      });
  return handle;
}

extern "C" {

void MINI_ELECTRON_CALL_TYPE mini_electron_init(const mini_electron_settings* settings) {
  if (g_initialized)
    return;
  g_initialized = true;
  if (!base::SingleThreadTaskRunner::HasCurrentDefault())
    g_at_exit_manager = std::make_unique<base::AtExitManager>();
  if (!base::CommandLine::InitializedForCurrentProcess())
    base::CommandLine::Init(0, nullptr);
  std::fprintf(stderr, "[mini-electron] mini_electron_init -> RenderThreadImpl renderer thread/task loop\n");
  content::RenderThreadImpl::get()->initializeWebKit();
  std::fprintf(stderr, "[mini-electron] RenderThreadImpl -> blink::Initialize complete\n");
}

void MINI_ELECTRON_CALL_TYPE mini_electron_uninit() {
  // Blink is process-wide. RenderThreadImpl::shutdown() owns final teardown.
}

mini_electron_web_view MINI_ELECTRON_CALL_TYPE mini_electron_create_web_view() {
  return mini_electron_create_web_view_with_background(0xffffffff, false);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_destroy_web_view(mini_electron_web_view handle) {
  content::WebViewHost* view = ViewFor(handle);
  if (!view || !view->preDestroyOnUiThread())
    return;
  content::ThreadCall::callBlinkThreadAsync(
      FROM_HERE, [view] { view->preDestroyOnBlinkThread(); });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_load_url(mini_electron_web_view handle, const utf8* url) {
  if (!url)
    return;
  auto request_url = std::make_shared<std::string>(url);
  std::fprintf(stderr, "[mini-electron] mini_electron_load_url(%s) -> network/Blink loader\n",
               request_url->c_str());
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle,
      [request_url](content::WebViewHost* view) {
        view->resetState();
        view->loadUrl(request_url->c_str());
        std::fprintf(stderr,
                     "[mini-electron] WebViewHost::loadUrl -> WebLocalFrame/DocumentLoader dispatched\n");
      });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_resize(mini_electron_web_view handle, int width, int height) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle, [width, height](content::WebViewHost* view) {
        view->onResize(width, height, false);
      });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_paint_bit_updated(mini_electron_web_view handle,
                                      mini_electron_paint_bit_updated_callback callback,
                                      void* parameter) {
  if (content::WebViewHost* view = ViewFor(handle))
    view->getClosure().setPaintBitUpdatedCallback(callback, parameter);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_loading_finish(mini_electron_web_view handle,
                                    mini_electron_loading_finish_callback callback,
                                    void* parameter) {
  if (content::WebViewHost* view = ViewFor(handle))
    view->getClosure().setLoadingFinishCallback(callback, parameter);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_load_url_begin(mini_electron_web_view handle,
                                   mini_electron_load_url_begin_callback callback,
                                   void* parameter) {
  if (content::WebViewHost* view = ViewFor(handle))
    view->getClosure().setLoadUrlBeginCallback(callback, parameter);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_did_create_script_context(
    mini_electron_web_view handle,
    mini_electron_did_create_script_context_callback callback,
    void* parameter) {
  if (content::WebViewHost* view = ViewFor(handle))
    view->getClosure().setDidCreateScriptContextCallback(callback, parameter);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_js_query(mini_electron_web_view handle,
                              mini_electron_js_query_callback callback,
                              void* parameter) {
  content::WebViewHost* view = ViewFor(handle);
  if (!view)
    return;
  auto* closure = new std::function<void(mini_electron_js_exec_state, int64_t, int,
                                          const utf8*)>(
      [handle, callback, parameter](mini_electron_js_exec_state state, int64_t query_id,
                                    int custom_message,
                                    const utf8* request) {
        callback(handle, parameter, state, query_id, custom_message, request);
      });
  view->getClosure().setJsQueryClosure(closure);
}

mini_electron_web_frame_handle MINI_ELECTRON_CALL_TYPE mini_electron_web_frame_get_main_frame(mini_electron_web_view handle) {
  content::WebViewHost* view = ViewFor(handle);
  if (!view)
    return nullptr;
  auto* frame = static_cast<blink::WebLocalFrame*>(view->getMainFrame());
  return reinterpret_cast<mini_electron_web_frame_handle>(
      content::getFrameIdByWebLocalFrame(frame));
}

void MINI_ELECTRON_CALL_TYPE mini_electron_run_js(mini_electron_web_view handle,
                          mini_electron_web_frame_handle frame,
                          const utf8* script,
                          BOOL is_in_closure,
                          mini_electron_run_js_callback callback,
                          void* parameter,
                          void* world_id) {
  auto* source = new std::string(script ? script : "");
  auto run = [handle, frame, source, is_in_closure, callback, parameter,
              world_id] {
    content::runJsOnBlinkThread(handle, frame,
                                static_cast<int>(reinterpret_cast<intptr_t>(
                                    world_id)),
                                source, is_in_closure, callback, parameter);
  };
  if (content::ThreadCall::isBlinkThread())
    run();
  else
    content::ThreadCall::callBlinkThreadAsync(FROM_HERE, std::move(run));
}

void MINI_ELECTRON_CALL_TYPE mini_electron_response_query(mini_electron_web_view handle,
                                  int64_t query_id,
                                  int custom_message,
                                  const utf8* response) {
  auto* response_copy = new std::string(response ? response : "");
  content::ThreadCall::callBlinkThreadAsync(
      FROM_HERE,
      [handle, query_id, custom_message, response_copy] {
        auto* id_info =
            reinterpret_cast<std::pair<mini_electron_web_frame_handle, int>*>(query_id);
        content::WebViewHost* view = ViewFor(handle);
        do {
          if (!view || !id_info)
            break;
          blink::LocalFrame* core_frame =
              blink::FromFrameTokenHash(
                  reinterpret_cast<size_t>(id_info->first));
          if (!core_frame)
            break;
          v8::Isolate* isolate = v8::Isolate::GetCurrent();
          v8::HandleScope handle_scope(isolate);
          blink::WebLocalFrame* frame =
              blink::WebLocalFrameImpl::FromFrame(core_frame);
          if (!frame)
            break;
          v8::Local<v8::Context> context = frame->MainWorldScriptContext();
          v8::MicrotasksScope microtasks_scope(
              context, v8::MicrotasksScope::kDoNotRunMicrotasks);
          v8::Context::Scope context_scope(context);
          v8::Local<v8::Object> global = context->Global();

          v8::Local<v8::Value> callback_value;
          if (!global
                   ->Get(context,
                         v8::String::NewFromUtf8Literal(isolate,
                                                       "__onMbQuery__"))
                   .ToLocal(&callback_value) ||
              !callback_value->IsFunction()) {
            break;
          }
          v8::Local<v8::Value> arguments[] = {
              v8::Integer::New(isolate, id_info->second),
              v8::Integer::New(isolate, custom_message),
              v8::String::NewFromUtf8(
                  isolate, response_copy->c_str(),
                  v8::NewStringType::kNormal,
                  static_cast<int>(response_copy->size()))
                  .ToLocalChecked(),
          };
          callback_value.As<v8::Function>()
              ->Call(context, v8::Undefined(isolate), std::size(arguments),
                     arguments)
              .ToLocalChecked();
        } while (false);
        delete id_info;
        delete response_copy;
      });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_net_set_mime_type(mini_electron_net_job job, const char* type) {
  mini_electron::onNetSetMIMEType(job, type);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_net_set_data(mini_electron_net_job job, void* buffer, int length) {
  mini_electron::onNetSetData(job, buffer, length);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_net_set_http_header_field_utf8(mini_electron_net_job job,
                                              const utf8* key,
                                              const utf8* value,
                                              BOOL response) {
  mini_electron::onNetSetHTTPHeaderFieldCommon(job, key, value, response);
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_fire_mouse_event(mini_electron_web_view handle,
                                   unsigned int message,
                                   int x,
                                   int y,
                                   unsigned int flags) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle, [message, x, y, flags](content::WebViewHost* view) {
        auto* widget = view->getWebView()->MainFrameWidget();
        const float scale = widget->GetOriginalScreenInfo().device_scale_factor;
        // AppKit reports DIP coordinates; Blink input uses physical pixels.
        view->onMouseMessage(message, std::lround(x * scale),
                             std::lround(y * scale), flags);
      });
  return TRUE;
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_fire_mouse_wheel_event(
    mini_electron_web_view handle, int x, int y, int delta_x, int delta_y,
    unsigned int flags) {
  content::WebViewHost* view = ViewFor(handle);
  if (!view)
    return FALSE;
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle,
      [x, y, delta_x, delta_y, flags](content::WebViewHost* view) {
        const float scale = view->getWebView()->MainFrameWidget()
                                ->GetOriginalScreenInfo().device_scale_factor;
        view->onWheelEvent(std::lround(x * scale), std::lround(y * scale),
                           delta_x, delta_y, flags);
      });
  return TRUE;
}

BOOL mini_electron_fire_precise_wheel_event(mini_electron_web_view handle,
                                           int x, int y,
                                           float delta_x, float delta_y,
                                           unsigned int flags) {
  if (!ViewFor(handle))
    return FALSE;
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle, [x, y, delta_x, delta_y, flags](content::WebViewHost* view) {
        const float scale = view->getWebView()->MainFrameWidget()
                                ->GetOriginalScreenInfo().device_scale_factor;
        const int modifiers =
            (flags & MINI_ELECTRON_SHIFT ? blink::WebInputEvent::kShiftKey : 0) |
            (flags & MINI_ELECTRON_CONTROL ? blink::WebInputEvent::kControlKey : 0) |
            (flags & MINI_ELECTRON_META ? blink::WebInputEvent::kMetaKey : 0) |
            (flags & MINI_ELECTRON_ALT ? blink::WebInputEvent::kAltKey : 0) |
            (flags & MINI_ELECTRON_LBUTTON ? blink::WebInputEvent::kLeftButtonDown : 0) |
            (flags & MINI_ELECTRON_RBUTTON ? blink::WebInputEvent::kRightButtonDown : 0) |
            (flags & MINI_ELECTRON_MBUTTON ? blink::WebInputEvent::kMiddleButtonDown : 0);
        view->onPreciseWheelEvent(std::lround(x * scale), std::lround(y * scale),
                                 delta_x * scale, delta_y * scale, modifiers);
      });
  return TRUE;
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_fire_key_down_event(mini_electron_web_view handle,
                                     unsigned int virtual_key,
                                     unsigned int flags,
                                     BOOL system_key) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle,
      [virtual_key, flags, system_key](content::WebViewHost* view) {
        view->onKeyDown(virtual_key, flags, system_key);
      });
  return TRUE;
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_fire_key_up_event(mini_electron_web_view handle,
                                   unsigned int virtual_key,
                                   unsigned int flags,
                                   BOOL system_key) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle,
      [virtual_key, flags, system_key](content::WebViewHost* view) {
        view->onKeyUp(virtual_key, flags, system_key);
      });
  return TRUE;
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_fire_key_press_event(mini_electron_web_view handle,
                                      unsigned int character,
                                      unsigned int flags,
                                      BOOL system_key) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle,
      [character, flags, system_key](content::WebViewHost* view) {
        view->onKeyPress(character, flags, system_key);
      });
  return TRUE;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_get_caret_rect(mini_electron_web_view handle, mini_electron_rect* rect) {
  if (!rect)
    return;
  if (content::WebViewHost* view = ViewFor(handle)) {
    const gfx::Point caret = view->getCaretPos();
    rect->x = caret.x();
    rect->y = caret.y();
    rect->w = 1;
    rect->h = 20;
  }
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_focus(mini_electron_web_view handle) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle,
      [](content::WebViewHost* view) { view->setFocus(); });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_kill_focus(mini_electron_web_view handle) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle,
      [](content::WebViewHost* view) { view->killFocus(); });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_execute_edit_command(
    mini_electron_web_view handle, const char* command) {
  if (!command || !*command)
    return;
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle,
      [command = std::string(command)](content::WebViewHost* view) {
        auto* web_view = view->getWebView();
        auto* frame = web_view ? web_view->FocusedFrame() : nullptr;
        if (frame)
          frame->ExecuteCommand(blink::WebString::FromUTF8(command));
      });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_editor_select_all(mini_electron_web_view view) {
  mini_electron_execute_edit_command(view, "SelectAll");
}
void MINI_ELECTRON_CALL_TYPE mini_electron_editor_copy(mini_electron_web_view view) {
  mini_electron_execute_edit_command(view, "Copy");
}
void MINI_ELECTRON_CALL_TYPE mini_electron_editor_cut(mini_electron_web_view view) {
  mini_electron_execute_edit_command(view, "Cut");
}
void MINI_ELECTRON_CALL_TYPE mini_electron_editor_paste(mini_electron_web_view view) {
  mini_electron_execute_edit_command(view, "Paste");
}
void MINI_ELECTRON_CALL_TYPE mini_electron_editor_delete(mini_electron_web_view view) {
  mini_electron_execute_edit_command(view, "Delete");
}
void MINI_ELECTRON_CALL_TYPE mini_electron_editor_undo(mini_electron_web_view view) {
  mini_electron_execute_edit_command(view, "Undo");
}
void MINI_ELECTRON_CALL_TYPE mini_electron_editor_redo(mini_electron_web_view view) {
  mini_electron_execute_edit_command(view, "Redo");
}
void MINI_ELECTRON_CALL_TYPE mini_electron_editor_un_select(mini_electron_web_view view) {
  mini_electron_execute_edit_command(view, "Unselect");
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_device_scale_factor(mini_electron_web_view handle, float scale) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle, [scale](content::WebViewHost* view) {
        view->setDeviceScaleFactor(scale);
      });
}

void mini_electron_mac_set_vsync_display_id(
    mini_electron_web_view handle, int64_t display_id) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle, [display_id](content::WebViewHost* view) {
        view->setVSyncDisplayId(display_id);
      });
}

void mini_electron_mac_set_composition(mini_electron_web_view handle,
                         const char16_t* text,
                         size_t length,
                         BOOL committed) {
  std::u16string composition(text ? text : u"", length);
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle,
      [composition = std::move(composition), committed](
          content::WebViewHost* view) {
        view->onImeText(composition, committed);
      });
}
void mini_electron_mac_set_storage_paths(mini_electron_web_view handle,
                          const char* cookie_path,
                          const char* local_storage_path) {
  std::string cookie(cookie_path ? cookie_path : "");
  std::string local_storage(local_storage_path ? local_storage_path : "");
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle,
      [cookie = std::move(cookie),
       local_storage = std::move(local_storage)](content::WebViewHost* view) {
        view->setCookieJarFullPath(cookie.c_str());
        view->setLocalStorageFullPath(local_storage.c_str());
      });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_call_blink_thread_async(
    mini_electron_thread_callback callback, void* param1, void* param2) {
  content::ThreadCall::callBlinkThreadAsync(
      FROM_HERE, [callback, param1, param2] { callback(param1, param2); });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_call_blink_thread_sync(
    mini_electron_thread_callback callback, void* param1, void* param2) {
  content::ThreadCall::callBlinkThreadSync(
      FROM_HERE, [callback, param1, param2] { callback(param1, param2); });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_wake(mini_electron_web_view) {
  content::ThreadCall::wake();
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_transparent(
    mini_electron_web_view handle, BOOL transparent) {
  if (content::WebViewHost* view = ViewFor(handle))
    view->setIsTransparent(!!transparent);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_auto_draw_to_hwnd(
    mini_electron_web_view handle, BOOL enabled) {
  if (content::WebViewHost* view = ViewFor(handle))
    view->setAutoDrawToHwnd(!!enabled);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_user_agent(
    mini_electron_web_view, const utf8* user_agent) {
  if (!user_agent)
    return;
  std::string value(user_agent);
  content::ThreadCall::callBlinkThreadAsync(
      FROM_HERE, [value = std::move(value)] {
        static_cast<content::RendererBlinkPlatformImpl*>(
            blink::Platform::Current())
            ->setUserAgent(value);
      });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_set_zoom_factor(
    mini_electron_web_view handle, float factor) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle,
      [factor](content::WebViewHost* view) { view->setZoomFactor(factor); });
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_alert_box(
    mini_electron_web_view handle,
    mini_electron_alert_box_callback callback,
    void* parameter) {
  if (content::WebViewHost* view = ViewFor(handle))
    view->getClosure().setAlertBoxCallback(callback, parameter);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_confirm_box(
    mini_electron_web_view handle,
    mini_electron_confirm_box_callback callback,
    void* parameter) {
  if (content::WebViewHost* view = ViewFor(handle))
    view->getClosure().setConfirmBoxCallback(callback, parameter);
}

void MINI_ELECTRON_CALL_TYPE mini_electron_on_prompt_box(
    mini_electron_web_view handle,
    mini_electron_prompt_box_callback callback,
    void* parameter) {
  if (content::WebViewHost* view = ViewFor(handle))
    view->getClosure().setPromptBoxCallback(callback, parameter);
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_is_main_frame(
    mini_electron_web_view handle, mini_electron_web_frame_handle frame_id) {
  if (!ViewFor(handle))
    return FALSE;
  if (frame_id == reinterpret_cast<mini_electron_web_frame_handle>(-2))
    return TRUE;
  blink::LocalFrame* core_frame =
      blink::FromFrameTokenHash(reinterpret_cast<size_t>(frame_id));
  blink::WebLocalFrameImpl* frame =
      core_frame ? blink::WebLocalFrameImpl::FromFrame(core_frame) : nullptr;
  return frame && frame->View()->MainFrame() == frame;
}

uint64_t MINI_ELECTRON_CALL_TYPE mini_electron_get_parent_frame_id(
    mini_electron_web_view handle, mini_electron_web_frame_handle frame_id) {
  if (!ViewFor(handle) ||
      frame_id == reinterpret_cast<mini_electron_web_frame_handle>(-2)) {
    return 0;
  }
  blink::LocalFrame* core_frame =
      blink::FromFrameTokenHash(reinterpret_cast<size_t>(frame_id));
  blink::WebLocalFrameImpl* frame =
      core_frame ? blink::WebLocalFrameImpl::FromFrame(core_frame) : nullptr;
  blink::WebFrame* parent = frame ? frame->Parent() : nullptr;
  if (!parent || !parent->IsWebLocalFrame())
    return 0;
  return static_cast<uint64_t>(
      content::getFrameIdByWebLocalFrame(parent->ToWebLocalFrame()));
}

void MINI_ELECTRON_CALL_TYPE mini_electron_insert_css_by_frame_with_result(
    mini_electron_web_view handle,
    mini_electron_web_frame_handle frame_id,
    const utf8* css_text,
    int css_origin,
    mini_electron_insert_css_by_frame_result_callback callback,
    void* parameter) {
  std::string css(css_text ? css_text : "");
  content::ThreadCall::callBlinkThreadAsync(
      FROM_HERE, [handle, frame_id, css = std::move(css), css_origin, callback,
                  parameter] {
        blink::WebLocalFrame* frame = nullptr;
        if (content::WebViewHost* view = ViewFor(handle)) {
          if (frame_id == reinterpret_cast<mini_electron_web_frame_handle>(-2)) {
            if (content::WebLocalFrameClientImpl* client = view->getFrameClient())
              frame = client->getFrame();
          } else if (blink::LocalFrame* core_frame = blink::FromFrameTokenHash(
                         reinterpret_cast<size_t>(frame_id))) {
            frame = blink::WebLocalFrameImpl::FromFrame(core_frame);
          }
        }
        std::string key;
        if (frame) {
          key = frame->GetDocument()
                    .InsertStyleSheet(blink::WebString::FromUTF8(css), nullptr,
                                      css_origin == 0
                                          ? blink::WebCssOrigin::kAuthor
                                          : blink::WebCssOrigin::kUser)
                    .Utf8();
        }
        content::ThreadCall::callUiThreadAsync(
            FROM_HERE, [handle, key = std::move(key), callback, parameter] {
              callback(handle, parameter, key.c_str());
            });
      });
}

mini_electron_js_type MINI_ELECTRON_CALL_TYPE mini_electron_get_js_value_type(
    mini_electron_js_exec_state, mini_electron_js_value value) {
  auto* bridge = static_cast<mini_electron::engine::JsValueBridge*>(
      common::LiveIdDetect::get()->getPtrLocked(value));
  if (!bridge)
    return kMiniElectronJsTypeUndefined;
  const mini_electron_js_type type = bridge->getType();
  common::LiveIdDetect::get()->unlock(value, bridge);
  return type;
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_js_to_boolean(
    mini_electron_js_exec_state, mini_electron_js_value value) {
  auto* bridge = static_cast<mini_electron::engine::JsValueBridge*>(
      common::LiveIdDetect::get()->getPtrLocked(value));
  if (!bridge)
    return FALSE;
  const BOOL result = bridge->getType() == kMiniElectronJsTypeBool
      ? bridge->getBoolVal()
      : FALSE;
  common::LiveIdDetect::get()->unlock(value, bridge);
  return result;
}

double MINI_ELECTRON_CALL_TYPE mini_electron_js_to_double(
    mini_electron_js_exec_state, mini_electron_js_value value) {
  auto* bridge = static_cast<mini_electron::engine::JsValueBridge*>(
      common::LiveIdDetect::get()->getPtrLocked(value));
  if (!bridge)
    return 0;
  const double result = bridge->getType() == kMiniElectronJsTypeNumber
      ? bridge->getDoubleVal()
      : 0;
  common::LiveIdDetect::get()->unlock(value, bridge);
  return result;
}

const utf8* MINI_ELECTRON_CALL_TYPE mini_electron_js_to_string(
    mini_electron_js_exec_state, mini_electron_js_value value) {
  auto* bridge = static_cast<mini_electron::engine::JsValueBridge*>(
      common::LiveIdDetect::get()->getPtrLocked(value));
  if (!bridge)
    return "";
  std::string result;
  if (bridge->getType() == kMiniElectronJsTypeString)
    result = bridge->getStrVal();
  common::LiveIdDetect::get()->unlock(value, bridge);
  return createTempCharString(result.c_str(), result.size());
}

void MINI_ELECTRON_CALL_TYPE mini_electron_net_cancel_request(
    mini_electron_net_job job) {
  const int job_id = static_cast<mini_electron::WebURLLoaderInternal*>(job)->m_id;
  auto cancel = [job_id] {
    mini_electron::WebURLLoaderManager* manager =
        mini_electron::WebURLLoaderManager::sharedInstance();
    mini_electron::AutoLockJob lock(manager, job_id);
    mini_electron::WebURLLoaderInternal* locked = lock.lock();
    if (!locked || locked->isCancelled() || locked->m_isEmbedderCanceled)
      return;
    locked->m_isEmbedderCanceled = true;
    if (locked->m_isHoldJobToAsynCommit && !locked->m_isUrlBegining)
      manager->continueJob(locked);
  };
  if (content::ThreadCall::isBlinkThread())
    cancel();
  else
    content::ThreadCall::callBlinkThreadAsync(FROM_HERE, std::move(cancel));
}

const char* MINI_ELECTRON_CALL_TYPE mini_electron_net_get_referrer(
    mini_electron_net_job) {
  return "";
}

mini_electron_mem_buf* MINI_ELECTRON_CALL_TYPE mini_electron_create_mem_buf(
    mini_electron_web_view, void* buffer, size_t length) {
  if (!buffer || !length)
    return nullptr;
  auto* result = static_cast<mini_electron_mem_buf*>(
      std::malloc(sizeof(mini_electron_mem_buf)));
  result->unuse = sizeof(mini_electron_mem_buf);
  result->length = length;
  result->data = std::malloc(length);
  std::memcpy(result->data, buffer, length);
  return result;
}

void MINI_ELECTRON_CALL_TYPE mini_electron_free_mem_buf(
    mini_electron_mem_buf* buffer) {
  if (!buffer)
    return;
  std::free(buffer->data);
  std::free(buffer);
}

}  // extern "C"

// context_bridge.cc declares these two helpers with C++ linkage.
BOOL MINI_ELECTRON_CALL_TYPE mini_electron_get_context_by_v8_object(
    void* isolate, void* object, int world_id, v8ContextPtr context_out) {
  v8::Local<v8::Object>* value = static_cast<v8::Local<v8::Object>*>(object);
  v8::Local<v8::Context> creation_context;
  if (!(*value)->GetCreationContext().ToLocal(&creation_context))
    return FALSE;
  blink::WebLocalFrame* frame =
      blink::WebLocalFrame::FrameForContext(creation_context);
  if (!frame)
    return FALSE;
  *static_cast<v8::Local<v8::Context>*>(context_out) = world_id == 0
      ? frame->MainWorldScriptContext()
      : frame->GetScriptContextFromWorldId(static_cast<v8::Isolate*>(isolate),
                                           world_id);
  return TRUE;
}

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_pass_dom_value_to_other_context(
    void* value_pointer, void* destination_pointer, void* out_pointer) {
  v8::Local<v8::Value> value =
      *static_cast<v8::Local<v8::Value>*>(value_pointer);
  v8::Local<v8::Context> destination =
      *static_cast<v8::Local<v8::Context>*>(destination_pointer);
  v8::Isolate* isolate = destination->GetIsolate();
  blink::Blob* blob = nullptr;
  if (blink::V8File::HasInstance(isolate, value))
    blob = blink::V8File::ToWrappable(isolate, value);
  else if (blink::V8Blob::HasInstance(isolate, value))
    blob = blink::V8Blob::ToWrappable(isolate, value);
  v8::Context::Scope destination_scope(destination);
  if (blob) {
    *static_cast<v8::Local<v8::Value>*>(out_pointer) =
        blob->ToV8(isolate, destination->Global());
    return TRUE;
  }
  blink::Element* element = blink::V8Element::ToWrappable(isolate, value);
  if (!element)
    return FALSE;
  *static_cast<v8::Local<v8::Value>*>(out_pointer) =
      blink::ToV8Traits<blink::Element>::ToV8(
          blink::ScriptState::From(isolate, destination), element);
  return TRUE;
}


void mini_electron_show_window_impl(mini_electron_web_view handle, BOOL show) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle,
      [show](content::WebViewHost* view) { view->setShow(show); });
}

void mini_electron_destroy_web_view_impl(mini_electron_web_view handle) {
  mini_electron_destroy_web_view(handle);
}

BOOL mini_electron_fire_mouse_event_impl(mini_electron_web_view handle,
                          unsigned int message,
                          int x,
                          int y,
                          unsigned int flags) {
  return mini_electron_fire_mouse_event(handle, message, x, y, flags);
}

BOOL mini_electron_fire_key_press_event_impl(mini_electron_web_view handle,
                             unsigned int character,
                             unsigned int flags,
                             BOOL system_key) {
  return mini_electron_fire_key_press_event(handle, character, flags, system_key);
}

BOOL mini_electron_fire_key_up_event_impl(mini_electron_web_view handle,
                          unsigned int virtual_key,
                          unsigned int flags,
                          BOOL system_key) {
  return mini_electron_fire_key_up_event(handle, virtual_key, flags, system_key);
}

BOOL mini_electron_fire_key_down_event_impl(mini_electron_web_view handle,
                            unsigned int virtual_key,
                            unsigned int flags,
                            BOOL system_key) {
  return mini_electron_fire_key_down_event(handle, virtual_key, flags, system_key);
}

BOOL mini_electron_fire_mouse_wheel_event_impl(mini_electron_web_view handle,
                               int x,
                               int y,
                               int delta_x,
                               int delta_y,
                               unsigned int flags) {
  return mini_electron_fire_mouse_wheel_event(
      handle, x, y, delta_x, delta_y, flags);
}

BOOL mini_electron_fire_windows_message_impl(mini_electron_web_view,
                              HWND,
                              UINT,
                              WPARAM,
                              LPARAM,
                              LRESULT* result) {
  if (result)
    *result = 0;
  return FALSE;
}
