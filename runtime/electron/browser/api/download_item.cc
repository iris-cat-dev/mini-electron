// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include <algorithm>
#include "runtime/electron/browser/api/download_item.h"
#include "runtime/electron/node_bindings.h"

#include "base/strings/utf_string_conversions.h"
#include "runtime/electron/common/api/event_emitter.h"
#include "runtime/electron/common/gin_helper/object_template_builder.h"
#include "runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "runtime/electron/common/gin_helper/public/wrapper_info.h"
#include "runtime/electron/common/id_live_detect.h"
#include "runtime/electron/common/node_register_help.h"
#include "runtime/electron/common/string_util.h"
#include "runtime/engine/common/thread_call.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"

namespace atom {

ApiDownloadItem::ApiDownloadItem(
    v8::Isolate* isolate, v8::Local<v8::Object> wrapper)
    : m_id(IdLiveDetect::get()->constructed(this)),
      m_recvSize(0),
      m_allSize(0),
      m_state(kProgressing),
      m_isPaused(false)
{
    gin_helper::Wrappable<ApiDownloadItem>::InitWith(isolate, wrapper);
}

ApiDownloadItem::~ApiDownloadItem()
{
    IdLiveDetect::get()->deconstructed(m_id);
}

ApiDownloadItem* ApiDownloadItem::create(v8::Isolate* isolate)
{
    v8::Local<v8::Function> function =
        v8::Local<v8::Function>::New(isolate, constructor);
    v8::Local<v8::Object> object;
    if (!function->NewInstance(isolate->GetCurrentContext()).ToLocal(&object))
        return nullptr;
    ApiDownloadItem* item = static_cast<ApiDownloadItem*>(
        WrappableBase::GetNativePtr(object, &kWrapperInfo));
    item->m_liveSelf.Reset(isolate, object);
    return item;
}

void ApiDownloadItem::init(
    v8::Isolate* isolate, v8::Local<v8::Object> target)
{
    v8::Local<v8::FunctionTemplate> function =
        v8::FunctionTemplate::New(isolate, newFunction);
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    function->SetClassName(
        v8::String::NewFromUtf8(isolate, "DownloadItem").ToLocalChecked());
    gin_helper::ObjectTemplateBuilder builder(isolate, function->InstanceTemplate());
    builder.SetMethod("setSavePath", &ApiDownloadItem::setSavePathApi);
    builder.SetMethod("getSavePath", &ApiDownloadItem::getSavePathApi);
    builder.SetMethod("setSaveDialogOptions", &ApiDownloadItem::setSaveDialogOptionsApi);
    builder.SetMethod("getSaveDialogOptions", &ApiDownloadItem::getSaveDialogOptionsApi);
    builder.SetMethod("pause", &ApiDownloadItem::pauseApi);
    builder.SetMethod("isPaused", &ApiDownloadItem::isPausedApi);
    builder.SetMethod("resume", &ApiDownloadItem::resumeApi);
    builder.SetMethod("canResume", &ApiDownloadItem::canResumeApi);
    builder.SetMethod("cancel", &ApiDownloadItem::cancelApi);
    builder.SetMethod("getURL", &ApiDownloadItem::getURLApi);
    builder.SetMethod("getMimeType", &ApiDownloadItem::getMimeTypeApi);
    builder.SetMethod("hasUserGesture", &ApiDownloadItem::hasUserGestureApi);
    builder.SetMethod("getFilename", &ApiDownloadItem::getFilenameApi);
    builder.SetMethod("getTotalBytes", &ApiDownloadItem::getTotalBytesApi);
    builder.SetMethod("getReceivedBytes", &ApiDownloadItem::getReceivedBytesApi);
    builder.SetMethod("getContentDisposition", &ApiDownloadItem::getContentDispositionApi);
    builder.SetMethod("getState", &ApiDownloadItem::getStateApi);
    builder.SetMethod("getURLChain", &ApiDownloadItem::getURLChainApi);
    builder.SetMethod("getLastModifiedTime", &ApiDownloadItem::getLastModifiedTimeApi);
    builder.SetMethod("getETag", &ApiDownloadItem::getETagApi);
    builder.SetMethod("getStartTime", &ApiDownloadItem::getStartTimeApi);

    v8::Local<v8::Function> constructorFunction =
        function->GetFunction(context).ToLocalChecked();
    constructor.Reset(isolate, constructorFunction);
    target->Set(context,
        v8::String::NewFromUtf8(isolate, "DownloadItem").ToLocalChecked(),
        constructorFunction).Check();
}

void ApiDownloadItem::newFunction(
    const v8::FunctionCallbackInfo<v8::Value>& args)
{
    if (!args.IsConstructCall())
        return;
    new ApiDownloadItem(args.GetIsolate(), args.This());
    args.GetReturnValue().Set(args.This());
}

void ApiDownloadItem::setSavePathApi(const std::string path)
{
    m_savePath = StringUtil::normalizePath(path);
}

std::string ApiDownloadItem::getSavePathApi() const { return m_savePath; }
void ApiDownloadItem::setSaveDialogOptionsApi(
    const v8::FunctionCallbackInfo<v8::Value>&) {}
void ApiDownloadItem::getSaveDialogOptionsApi(
    const v8::FunctionCallbackInfo<v8::Value>&) const {}
void ApiDownloadItem::pauseApi()
{
    if (m_state != kProgressing)
        return;
    m_isPaused = true;
    if (m_paused)
        m_paused->store(true, std::memory_order_release);
}
bool ApiDownloadItem::isPausedApi() const { return m_isPaused; }
void ApiDownloadItem::resumeApi()
{
    m_isPaused = false;
    if (m_paused)
        m_paused->store(false, std::memory_order_release);
}
bool ApiDownloadItem::canResumeApi() const { return false; }
void ApiDownloadItem::cancelApi()
{
    if (m_state != kProgressing)
        return;
    m_state = kCancelled;
    if (m_canceled)
        m_canceled->store(true, std::memory_order_release);
}

void ApiDownloadItem::finishCancelledBeforeStart()
{
    if (m_state == kCancelled)
        finish(MINI_ELECTRON_LOADING_CANCELED);
}

std::string ApiDownloadItem::getURLApi() const { return m_url; }
std::string ApiDownloadItem::getMimeTypeApi() const { return m_mime; }
bool ApiDownloadItem::hasUserGestureApi() const { return true; }
std::string ApiDownloadItem::getFilenameApi() const
{
    size_t slash = m_url.find_last_of("/\\");
    return slash == std::string::npos ? m_url : m_url.substr(slash + 1);
}
int ApiDownloadItem::getTotalBytesApi() const
{
    return static_cast<int>(m_allSize);
}
int ApiDownloadItem::getReceivedBytesApi() const
{
    return static_cast<int>(m_recvSize);
}
std::string ApiDownloadItem::getContentDispositionApi() const
{
    return m_disposition;
}
std::string ApiDownloadItem::getStateApi() const
{
    switch (m_state) {
    case kProgressing: return "progressing";
    case kCompleted: return "completed";
    case kCancelled: return "cancelled";
    case kInterrupted: return "interrupted";
    }
    return "interrupted";
}
std::vector<std::string> ApiDownloadItem::getURLChainApi() const
{
    return { m_url };
}
std::string ApiDownloadItem::getLastModifiedTimeApi() const { return {}; }
std::string ApiDownloadItem::getETagApi() const { return {}; }
std::string ApiDownloadItem::getStartTimeApi() const { return {}; }

void ApiDownloadItem::updateProgress(size_t received)
{
    if (m_done || m_state != kProgressing)
        return;
    m_recvSize = received;
    mate::EventEmitter<ApiDownloadItem>::emit(
        std::string("updated"), std::string("progressing"));
}

void ApiDownloadItem::finish(mini_electron_loading_result result)
{
    if (m_done)
        return;
    m_done = true;
    const bool canceled = m_state == kCancelled
        || result == MINI_ELECTRON_LOADING_CANCELED;
    m_state = canceled ? kCancelled
        : result == MINI_ELECTRON_LOADING_SUCCEEDED ? kCompleted : kInterrupted;
    m_isPaused = false;
    if (m_paused)
        m_paused->store(false, std::memory_order_release);
    const std::string state = getStateApi();
    mate::EventEmitter<ApiDownloadItem>::emit(
        std::string("done"), state, state);
    m_liveSelf.Reset();
}

gin_helper::WrapperInfo ApiDownloadItem::kWrapperInfo = {
    gin_helper::GinEmbedder::kEmbedderNativeGin
};
v8::Persistent<v8::Function> ApiDownloadItem::constructor;

void initializeBrowserDownloadItemApi(v8::Local<v8::Object> exports,
    v8::Local<v8::Value>, v8::Local<v8::Context> context, void*)
{
    ApiDownloadItem::init(context->GetIsolate(), exports);
}

static const char BrowserDownloadItemName[] =
    "console.log('BrowserDownloadItemNative');;";
static NodeNative BrowserDownloadItemNative {
    "DownloadItem", BrowserDownloadItemName,
    sizeof(BrowserDownloadItemName) - 1
};

NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_MANUAL(
    electron_browser_downloaditem, initializeBrowserDownloadItemApi,
    &BrowserDownloadItemNative)

} // namespace atom

namespace gin_helper {

v8::Local<v8::Value> ConvertToV8(
    v8::Isolate* isolate, const atom::ApiDownloadItem& item)
{
    return const_cast<atom::ApiDownloadItem&>(item).GetWrapper(isolate);
}

} // namespace gin_helper
