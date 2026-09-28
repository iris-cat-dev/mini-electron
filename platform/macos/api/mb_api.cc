// Copyright 2026 The miniblink132 Authors
// Use of this source code is governed by the Apache-2.0 license.

#include "mbvip/core/mb.h"

#include <cstdio>
#include <cmath>
#include <functional>
#include <memory>
#include <string>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "content/browser/MbWebview.h"
#include "content/common/LiveIdDetect.h"
#include "content/common/ThreadCall.h"
#include "content/renderer/RenderThreadImpl.h"
#include "third_party/blink/public/web/web_frame_widget.h"
#include "content/renderer/WebLocalFrameClientImpl.h"
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
void runJsOnBlinkThread(mbWebView webview_handle,
                        mbWebFrameHandle frame_id,
                        int world_id,
                        std::string* script,
                        BOOL is_in_closure,
                        mbRunJsCallback callback,
                        void* parameter);
}

namespace mbnet {
void onNetSetData(mbNetJob job, void* buffer, int length);
void onNetSetMIMEType(mbNetJob job, const char* type);
void onNetSetHTTPHeaderFieldCommon(mbNetJob job,
                                   const utf8* key,
                                   const utf8* value,
                                   BOOL response);
}
namespace {

std::unique_ptr<base::AtExitManager> g_at_exit_manager;
bool g_initialized = false;

content::MbWebView* ViewFor(mbWebView handle) {
  return static_cast<content::MbWebView*>(
      common::LiveIdDetect::getMbWebviewIds()->getPtr(
          static_cast<int64_t>(handle)));
}

WPARAM MouseFlags(unsigned int flags) {
  constexpr WPARAM kLeft = 0x0001;
  constexpr WPARAM kRight = 0x0002;
  constexpr WPARAM kShift = 0x0004;
  constexpr WPARAM kControl = 0x0008;
  constexpr WPARAM kMiddle = 0x0010;
  WPARAM result = 0;
  if (flags & MB_LBUTTON)
    result |= kLeft;
  if (flags & MB_RBUTTON)
    result |= kRight;
  if (flags & MB_SHIFT)
    result |= kShift;
  if (flags & MB_CONTROL)
    result |= kControl;
  if (flags & MB_MBUTTON)
    result |= kMiddle;
  return result;
}

}  // namespace

extern "C" {

void MB_CALL_TYPE mbInit(const mbSettings* settings) {
  if (g_initialized)
    return;
  g_initialized = true;
  g_at_exit_manager = std::make_unique<base::AtExitManager>();
  if (!base::CommandLine::InitializedForCurrentProcess())
    base::CommandLine::Init(0, nullptr);
  std::fprintf(stderr, "[mb] mbInit -> RenderThreadImpl renderer thread/task loop\n");
  content::RenderThreadImpl::get()->initializeWebKit();
  std::fprintf(stderr, "[mb] RenderThreadImpl -> blink::Initialize complete\n");
}

void MB_CALL_TYPE mbUninit() {
  g_initialized = false;
}

mbWebView MB_CALL_TYPE mbCreateWebView() {
  auto* view = new content::MbWebView(false);
  const mbWebView handle = view->getWebviewHandle();
  std::fprintf(stderr, "[mb] mbCreateWebView(%lld) -> Blink WebView task\n",
               static_cast<long long>(handle));
  content::ThreadCall::callBlinkThreadAsync(
      MB_FROM_HERE, [view] {
        view->createWebWindowOrViewInBlinkThread(nullptr, nullptr, false,
                                                 view->isTransparent());
        std::fprintf(stderr,
                     "[mb] MbWebView -> WebViewImpl/LocalFrame/Page created\n");
      });
  return handle;
}

void MB_CALL_TYPE mbDestroyWebView(mbWebView handle) {
  content::MbWebView* view = ViewFor(handle);
  if (!view || !view->preDestroyOnUiThread())
    return;
  content::ThreadCall::callBlinkThreadAsync(
      MB_FROM_HERE, [view] { view->preDestroyOnBlinkThread(); });
}

void MB_CALL_TYPE mbLoadURL(mbWebView handle, const utf8* url) {
  if (!url)
    return;
  auto request_url = std::make_shared<std::string>(url);
  std::fprintf(stderr, "[mb] mbLoadURL(%s) -> mbnet/Blink loader\n",
               request_url->c_str());
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      MB_FROM_HERE, handle,
      [request_url](content::MbWebView* view) {
        view->resetState();
        view->loadUrl(request_url->c_str());
        std::fprintf(stderr,
                     "[mb] MbWebView::loadUrl -> WebLocalFrame/DocumentLoader dispatched\n");
      });
}

void MB_CALL_TYPE mbResize(mbWebView handle, int width, int height) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      MB_FROM_HERE, handle, [width, height](content::MbWebView* view) {
        view->onResize(width, height, false);
      });
}

void MB_CALL_TYPE mbOnPaintBitUpdated(mbWebView handle,
                                      mbPaintBitUpdatedCallback callback,
                                      void* parameter) {
  if (content::MbWebView* view = ViewFor(handle))
    view->getClosure().setPaintBitUpdatedCallback(callback, parameter);
}

void MB_CALL_TYPE mbOnLoadingFinish(mbWebView handle,
                                    mbLoadingFinishCallback callback,
                                    void* parameter) {
  if (content::MbWebView* view = ViewFor(handle))
    view->getClosure().setLoadingFinishCallback(callback, parameter);
}

void MB_CALL_TYPE mbOnLoadUrlBegin(mbWebView handle,
                                   mbLoadUrlBeginCallback callback,
                                   void* parameter) {
  if (content::MbWebView* view = ViewFor(handle))
    view->getClosure().setLoadUrlBeginCallback(callback, parameter);
}

void MB_CALL_TYPE mbOnDidCreateScriptContext(
    mbWebView handle,
    mbDidCreateScriptContextCallback callback,
    void* parameter) {
  if (content::MbWebView* view = ViewFor(handle))
    view->getClosure().setDidCreateScriptContextCallback(callback, parameter);
}

void MB_CALL_TYPE mbOnJsQuery(mbWebView handle,
                              mbJsQueryCallback callback,
                              void* parameter) {
  content::MbWebView* view = ViewFor(handle);
  if (!view)
    return;
  auto* closure = new std::function<void(mbJsExecState, int64_t, int,
                                          const utf8*)>(
      [handle, callback, parameter](mbJsExecState state, int64_t query_id,
                                    int custom_message,
                                    const utf8* request) {
        callback(handle, parameter, state, query_id, custom_message, request);
      });
  view->getClosure().setJsQueryClosure(closure);
}

mbWebFrameHandle MB_CALL_TYPE mbWebFrameGetMainFrame(mbWebView handle) {
  content::MbWebView* view = ViewFor(handle);
  if (!view)
    return nullptr;
  auto* frame = static_cast<blink::WebLocalFrame*>(view->getMainFrame());
  return reinterpret_cast<mbWebFrameHandle>(
      content::getFrameIdByWebLocalFrame(frame));
}

void MB_CALL_TYPE mbRunJs(mbWebView handle,
                          mbWebFrameHandle frame,
                          const utf8* script,
                          BOOL is_in_closure,
                          mbRunJsCallback callback,
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
    content::ThreadCall::callBlinkThreadAsync(MB_FROM_HERE, std::move(run));
}

void MB_CALL_TYPE mbResponseQuery(mbWebView handle,
                                  int64_t query_id,
                                  int custom_message,
                                  const utf8* response) {
  auto* response_copy = new std::string(response ? response : "");
  content::ThreadCall::callBlinkThreadAsync(
      MB_FROM_HERE,
      [handle, query_id, custom_message, response_copy] {
        auto* id_info =
            reinterpret_cast<std::pair<mbWebFrameHandle, int>*>(query_id);
        content::MbWebView* view = ViewFor(handle);
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

void MB_CALL_TYPE mbNetSetMIMEType(mbNetJob job, const char* type) {
  mbnet::onNetSetMIMEType(job, type);
}

void MB_CALL_TYPE mbNetSetData(mbNetJob job, void* buffer, int length) {
  mbnet::onNetSetData(job, buffer, length);
}

void MB_CALL_TYPE mbNetSetHTTPHeaderFieldUtf8(mbNetJob job,
                                              const utf8* key,
                                              const utf8* value,
                                              BOOL response) {
  mbnet::onNetSetHTTPHeaderFieldCommon(job, key, value, response);
}

BOOL MB_CALL_TYPE mbFireMouseEvent(mbWebView handle,
                                   unsigned int message,
                                   int x,
                                   int y,
                                   unsigned int flags) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      MB_FROM_HERE, handle, [message, x, y, flags](content::MbWebView* view) {
        auto* widget = view->getWebView()->MainFrameWidget();
        const float scale = widget->GetOriginalScreenInfo().device_scale_factor;
        // AppKit reports DIP coordinates; Blink input uses physical pixels.
        view->onMouseMessage(message, std::lround(x * scale),
                             std::lround(y * scale), flags);
      });
  return TRUE;
}

BOOL MB_CALL_TYPE mbFireMouseWheelEvent(mbWebView handle,
                                        int x,
                                        int y,
                                        int wheel_delta,
                                        unsigned int flags) {
  content::MbWebView* view = ViewFor(handle);
  if (!view)
    return FALSE;
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      MB_FROM_HERE, handle, [x, y, wheel_delta, flags](content::MbWebView* view) {
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

BOOL MB_CALL_TYPE mbFireKeyDownEvent(mbWebView handle,
                                     unsigned int virtual_key,
                                     unsigned int flags,
                                     BOOL system_key) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      MB_FROM_HERE, handle,
      [virtual_key, flags, system_key](content::MbWebView* view) {
        view->onKeyDown(virtual_key, flags, system_key);
      });
  return TRUE;
}

BOOL MB_CALL_TYPE mbFireKeyUpEvent(mbWebView handle,
                                   unsigned int virtual_key,
                                   unsigned int flags,
                                   BOOL system_key) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      MB_FROM_HERE, handle,
      [virtual_key, flags, system_key](content::MbWebView* view) {
        view->onKeyUp(virtual_key, flags, system_key);
      });
  return TRUE;
}

BOOL MB_CALL_TYPE mbFireKeyPressEvent(mbWebView handle,
                                      unsigned int character,
                                      unsigned int flags,
                                      BOOL system_key) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      MB_FROM_HERE, handle,
      [character, flags, system_key](content::MbWebView* view) {
        view->onKeyPress(character, flags, system_key);
      });
  return TRUE;
}

void MB_CALL_TYPE mbGetCaretRect(mbWebView handle, mbRect* rect) {
  if (!rect)
    return;
  if (content::MbWebView* view = ViewFor(handle)) {
    const gfx::Point caret = view->getCaretPos();
    rect->x = caret.x();
    rect->y = caret.y();
    rect->w = 1;
    rect->h = 20;
  }
}

void MB_CALL_TYPE mbSetFocus(mbWebView handle) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      MB_FROM_HERE, handle,
      [](content::MbWebView* view) { view->setFocus(); });
}

void MB_CALL_TYPE mbKillFocus(mbWebView handle) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      MB_FROM_HERE, handle,
      [](content::MbWebView* view) { view->killFocus(); });
}

void mbMacSetDeviceScaleFactor(mbWebView handle, float scale) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      MB_FROM_HERE, handle, [scale](content::MbWebView* view) {
        view->setDeviceScaleFactor(scale);
      });
}

void mbMacSetComposition(mbWebView handle,
                         const char16_t* text,
                         size_t length,
                         BOOL committed) {
  std::u16string composition(text ? text : u"", length);
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      MB_FROM_HERE, handle,
      [composition = std::move(composition), committed](
          content::MbWebView* view) {
        view->onImeText(composition, committed);
      });
}
void mbMacSetStoragePaths(mbWebView handle,
                          const char* cookie_path,
                          const char* local_storage_path) {
  std::string cookie(cookie_path ? cookie_path : "");
  std::string local_storage(local_storage_path ? local_storage_path : "");
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      MB_FROM_HERE, handle,
      [cookie = std::move(cookie),
       local_storage = std::move(local_storage)](content::MbWebView* view) {
        view->setCookieJarFullPath(cookie.c_str());
        view->setLocalStorageFullPath(local_storage.c_str());
      });
}

}  // extern "C"

void mbShowWindowImpl(mbWebView handle, BOOL show) {
  content::ThreadCall::callBlinkThreadAsyncWithValid(
      MB_FROM_HERE, handle,
      [show](content::MbWebView* view) { view->setShow(show); });
}

void mbDestroyWebViewImpl(mbWebView handle) {
  mbDestroyWebView(handle);
}

BOOL mbFireMouseEventImpl(mbWebView handle,
                          unsigned int message,
                          int x,
                          int y,
                          unsigned int flags) {
  return mbFireMouseEvent(handle, message, x, y, flags);
}

BOOL mbFireKeyPressEventImpl(mbWebView handle,
                             unsigned int character,
                             unsigned int flags,
                             BOOL system_key) {
  return mbFireKeyPressEvent(handle, character, flags, system_key);
}

BOOL mbFireKeyUpEventImpl(mbWebView handle,
                          unsigned int virtual_key,
                          unsigned int flags,
                          BOOL system_key) {
  return mbFireKeyUpEvent(handle, virtual_key, flags, system_key);
}

BOOL mbFireKeyDownEventImpl(mbWebView handle,
                            unsigned int virtual_key,
                            unsigned int flags,
                            BOOL system_key) {
  return mbFireKeyDownEvent(handle, virtual_key, flags, system_key);
}

BOOL mbFireMouseWheelEventImpl(mbWebView handle,
                               int x,
                               int y,
                               int wheel_delta,
                               unsigned int flags) {
  return mbFireMouseWheelEvent(handle, x, y, wheel_delta, flags);
}

BOOL mbFireWindowsMessageImpl(mbWebView,
                              HWND,
                              UINT,
                              WPARAM,
                              LPARAM,
                              LRESULT* result) {
  if (result)
    *result = 0;
  return FALSE;
}
