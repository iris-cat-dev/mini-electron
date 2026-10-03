// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "runtime/electron/node_bindings.h"

#include "runtime/electron/common/gin_helper/object_template_builder.h"
#include "runtime/electron/common/gin_helper/dictionary.h"
#include "runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "runtime/electron/common/gin_helper/public/wrapper_info.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "third_party/libuv/include/uv.h"
#include "third_party/libnode/src/node_buffer.h"
#include "runtime/electron/browser/api/protocol_interface.h"
#include "runtime/electron/common/node_register_help.h"
#include "runtime/electron/common/api/event_emitter.h"
#include "runtime/engine/common/thread_call.h"
#include "base/memory/ref_counted.h"
#include "base/files/file_util.h"
#include "url/url_util.h"
#include "base/strings/string_util.h"
#include "base/strings/string_number_conversions.h"
#include "url/gurl.h"
#include <algorithm>
#include <memory>
#include <vector>
#include <map>

namespace atom {

namespace {

struct ProtocolCallbackInfo {
    std::string type;
    ProtocolInterface::BrokerReply brokerReply;
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
        builder.SetMethod("_isProtocolHandled", &Protocol::isProtocolHandled);
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
            scheme = base::ToLowerASCII(scheme);
            if (scheme.empty())
                continue;
            gin_helper::Dictionary privileges(isolate, value.As<v8::Object>());
            bool standard = false, secure = false, fetch = false, cors = false, serviceWorkers = false;
            privileges.Get("standard", &standard);
            privileges.Get("secure", &secure);
            privileges.Get("supportFetchAPI", &fetch);
            privileges.Get("corsEnabled", &cors);
            privileges.Get("allowServiceWorkers", &serviceWorkers);
            RendererPrivilegedScheme registered;
            registered.scheme = scheme;
            registered.standard = standard;
            registered.secure = secure;
            registered.support_fetch_api = fetch;
            registered.cors_enabled = cors;
            registered.allow_service_workers = serviceWorkers;
            {
                base::AutoLock autoLock(m_lock);
                auto existing = std::find_if(m_privilegedSchemes.begin(),
                    m_privilegedSchemes.end(),
                    [&scheme](const RendererPrivilegedScheme& value) {
                        return value.scheme == scheme;
                    });
                if (existing == m_privilegedSchemes.end())
                    m_privilegedSchemes.push_back(std::move(registered));
                else
                    *existing = std::move(registered);
            }
            if (standard && !url::IsStandardScheme(scheme))
                url::AddStandardScheme(scheme.c_str(), url::SCHEME_WITH_HOST);
            if (secure)
                url::AddSecureScheme(scheme.c_str());
            if (cors)
                url::AddCorsEnabledScheme(scheme.c_str());
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

        m_schemeToHandleId.insert(std::make_pair(scheme, ProtocolInfo(handlerId, type)));
        return true;
    }

    void _unregisterProtocolApi(const std::string& scheme)
    {
        base::AutoLock autoLock(m_lock);
        m_schemeToHandleId.erase(scheme);
    }

    bool isProtocolHandled(const std::string& scheme) override
    {
        base::AutoLock autoLock(m_lock);
        std::map<std::string, ProtocolInfo>::iterator it = m_schemeToHandleId.find(scheme);
        return (it != m_schemeToHandleId.end());
    }


    void onHandlerFinishApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        v8::Isolate* isolate = args.GetIsolate();
        if (args.Length() < 2 || !args[1]->IsBigInt())
            return;
        uint64_t requestId = args[1].As<v8::BigInt>()->Uint64Value();
        ProtocolCallbackInfo info;
        {
            base::AutoLock autoLock(m_lock);
            auto pending = m_pendingResponses.find(requestId);
            if (pending == m_pendingResponses.end())
                return;
            info = std::move(pending->second);
            m_pendingResponses.erase(pending);
        }

        gin_helper::Dictionary response(isolate, args[0]->IsObject()
            ? args[0].As<v8::Object>() : v8::Object::New(isolate));
        std::string filePath, mimeType, statusText;
        std::vector<uint8_t> data;
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
        base::Value::Dict result;
        if (error) {
            info.brokerReply({}, "Protocol handler failed with error " +
                std::to_string(error));
            return;
        }
        if (info.type == "file") {
            std::optional<std::vector<uint8_t>> bytes =
                base::ReadFileToBytes(base::FilePath::FromUTF8Unsafe(filePath));
            if (!bytes) {
                info.brokerReply({}, "Protocol file response is unreadable");
                return;
            }
            result.Set("body", base::Value(std::move(*bytes)));
        } else {
            result.Set("body", base::Value(std::move(data)));
        }
        result.Set("statusCode", statusCode);
        result.Set("statusText", statusText);
        result.Set("mimeType", mimeType);
        base::Value::Dict responseHeaders;
        for (const auto& [key, value] : headers)
            responseHeaders.Set(key, value);
        result.Set("headers", std::move(responseHeaders));
        info.brokerReply(std::move(result), {});
    }

    void handleBrokerRequest(int contentsId, uint64_t frameId,
        const base::Value::Dict& payload, BrokerReply reply) override
    {
        const std::string* requestUrl = payload.FindString("url");
        const std::string* method = payload.FindString("method");
        if (!requestUrl || !method) {
            reply({}, "Invalid protocol broker request");
            return;
        }
        GURL parsed(*requestUrl);
        if (!parsed.is_valid() || !parsed.has_scheme()) {
            reply({}, "Invalid protocol URL");
            return;
        }

        int handlerId = 0;
        uint64_t requestId = 0;
        {
            base::AutoLock autoLock(m_lock);
            auto handler = m_schemeToHandleId.find(parsed.scheme());
            if (handler == m_schemeToHandleId.end()) {
                reply({}, "Protocol is not handled");
                return;
            }
            handlerId = handler->second.id;
            requestId = ++m_nextRequestId;
            ProtocolCallbackInfo info;
            info.type = handler->second.type;
            info.brokerReply = std::move(reply);
            m_pendingResponses.emplace(requestId, std::move(info));
        }

        base::Value::Dict requestData = payload.Clone();
        requestData.Remove("kind");
        requestData.Set("contentsId", contentsId);
        requestData.Set("frameId", base::NumberToString(frameId));
        auto ownedRequestData =
            std::make_shared<base::Value::Dict>(std::move(requestData));
        content::ThreadCall::callUiThreadAsync(FROM_HERE,
            [this, handlerId, requestId,
             requestData = std::move(ownedRequestData)] {
                auto isolate = v8::Isolate::GetCurrent();
                if (!isolate)
                    return;
                v8::HandleScope handleScope(isolate);
                auto callback = m_jsReciver.Get(isolate);
                auto context = callback->GetCreationContextChecked();
                v8::Context::Scope contextScope(context);
                std::optional<base::Value> body = requestData->Extract("body");
                v8::Local<v8::Value> request =
                    gin_helper::ConvertToV8(isolate, *requestData);
                if (body && body->is_blob() && !body->GetBlob().empty()) {
                    auto* bytes = new base::Value::BlobStorage(std::move(*body).TakeBlob());
                    v8::Local<v8::Object> buffer = node::Buffer::New(isolate,
                        reinterpret_cast<char*>(bytes->data()), bytes->size(),
                        [](char*, void* owned) {
                            delete static_cast<base::Value::BlobStorage*>(owned);
                        }, bytes).ToLocalChecked();
                    gin_helper::Dictionary requestObject(isolate, request.As<v8::Object>());
                    requestObject.Set("body", buffer);
                }
                v8::Local<v8::Value> arguments[] = {
                    v8::Integer::New(isolate, handlerId),
                    request,
                    v8::BigInt::NewFromUnsigned(isolate, requestId)
                };
                node::MakeCallback(isolate, context->Global(), callback,
                    3, arguments, { 0, 0 });
            });
    }


    v8::Local<v8::Object> getWrapper(v8::Isolate* isolate) override
    {
        return GetWrapper(isolate);
    }

    std::vector<RendererPrivilegedScheme> getPrivilegedSchemes() const override
    {
        base::AutoLock autoLock(m_lock);
        return m_privilegedSchemes;
    }

public:
    static gin_helper::WrapperInfo kWrapperInfo;
    static v8::Persistent<v8::Function> constructor;

    v8::Persistent<v8::Function> m_jsReciver;
    std::map<std::string, ProtocolInfo> m_schemeToHandleId;
    std::map<uint64_t, ProtocolCallbackInfo> m_pendingResponses;
    std::vector<RendererPrivilegedScheme> m_privilegedSchemes;
    uint64_t m_nextRequestId = 0;
    mutable base::Lock m_lock;
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
