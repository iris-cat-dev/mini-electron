// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "runtime/electron/browser/api/web_request.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>
#include <string_view>
#include <utility>

#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"

#include "runtime/electron/browser/api/session.h"
#include "runtime/electron/node_bindings.h"
#include "runtime/electron/common/node_register_help.h"
#include "runtime/electron/common/api/event_emitter.h"
#include "runtime/electron/common/file_util.h"
#include "runtime/electron/common/string_util.h"
#include "runtime/electron/common/gin_helper/wrappable.h"
#include "runtime/electron/common/gin_helper/object_template_builder.h"
#include "runtime/electron/common/gin_helper/dictionary.h"
#include "runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "runtime/electron/common/gin_helper/public/wrapper_info.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "third_party/libuv/include/uv.h"
#include <vector>
#include "url/gurl.h"
#include "url/url_util.h"

namespace atom {

ApiWebRequest::ApiWebRequest(v8::Isolate* isolate, v8::Local<v8::Object> wrapper)
{
    gin_helper::Wrappable<ApiWebRequest>::InitWith(isolate, wrapper);
}

ApiWebRequest* ApiWebRequest::create(v8::Isolate* isolate)
{
    //     const int argc = 1;
    //     v8::Local<v8::Value> argv[argc] = { v8::Null(isolate) };
    v8::Local<v8::Function> constructorFunction = v8::Local<v8::Function>::New(isolate, constructor);
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    //v8::MaybeLocal<v8::Object> obj = constructorFunction->NewInstance(context, argc, argv);
    v8::MaybeLocal<v8::Object> obj = constructorFunction->NewInstance(context);
    v8::Local<v8::Object> objV8 = obj.ToLocalChecked();

    ApiWebRequest* self = (ApiWebRequest*)WrappableBase::GetNativePtr(objV8, &kWrapperInfo);
    self->m_liveSelf.Reset(isolate, objV8);

    return self;
}

void ApiWebRequest::init(v8::Isolate* isolate, v8::Local<v8::Object> target)
{
    const char* className = "WebRequest";
    v8::Local<v8::FunctionTemplate> funTempl = v8::FunctionTemplate::New(isolate, newFunction);
    v8::Local<v8::Context> context = isolate->GetCurrentContext();

    funTempl->SetClassName(v8::String::NewFromUtf8(isolate, className).ToLocalChecked());
    gin_helper::ObjectTemplateBuilder builder(isolate, funTempl->InstanceTemplate());
    builder.SetMethod("onBeforeSendHeaders", &ApiWebRequest::onBeforeSendHeadersApi);
    builder.SetMethod("onSendHeaders", &ApiWebRequest::onSendHeadersApi);
    builder.SetMethod("onBeforeRedirect", &ApiWebRequest::onBeforeRedirectApi);
    builder.SetMethod("onHeadersReceived", &ApiWebRequest::onHeadersReceivedApi);
    builder.SetMethod("onResponseStarted", &ApiWebRequest::onResponseStartedApi);
    builder.SetMethod("onCompleted", &ApiWebRequest::onCompletedApi);
    builder.SetMethod("onErrorOccurred", &ApiWebRequest::onErrorOccurredApi);
    builder.SetMethod("onBeforeRequest", &ApiWebRequest::onBeforeRequestApi);

    v8::Local<v8::Function> fun = funTempl->GetFunction(context).ToLocalChecked();
    //     gin_helper::Dictionary sessionClass(isolate, fun);
    //     sessionClass.SetMethod("fromPartition", &ApiWebRequest::fromPartitionApi);

    constructor.Reset(isolate, fun);
    target->Set(context, v8::String::NewFromUtf8(isolate, className).ToLocalChecked(), fun);
}

void ApiWebRequest::newFunction(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    if (args.IsConstructCall()) {
        new ApiWebRequest(isolate, args.This());
        args.GetReturnValue().Set(args.This());
        return;
    }
}

namespace {

void ThrowTypeError(v8::Isolate* isolate, const std::string& message)
{
    isolate->ThrowException(v8::Exception::TypeError(
        gin_helper::StringToV8(isolate, message)));
}

bool MatchWithAsterisk(std::string_view value, std::string_view pattern)
{
    size_t valueIndex = 0;
    size_t patternIndex = 0;
    size_t wildcardIndex = std::string_view::npos;
    size_t wildcardValueIndex = 0;
    while (valueIndex < value.size()) {
        if (patternIndex < pattern.size()
            && pattern[patternIndex] == value[valueIndex]) {
            ++valueIndex;
            ++patternIndex;
        } else if (patternIndex < pattern.size()
            && pattern[patternIndex] == '*') {
            wildcardIndex = patternIndex++;
            wildcardValueIndex = valueIndex;
        } else if (wildcardIndex != std::string_view::npos) {
            patternIndex = wildcardIndex + 1;
            valueIndex = ++wildcardValueIndex;
        } else {
            return false;
        }
    }
    while (patternIndex < pattern.size() && pattern[patternIndex] == '*')
        ++patternIndex;
    return patternIndex == pattern.size();
}

} // namespace

int ApiWebRequest::listenerIndex(const char* eventName)
{
    static constexpr const char* kEventNames[kListenerCount] = {
        "onBeforeRequest",
        "onBeforeSendHeaders",
        "onSendHeaders",
        "onHeadersReceived",
        "onBeforeRedirect",
        "onResponseStarted",
        "onCompleted",
        "onErrorOccurred",
    };
    if (!eventName)
        return -1;
    for (int i = 0; i < kListenerCount; ++i) {
        if (std::strcmp(eventName, kEventNames[i]) == 0)
            return i;
    }
    return -1;
}

void ApiWebRequest::setListener(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    const char* eventName, v8::Persistent<v8::Value>* listener)
{
    v8::Isolate* isolate = args.GetIsolate();
    const int index = listenerIndex(eventName);
    if (index < 0)
        return;
    if (args.Length() < 1 || args.Length() > 2) {
        ThrowTypeError(isolate,
            std::string(eventName) + " requires a listener and optional filter");
        return;
    }

    v8::Local<v8::Value> candidate = args[args.Length() - 1];
    if (candidate->IsNull()) {
        listener->Reset();
        m_listenerFilters[index] = ListenerFilter();
        return;
    }
    if (!candidate->IsFunction()) {
        ThrowTypeError(isolate,
            std::string(eventName) + " listener must be a function or null");
        return;
    }

    ListenerFilter filter;
    if (args.Length() == 2) {
        if (!args[0]->IsObject() || args[0]->IsNull()
            || args[0]->IsArray()) {
            ThrowTypeError(isolate,
                std::string(eventName) + " filter must be an object");
            return;
        }
        filter.matchesAll = false;
        gin_helper::Dictionary dictionary(isolate, args[0].As<v8::Object>());
        std::vector<std::string> urls;
        if (!dictionary.Has("urls")
            || !dictionary.Get("urls", &urls)) {
            ThrowTypeError(isolate,
                std::string(eventName)
                    + " filter.urls must be an array of URL patterns");
            return;
        }
        std::vector<std::string> excludeUrls;
        if (dictionary.Has("excludeUrls")
            && !dictionary.Get("excludeUrls", &excludeUrls)) {
            ThrowTypeError(isolate,
                std::string(eventName)
                    + " filter.excludeUrls must be an array of URL patterns");
            return;
        }
        std::vector<std::string> resourceTypes;
        if (dictionary.Has("types")
            && !dictionary.Get("types", &resourceTypes)) {
            ThrowTypeError(isolate,
                std::string(eventName)
                    + " filter.types must be an array of resource types");
            return;
        }
        filter.hasResourceTypes = dictionary.Has("types");

        auto parsePattern = [](const std::string& value, UrlPattern* out) {
            if (value == "<all_urls>") {
                out->allUrls = true;
                return true;
            }
            const size_t separator = value.find(':');
            if (separator == std::string::npos || separator == 0)
                return false;
            out->scheme = base::ToLowerASCII(value.substr(0, separator));
            out->anyScheme = out->scheme == "*";
            if (!out->anyScheme) {
                if (!base::IsStringASCII(out->scheme)
                    || !std::isalpha(
                        static_cast<unsigned char>(out->scheme[0]))) {
                    return false;
                }
                for (char character : out->scheme) {
                    if (!std::isalnum(static_cast<unsigned char>(character))
                        && character != '+' && character != '-'
                        && character != '.') {
                        return false;
                    }
                }
            }

            const bool hasStandardSeparator =
                value.compare(separator, 3, "://") == 0;
            const bool isStandardScheme =
                out->anyScheme || url::IsStandardScheme(out->scheme);
            if (hasStandardSeparator != isStandardScheme)
                return false;
            const size_t authorityStart =
                separator + (hasStandardSeparator ? 3 : 1);
            if (!hasStandardSeparator) {
                out->matchHost = false;
                out->path = value.substr(authorityStart);
                if (out->path.empty())
                    return false;
                out->decodedPath = base::UnescapeURLComponent(
                    out->path, base::UnescapeRule::NORMAL);
                return true;
            }

            if (authorityStart == value.size())
                return false;
            size_t pathStart = value.find('/', authorityStart);
            if (out->scheme == "file" && pathStart == std::string::npos) {
                out->path = "/" + value.substr(authorityStart);
            } else {
                if (pathStart == std::string::npos)
                    return false;
                out->path = value.substr(pathStart);
            }
            if (out->path.empty())
                return false;
            out->decodedPath = base::UnescapeURLComponent(
                out->path, base::UnescapeRule::NORMAL);
            if (out->scheme == "file") {
                out->matchHost = false;
                return true;
            }

            std::string authority =
                value.substr(authorityStart, pathStart - authorityStart);
            if (authority.empty() || authority.find('@') != std::string::npos)
                return false;
            std::string host = authority;
            std::string port;
            if (authority.front() == '[') {
                const size_t closingBracket = authority.find(']');
                if (closingBracket == std::string::npos)
                    return false;
                host = authority.substr(0, closingBracket + 1);
                if (closingBracket + 1 < authority.size()) {
                    if (authority[closingBracket + 1] != ':')
                        return false;
                    port = authority.substr(closingBracket + 2);
                    if (port.empty())
                        return false;
                }
            } else {
                const size_t portSeparator = authority.find(':');
                if (portSeparator != std::string::npos) {
                    if (authority.find(':', portSeparator + 1)
                        != std::string::npos) {
                        return false;
                    }
                    host = authority.substr(0, portSeparator);
                    port = authority.substr(portSeparator + 1);
                    if (port.empty())
                        return false;
                }
            }
            if (host == "*") {
                out->hostMatch = HostMatch::kAny;
            } else {
                if (base::StartsWith(host, "*.")) {
                    out->hostMatch = HostMatch::kSubdomains;
                    host.erase(0, 2);
                }
                if (host.empty() || host.find('*') != std::string::npos)
                    return false;
                GURL canonical("http://" + host + "/");
                if (!canonical.is_valid() || canonical.host().empty())
                    return false;
                out->host = canonical.host();
                while (!out->host.empty() && out->host.back() == '.')
                    out->host.pop_back();
                if (out->host.empty())
                    return false;
            }
            if (!port.empty() && port != "*") {
                if (!base::StringToInt(port, &out->port)
                    || out->port < 0 || out->port > 65535) {
                    return false;
                }
            }
            return true;
        };

        auto parsePatterns = [&](const std::vector<std::string>& inputs,
                                 std::vector<UrlPattern>* outputs,
                                 const char* property) {
            outputs->reserve(inputs.size());
            for (const std::string& input : inputs) {
                UrlPattern pattern;
                if (!parsePattern(input, &pattern)) {
                    ThrowTypeError(isolate, std::string(eventName)
                        + " filter." + property
                        + " contains an invalid URL pattern: " + input);
                    return false;
                }
                outputs->push_back(std::move(pattern));
            }
            return true;
        };
        if (!parsePatterns(urls, &filter.urls, "urls")
            || !parsePatterns(
                excludeUrls, &filter.excludeUrls, "excludeUrls")) {
            return;
        }

        static constexpr const char* kResourceTypes[] = {
            "mainFrame", "subFrame", "stylesheet", "script", "image",
            "font", "object", "xhr", "ping", "cspReport", "media",
            "webSocket", "other",
        };
        for (const std::string& resourceType : resourceTypes) {
            if (std::find(std::begin(kResourceTypes),
                    std::end(kResourceTypes), resourceType)
                == std::end(kResourceTypes)) {
                ThrowTypeError(isolate, std::string(eventName)
                    + " filter.types contains an invalid resource type: "
                    + resourceType);
                return;
            }
        }
        filter.resourceTypes = std::move(resourceTypes);
    }

    listener->Reset(isolate, candidate);
    m_listenerFilters[index] = std::move(filter);
}

bool ApiWebRequest::matchesListener(const char* eventName,
    const std::string& url, const std::string& resourceType) const
{
    const int index = listenerIndex(eventName);
    if (index < 0)
        return false;
    const ListenerFilter& filter = m_listenerFilters[index];
    if (filter.matchesAll)
        return true;
    if (filter.hasResourceTypes
        && std::find(filter.resourceTypes.begin(), filter.resourceTypes.end(),
               resourceType) == filter.resourceTypes.end()) {
        return false;
    }

    GURL parsed(url);
    auto matchesPattern = [&parsed](const UrlPattern& pattern) {
        if (!parsed.is_valid())
            return false;
        if (pattern.allUrls)
            return true;
        if (pattern.anyScheme) {
            if (!parsed.SchemeIs("http") && !parsed.SchemeIs("https"))
                return false;
        } else if (parsed.scheme() != pattern.scheme) {
            return false;
        }
        if (pattern.matchHost) {
            std::string_view host = parsed.host_piece();
            while (!host.empty() && host.back() == '.')
                host.remove_suffix(1);
            if (pattern.hostMatch == HostMatch::kExact
                && host != pattern.host) {
                return false;
            }
            if (pattern.hostMatch == HostMatch::kSubdomains
                && host != pattern.host
                && (host.size() <= pattern.host.size()
                    || !base::EndsWith(host, pattern.host)
                    || host[host.size() - pattern.host.size() - 1] != '.')) {
                return false;
            }
            if (pattern.port >= 0
                && parsed.EffectiveIntPort() != pattern.port) {
                return false;
            }
        }
        const std::string requestPath = parsed.PathForRequest();
        if (MatchWithAsterisk(requestPath, pattern.path))
            return true;
        if (requestPath.find('%') == std::string::npos
            && pattern.decodedPath == pattern.path) {
            return false;
        }
        return MatchWithAsterisk(
            base::UnescapeURLComponent(
                requestPath, base::UnescapeRule::NORMAL),
            pattern.decodedPath);
    };
    if (!std::any_of(filter.urls.begin(), filter.urls.end(), matchesPattern))
        return false;
    return !std::any_of(filter.excludeUrls.begin(),
        filter.excludeUrls.end(), matchesPattern);
}

void ApiWebRequest::onBeforeSendHeadersApi(
    const v8::FunctionCallbackInfo<v8::Value>& args)
{
    setListener(args, "onBeforeSendHeaders", &m_beforeSendHeadersCb);
}

void ApiWebRequest::onSendHeadersApi(
    const v8::FunctionCallbackInfo<v8::Value>& args)
{
    setListener(args, "onSendHeaders", &m_sendHeadersCb);
}

void ApiWebRequest::onHeadersReceivedApi(
    const v8::FunctionCallbackInfo<v8::Value>& args)
{
    setListener(args, "onHeadersReceived", &m_headersReceivedCb);
}

void ApiWebRequest::onResponseStartedApi(
    const v8::FunctionCallbackInfo<v8::Value>& args)
{
    setListener(args, "onResponseStarted", &m_responseStartedCb);
}

void ApiWebRequest::onBeforeRedirectApi(
    const v8::FunctionCallbackInfo<v8::Value>& args)
{
    setListener(args, "onBeforeRedirect", &m_beforeRedirectCb);
}

void ApiWebRequest::onCompletedApi(
    const v8::FunctionCallbackInfo<v8::Value>& args)
{
    setListener(args, "onCompleted", &m_completedCb);
}

void ApiWebRequest::onErrorOccurredApi(
    const v8::FunctionCallbackInfo<v8::Value>& args)
{
    setListener(args, "onErrorOccurred", &m_errorOccurredCb);
}

void ApiWebRequest::onBeforeRequestApi(
    const v8::FunctionCallbackInfo<v8::Value>& args)
{
    setListener(args, "onBeforeRequest", &m_beforeRequestCb);
}

gin_helper::WrapperInfo ApiWebRequest::kWrapperInfo = { gin_helper::GinEmbedder::kEmbedderNativeGin };
v8::Persistent<v8::Function> ApiWebRequest::constructor;

void initializeBrowserWebRequestApi(v8::Local<v8::Object> exports, v8::Local<v8::Value> unused, v8::Local<v8::Context> context, void* priv)
{
    ApiWebRequest::init(context->GetIsolate(), exports);
}

static const char BrowserWebRequestName[] = "console.log('BrowserWebRequestNative');;";
static NodeNative BrowserWebRequestNative { "WebRequest", BrowserWebRequestName, sizeof(BrowserWebRequestName) - 1 };

NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_MANUAL(electron_browser_webrequest, initializeBrowserWebRequestApi, &BrowserWebRequestNative)

} // atom namespace