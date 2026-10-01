// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "runtime/engine/public/engine_api.h"

#include <cstdio>
#include <cmath>
#include <functional>
#include <memory>
#include <string>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "runtime/engine/browser/web_view_host.h"
#include "runtime/engine/common/live_id_detect.h"
#include "runtime/engine/common/thread_call.h"
#include "runtime/engine/renderer/render_thread_impl.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "runtime/engine/renderer/web_local_frame_client_impl.h"
#include "third_party/blink/public/web/web_view.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/web_local_frame_impl.h"
#include "ui/display/screen_info.h"


namespace blink {
LocalFrame* FromFrameTokenHash(const size_t& frame_token_hash);
}

namespace content {
void freeTempCharStrings() {}
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

extern "C" {

void MINI_ELECTRON_CALL_TYPE mini_electron_init(const mini_electron_settings* settings) {
  if (g_initialized)
    return;
  g_initialized = true;
  g_at_exit_manager = std::make_unique<base::AtExitManager>();
  if (!base::CommandLine::InitializedForCurrentProcess())
    base::CommandLine::Init(0, nullptr);
  std::fprintf(stderr, "[mini-electron] mini_electron_init -> RenderThreadImpl renderer thread/task loop\n");
  content::RenderThreadImpl::get()->initializeWebKit();
  std::fprintf(stderr, "[mini-electron] RenderThreadImpl -> blink::Initialize complete\n");
}

void MINI_ELECTRON_CALL_TYPE mini_electron_uninit() {
  g_initialized = false;
}

mini_electron_web_view MINI_ELECTRON_CALL_TYPE mini_electron_create_web_view() {
  auto* view = new content::WebViewHost(false);
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

BOOL MINI_ELECTRON_CALL_TYPE mini_electron_fire_mouse_wheel_event(mini_electron_web_view handle,
                                        int x,
                                        int y,
                                        int wheel_delta,
                                        unsigned int flags) {
  content::WebViewHost* view = ViewFor(handle);
  if (!view)
    return FALSE;
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle, [x, y, wheel_delta, flags](content::WebViewHost* view) {
        const float scale = view->getWebView()->MainFrameWidget()
                                ->GetOriginalScreenInfo().device_scale_factor;
        const WPARAM wparam = MouseFlags(flags) |
            (static_cast<WPARAM>(static_cast<uint16_t>(wheel_delta)) << 16);
        const LPARAM lparam =
            static_cast<LPARAM>(static_cast<uint16_t>(std::lround(x * scale))) |
            (static_cast<LPARAM>(static_cast<uint16_t>(std::lround(y * scale))) << 16);
        view->fireWheelEventOnUiThread(wparam, lparam);
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

void mini_electron_mac_set_device_scale_factor(mini_electron_web_view handle, float scale) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      FROM_HERE, handle, [scale](content::WebViewHost* view) {
        view->setDeviceScaleFactor(scale);
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

}  // extern "C"

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
                               int wheel_delta,
                               unsigned int flags) {
  return mini_electron_fire_mouse_wheel_event(handle, x, y, wheel_delta, flags);
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
