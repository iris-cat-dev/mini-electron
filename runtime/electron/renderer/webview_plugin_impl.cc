
#define NODE_WANT_INTERNALS 1

#include "runtime/electron/renderer/webview_plugin_impl.h"

#include "runtime/electron/node_bindings.h"
#include "runtime/electron/browser/api/web_contents.h"
#include "runtime/electron/common/id_live_detect.h"
#include "runtime/electron/common/node_binding.h"
#include "runtime/electron/common/api/event_emitter_caller.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "third_party/libnode/src/node_version.h"
#include "third_party/libuv/include/uv.h"
#include "base/json/json_writer.h"
#include "v8.h"
#include <string>

namespace atom {

extern NPNetscapeFuncs* g_npBrowserFunctions;

void WebviewPluginImpl::staticDidCreateScriptContextCallback(
    mini_electron_web_view webView, WebviewPluginImpl* self, mini_electron_web_frame_handle frame, void* context, int extensionGroup, int worldId)
{
    //     v8::Local<v8::Context>& contextV8 = *(v8::Local<v8::Context>*)context;
    //     contextV8->SetAlignedPointerInEmbedderData(node::Environment::kContextEmbedderDataIndex, nullptr);
    //
    //     BlinkMicrotaskSuppressionHandle handle = nodeBlinkMicrotaskSuppressionEnter(contextV8->GetIsolate());
    //
    //     self->m_nodeBinding = new NodeBindings(false);
    //     self->m_nodeBinding->setUvLoop(ThreadCall::getBlinkLoop());
    //     node::Environment* env = self->m_nodeBinding->createEnvironment(contextV8);
     
    //     self->m_nodeBinding->loadEnvironment();
    //
    //     nodeBlinkMicrotaskSuppressionLeave(handle);
    //
    //     self->loadPreloadURL();
}

void WebviewPluginImpl::staticOnWillReleaseScriptContextCallback(mini_electron_web_view webView, WebviewPluginImpl* self, mini_electron_web_frame_handle frame, void* context, int worldId)
{
    //     v8::Local<v8::Context>& contextV8 = *(v8::Local<v8::Context>*)context;
    //     node::Environment* env = node::Environment::GetCurrent(contextV8);
    //     if (env)
    //         mate::emitEvent(env->isolate(), env->process_object(), "exit");
    //
    //     delete self->m_nodeBinding;
    //     self->m_nodeBinding = nullptr;
}

void WebviewPluginImpl::onLoadingFinishCallback(mini_electron_web_view webView, WebviewPluginImpl* self, const utf8* url, mini_electron_loading_result result, const utf8* failedReason)
{
}

void WebviewPluginImpl::onDocumentReady(mini_electron_web_view webView, WebviewPluginImpl* self)
{
    //     base::ListValue listParams;
    //     std::string jsonRet;
    //     self->guestSendMessageToHost("dom-ready", listParams);
}


WebviewPluginImpl::WebviewPluginImpl(mini_electron_web_view parentWebview)
{
    m_id = IdLiveDetect::get()->constructed(this);
    m_preloadcode = nullptr;
    m_guestId = -1;
    m_npObj = nullptr;
    m_instance = nullptr;
    DebugBreak();

    m_parentWebview = parentWebview;
}

WebviewPluginImpl::~WebviewPluginImpl()
{
    IdLiveDetect::get()->deconstructed(m_id);
}

void WebviewPluginImpl::loadPreloadURL()
{
}

void WebviewPluginImpl::staticOnPaintUpdated(mini_electron_web_view webView, WebviewPluginImpl* self, const HDC hdc, int x, int y, int cx, int cy)
{
    //     int id = self->m_id;
    //     ThreadCall::callBlinkThreadAsync([self, id, x, y, cx, cy] {
    //         if (!IdLiveDetect::get()->isLive(id))
    //             return;
    //         NPRect rect = { (uint16_t)y, (uint16_t)x, (uint16_t)(y + cy), (uint16_t)(x + cx) };
    //         g_npBrowserFunctions->invalidaterect(self->m_instance, &rect);
    //     });
}

void WebviewPluginImpl::onSize(const WINDOWPOS& windowpos)
{
}

void WebviewPluginImpl::onPaint(HDC hdc)
{
}

void WebviewPluginImpl::onSetWinow(const NPWindow& npWindow)
{
    m_npWindow = npWindow;
}

void WebviewPluginImpl::onMouseEvt(uint32_t message, uint32_t wParam, uint32_t lParam)
{
}

void WebviewPluginImpl::onKey(uint32_t message, uint32_t wParam, uint32_t lParam)
{
}

void WebviewPluginImpl::setPreloadURL(const std::string& preload)
{
    std::string preloadURL = preload;
    if (preloadURL.size() > 9 && "file:///" == preloadURL.substr(0, 8))
        preloadURL = preloadURL.substr(8);

    if (m_preloadcode)
        delete m_preloadcode;
    m_preloadcode = new std::string("require('");
    m_preloadcode->append(preloadURL);
    m_preloadcode->append("');");
}

void WebviewPluginImpl::hostSendMessageToGuest(const std::string& channel, const base::Value::List& listParams)
{
    DebugBreak();
}

void WebviewPluginImpl::guestSendMessageToHost(const std::string& channel, const base::Value::List& listParams)
{
    NPVariant voidResponse;

    NPVariant channelNp;
    STRINGZ_TO_NPVARIANT(channel.c_str(), channelNp);

    std::string params;
    base::JSONWriter::Write(listParams, &params);
    NPVariant paramsNp;
    STRINGZ_TO_NPVARIANT(params.c_str(), paramsNp);

    NPVariant args[] = { channelNp, paramsNp };

    NPIdentifier funcID = g_npBrowserFunctions->getstringidentifier("onNativeMessage");

    NPObject* pluginScriptObject;
    NPError err = g_npBrowserFunctions->getvalue(m_instance, NPNVPluginElementNPObject, (void*)&pluginScriptObject);
    if (NPERR_GENERIC_ERROR == err)
        return;

    bool isOk = g_npBrowserFunctions->invoke(m_instance, pluginScriptObject, funcID, args, 2, &voidResponse);
    isOk = isOk;
}

void WebviewPluginImpl::loadURL(
    const std::string& urlString, const std::string& httpReferrerString, const std::string& userAgentString, const std::string& extraHeadersString)
{
}

std::string WebviewPluginImpl::getURL()
{
    return mini_electron_get_url(m_webview);
}

}
