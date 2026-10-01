// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "runtime/electron/node_bindings.h"

#include "runtime/electron/common/gin_helper/object_template_builder.h"
#include "runtime/electron/common/gin_helper/dictionary.h"
#include "runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "runtime/electron/common/gin_helper/public/wrapper_info.h"
#include "third_party/blink/renderer/platform/weborigin/scheme_registry.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "third_party/libuv/include/uv.h"
#include "runtime/electron/browser/api/protocol_interface.h"
#include "runtime/electron/common/node_register_help.h"
#include "runtime/electron/common/api/event_emitter.h"
#include "runtime/engine/common/thread_call.h"
#include "runtime/engine/public/engine_api.h"
#include "base/memory/ref_counted.h"
#include "runtime/network/loader/web_url_loader_internal.h"
#include "runtime/network/loader/web_url_loader_manager.h"
#include "url/url_util.h"
#include <vector>
#include <map>

namespace atom {

namespace {

struct ProtocolCallbackInfo {
    int jobId;
    std::string type;
};

}

class Protocol : public mate::EventEmitter<Protocol>, public ProtocolInterface {
public:
    Protocol(v8::Isolate* isolate, v8::Local<v8::Object> wrapper, v8::Local<v8::Value> jsReciver)
    {
        gin_helper::Wrappable<Protocol>::InitWith(isolate, wrapper);
        ProtocolInterface::m_inst = this;
        m_jsReciver.Reset(isolate, v8::Local<v8::Function>::Cast(jsReciver));
    }

    static void init(v8::Isolate* isolate, v8::Local<v8::Object> target)
    {
        v8::Local<v8::FunctionTemplate> prototype = v8::FunctionTemplate::New(isolate, newFunction);
        v8::Local<v8::Context> context = isolate->GetCurrentContext();

        prototype->SetClassName(v8::String::NewFromUtf8(isolate, "Protocol").ToLocalChecked());
        gin_helper::ObjectTemplateBuilder builder(isolate, prototype->InstanceTemplate());
        builder.SetMethod("registerStandardSchemes", &Protocol::registerStandardSchemesApi);
        builder.SetMethod("registerSchemesAsPrivileged", &Protocol::registerSchemesAsPrivilegedApi);
        //         builder.SetMethod("registerFileProtocol", &Protocol::registerFileProtocolApi);
        //         builder.SetMethod("registerBufferProtocol", &Protocol::registerBufferProtocolApi);
        //         builder.SetMethod("registerStringProtocol", &Protocol::registerStringProtocolApi);
        //         builder.SetMethod("registerHttpProtocol", &Protocol::registerHttpProtocolApi);
        builder.SetMethod("registerStreamProtocol", &Protocol::registerStreamProtocolApi);

        builder.SetMethod("interceptFileProtocol", &Protocol::interceptFileProtocolApi);
        builder.SetMethod("interceptStringProtocol", &Protocol::interceptStringProtocolApi);
        builder.SetMethod("interceptBufferProtocol", &Protocol::interceptBufferProtocolApi);
        builder.SetMethod("interceptHttpProtocol", &Protocol::interceptHttpProtocolApi);
        builder.SetMethod("interceptStreamProtocol", &Protocol::interceptStreamProtocolApi);
        builder.SetMethod("uninterceptProtocol", &Protocol::uninterceptProtocolApi);

        builder.SetMethod("_registerProtocol", &Protocol::_registerProtocolApi);
        builder.SetMethod("_unregisterProtocol", &Protocol::_unregisterProtocolApi);
        builder.SetMethod("_isProtocolHandled", &Protocol::_isProtocolHandledApi);
        builder.SetMethod("onHandlerFinish", &Protocol::onHandlerFinishApi);

        constructor.Reset(isolate, prototype->GetFunction(context).ToLocalChecked());
        target->Set(context, v8::String::NewFromUtf8(isolate, "Protocol").ToLocalChecked(), prototype->GetFunction(context).ToLocalChecked());
    }

    static void newFunction(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        v8::Isolate* isolate = args.GetIsolate();
        if (!args.IsConstructCall())
            DebugBreak();

        v8::Local<v8::Value> jsReciver = args[0];

        new Protocol(isolate, args.This(), jsReciver);
        args.GetReturnValue().Set(args.This());
        return;
    }

    void registerStandardSchemesApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
    }
    void registerSchemesAsPrivilegedApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        v8::Isolate* isolate = args.GetIsolate();
        if (args.Length() == 0 || !args[0]->IsArray())
            return;
        v8::Local<v8::Context> context = isolate->GetCurrentContext();
        auto schemes = args[0].As<v8::Array>();
        for (uint32_t i = 0; i < schemes->Length(); ++i) {
            v8::Local<v8::Value> entry;
            if (!schemes->Get(context, i).ToLocal(&entry) || !entry->IsObject())
                continue;
            gin_helper::Dictionary definition(isolate, entry.As<v8::Object>());
            std::string scheme;
            v8::Local<v8::Value> value;
            if (!definition.Get("scheme", &scheme) ||
                !definition.Get("privileges", &value) || !value->IsObject())
                continue;
            gin_helper::Dictionary privileges(isolate, value.As<v8::Object>());
            bool standard = false, secure = false, fetch = false, cors = false, serviceWorkers = false;
            privileges.Get("standard", &standard);
            privileges.Get("secure", &secure);
            privileges.Get("supportFetchAPI", &fetch);
            privileges.Get("corsEnabled", &cors);
            privileges.Get("allowServiceWorkers", &serviceWorkers);
            content::ThreadCall::callBlinkThreadSync(FROM_HERE, [=] {
                if (standard && !url::IsStandardScheme(scheme))
                    url::AddStandardScheme(scheme.c_str(), url::SCHEME_WITH_HOST);
                if (secure) {
                    url::AddSecureScheme(scheme.c_str());
                    blink::SchemeRegistry::RegisterURLSchemeBypassingSecureContextCheck(WTF::String::FromUTF8(scheme));
                }
                if (cors)
                    url::AddCorsEnabledScheme(scheme.c_str());
                if (fetch)
                    blink::SchemeRegistry::RegisterURLSchemeAsSupportingFetchAPI(WTF::String::FromUTF8(scheme));
                if (serviceWorkers)
                    blink::SchemeRegistry::RegisterURLSchemeAsAllowingServiceWorkers(WTF::String::FromUTF8(scheme));
            });
        }
    }
    //     void registerFileProtocolApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    //     {
    //         OutputDebugStringA("");
    //     }
    //
    //     void registerBufferProtocolApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    //     {
    //     }
    //     void registerStringProtocolApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    //     {
    //     }
    //     void registerHttpProtocolApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    //     {
    //     }
    void registerStreamProtocolApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
    }
    void unregisterProtocolApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
    }
    void interceptFileProtocolApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
    }
    void interceptStringProtocolApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
    }
    void interceptBufferProtocolApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
    }
    void interceptHttpProtocolApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
    }
    void interceptStreamProtocolApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
    }
    void uninterceptProtocolApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
    }

    struct ProtocolInfo {
        ProtocolInfo(int handlerId, const std::string& protocolType)
        {
            id = handlerId;
            type = protocolType;
        }
        int id;
        std::string type;
    };

    bool _registerProtocolApi(const std::string& scheme, int handlerId, const std::string& type)
    {
        base::AutoLock autoLock(m_lock);
        std::map<std::string, ProtocolInfo>::iterator it = m_schemeToHandleId.find(scheme);
        if (it != m_schemeToHandleId.end())
            return false;

        content::ThreadCall::callBlinkThreadAsync(FROM_HERE, [scheme] {
            WTF::String schemeStr = WTF::String::FromUTF8(scheme);
            blink::SchemeRegistry::RegisterURLSchemeAsSupportingFetchAPI(schemeStr);
            blink::SchemeRegistry::RegisterURLSchemeAsAllowingServiceWorkers(schemeStr);
        });

        m_schemeToHandleId.insert(std::make_pair(scheme, ProtocolInfo(handlerId, type)));
        return true;
    }

    void _unregisterProtocolApi(const std::string& scheme)
    {
        base::AutoLock autoLock(m_lock);
        m_schemeToHandleId.erase(scheme);
    }

    bool _isProtocolHandledApi(const std::string& scheme)
    {
        base::AutoLock autoLock(m_lock);
        std::map<std::string, ProtocolInfo>::iterator it = m_schemeToHandleId.find(scheme);
        return (it != m_schemeToHandleId.end());
    }

    static std::string normalizeFilePath(const std::string& path)
    {
        std::string result = "file:///";
        for (size_t i = 0; i < path.size(); ++i) {
            if (path[i] == '\\')
                result += '/';
            else
                result += path[i];
        }
        return result;
    }

    void onHandlerFinishApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        v8::Isolate* isolate = args.GetIsolate();
        uint64_t requestId = args[1].As<v8::BigInt>()->Uint64Value();
        auto pending = m_pendingResponses.find(requestId);
        if (pending == m_pendingResponses.end())
            return;
        ProtocolCallbackInfo info = std::move(pending->second);
        m_pendingResponses.erase(pending);

        gin_helper::Dictionary response(isolate, args[0]->IsObject()
            ? args[0].As<v8::Object>() : v8::Object::New(isolate));
        std::string filePath, mimeType, statusText;
        std::vector<char> data;
        std::map<std::string, std::string> headers;
        int error = 0, statusCode = 200;
        response.Get("error", &error);
        response.Get("mimeType", &mimeType);
        response.Get("statusCode", &statusCode);
        response.Get("statusText", &statusText);
        if (info.type == "file") {
            if (args[0]->IsString())
                gin_helper::ConvertFromV8(isolate, args[0], &filePath);
            else
                response.Get("path", &filePath);
            if (filePath.empty())
                error = -2;
        } else if (info.type == "string") {
            std::string text;
            if (!response.Get("data", &text))
                error = -2;
            data.assign(text.begin(), text.end());
        } else {
            v8::Local<v8::Value> buffer;
            if (!response.Get("data", &buffer)) {
                error = -2;
            } else {
                size_t size = 0;
                char* bytes = nodeBufferGetData(&buffer, &size);
                if (size)
                    data.assign(bytes, bytes + size);
            }
        }
        v8::Local<v8::Value> headerValue;
        if (response.Get("headers", &headerValue) && headerValue->IsObject()) {
            auto object = headerValue.As<v8::Object>();
            auto context = isolate->GetCurrentContext();
            auto names = object->GetOwnPropertyNames(context).ToLocalChecked();
            for (uint32_t i = 0; i < names->Length(); ++i) {
                auto name = names->Get(context, i).ToLocalChecked();
                std::string key, value;
                gin_helper::ConvertFromV8(isolate, name, &key);
                gin_helper::ConvertFromV8(isolate, object->Get(context, name).ToLocalChecked(), &value);
                headers.emplace(std::move(key), std::move(value));
            }
        }
        content::ThreadCall::callBlinkThreadAsync(FROM_HERE,
            [info = std::move(info), filePath = std::move(filePath), data = std::move(data),
             mimeType = std::move(mimeType), headers = std::move(headers),
             statusText = std::move(statusText), statusCode, error]() mutable {
                auto manager = mini_electron::WebURLLoaderManager::sharedInstance();
                mini_electron::AutoLockJob lock(manager, info.jobId);
                auto job = lock.lock();
                if (!job)
                    return;
                if (error) {
                    mini_electron_net_cancel_request(job);
                    return;
                }
                if (info.type == "file") {
                    mini_electron_net_change_request_url(job, normalizeFilePath(filePath).c_str());
                } else {
                    mini_electron_net_set_data(job, data.data(), static_cast<int>(data.size()));
                    job->m_response.SetHttpStatusCode(statusCode);
                    job->m_response.SetHttpStatusText(blink::WebString::FromUTF8(statusText));
                    if (!mimeType.empty())
                        mini_electron_net_set_mime_type(job, mimeType.c_str());
                    for (const auto& [key, value] : headers)
                        mini_electron_net_set_http_header_field_utf8(job, key.c_str(), value.c_str(), TRUE);
                }
                mini_electron_net_continue_job(job);
            });
    }

    virtual bool handleLoadUrlBegin(void* param, const char* url, void* job) override
    {
        const char* separator = strstr(url, "://");
        if (!separator)
            return false;
        std::string scheme(url, separator);
        base::AutoLock autoLock(m_lock);
        auto handler = m_schemeToHandleId.find(scheme);
        if (handler == m_schemeToHandleId.end())
            return false;
        int id = handler->second.id;
        uint64_t requestId = ++m_nextRequestId;
        ProtocolCallbackInfo info { static_cast<mini_electron::WebURLLoaderInternal*>(job)->m_id,
            handler->second.type };
        std::string requestUrl(url);
        std::string referrer(mini_electron_net_get_referrer(job));
        auto method = mini_electron_net_get_request_method(job);
        // Hold before dispatch; the handler may resolve synchronously or asynchronously.
        mini_electron_net_hold_job_to_asyn_commit(job);
        content::ThreadCall::callUiThreadAsync(FROM_HERE,
            [this, id, requestId, info = std::move(info),
             requestUrl = std::move(requestUrl), referrer = std::move(referrer), method] {
                m_pendingResponses.emplace(requestId, std::move(info));
                auto isolate = v8::Isolate::GetCurrent();
                v8::HandleScope handleScope(isolate);
                auto request = v8::Object::New(isolate);
                gin_helper::Dictionary dictionary(isolate, request);
                dictionary.Set("url", requestUrl);
                dictionary.Set("referrer", referrer);
                dictionary.Set("method", method == kMiniElectronRequestTypeGet ? "GET" :
                    (method == kMiniElectronRequestTypePost ? "POST" : "PUT"));
                v8::Local<v8::Value> arguments[] = { v8::Integer::New(isolate, id),
                    request, v8::BigInt::NewFromUnsigned(isolate, requestId) };
                auto callback = m_jsReciver.Get(isolate);
                auto context = callback->GetCreationContextChecked();
                v8::Context::Scope contextScope(context);
                node::MakeCallback(isolate, request, callback, 3, arguments, { 0, 0 });
            });
        return true;
    }

    v8::Local<v8::Object> getWrapper(v8::Isolate* isolate) override
    {
        return GetWrapper(isolate);
    }

public:
    static gin_helper::WrapperInfo kWrapperInfo;
    static v8::Persistent<v8::Function> constructor;

    v8::Persistent<v8::Function> m_jsReciver;
    std::map<std::string, ProtocolInfo> m_schemeToHandleId;
    std::map<uint64_t, ProtocolCallbackInfo> m_pendingResponses;
    uint64_t m_nextRequestId = 0;
    base::Lock m_lock;
};

v8::Persistent<v8::Function> Protocol::constructor;
gin_helper::WrapperInfo Protocol::kWrapperInfo = { gin_helper::GinEmbedder::kEmbedderNativeGin };
ProtocolInterface* ProtocolInterface::m_inst = nullptr;

void initializeProtocolApi(v8::Local<v8::Object> exports, v8::Local<v8::Value> unused, v8::Local<v8::Context> context, void* priv)
{
    Protocol::init(context->GetIsolate(), exports);
}

} // atom namespace

static const char BrowserProtocolNative[] = "console.log('BrowserProtocolNative');;";
static NodeNative nativeBrowserProtocolNative { "Protocol", BrowserProtocolNative, sizeof(BrowserProtocolNative) - 1 };

NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_MANUAL(electron_browser_protocol, atom::initializeProtocolApi, &nativeBrowserProtocolNative)
