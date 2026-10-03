// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ELECTRON_BROWSER_API_API_WEB_REQUEST_H_
#define ELECTRON_BROWSER_API_API_WEB_REQUEST_H_

#include <array>
#include <string>
#include <vector>

#include "runtime/electron/common/api/event_emitter.h"

namespace atom {
class BrokerRequestPipeline;
class EngineWebRequestPipeline;

class ApiWebRequest : public mate::EventEmitter<ApiWebRequest> {
public:
    static const char* kDefaultSessionName;
    static const char* kDefaultDir;

    static ApiWebRequest* create(v8::Isolate* isolate);

    static void init(v8::Isolate* isolate, v8::Local<v8::Object> target);

    bool matchesListener(const char* eventName, const std::string& url,
        const std::string& resourceType) const;

private:
    enum class HostMatch {
        kExact,
        kAny,
        kSubdomains,
    };

    struct UrlPattern {
        bool allUrls = false;
        bool anyScheme = false;
        HostMatch hostMatch = HostMatch::kExact;
        bool matchHost = true;
        std::string scheme;
        std::string host;
        int port = -1;
        std::string path;
        std::string decodedPath;
    };

    struct ListenerFilter {
        bool matchesAll = true;
        bool hasResourceTypes = false;
        std::vector<UrlPattern> urls;
        std::vector<UrlPattern> excludeUrls;
        std::vector<std::string> resourceTypes;
    };

    enum ListenerIndex {
        kBeforeRequest,
        kBeforeSendHeaders,
        kSendHeaders,
        kHeadersReceived,
        kBeforeRedirect,
        kResponseStarted,
        kCompleted,
        kErrorOccurred,
        kListenerCount,
    };

    static int listenerIndex(const char* eventName);
    void setListener(const v8::FunctionCallbackInfo<v8::Value>& args,
        const char* eventName, v8::Persistent<v8::Value>* listener);

    friend class ApiSession;
    friend class BrokerRequestPipeline;
    friend class EngineWebRequestPipeline;
    static void newFunction(const v8::FunctionCallbackInfo<v8::Value>& args);
    void onBeforeSendHeadersApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    void onSendHeadersApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    void onHeadersReceivedApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    void onResponseStartedApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    void onBeforeRedirectApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    void onCompletedApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    void onErrorOccurredApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    void onBeforeRequestApi(const v8::FunctionCallbackInfo<v8::Value>& args);

    ApiWebRequest(v8::Isolate* isolate, v8::Local<v8::Object> wrapper);
    v8::Persistent<v8::Object> m_liveSelf;

    v8::Persistent<v8::Value> m_beforeSendHeadersCb;
    v8::Persistent<v8::Value> m_sendHeadersCb;
    v8::Persistent<v8::Value> m_beforeRequestCb;
    v8::Persistent<v8::Value> m_headersReceivedCb;
    v8::Persistent<v8::Value> m_beforeRedirectCb;
    v8::Persistent<v8::Value> m_responseStartedCb;
    v8::Persistent<v8::Value> m_completedCb;
    v8::Persistent<v8::Value> m_errorOccurredCb;
    std::array<ListenerFilter, kListenerCount> m_listenerFilters;

public:
    static gin::WrapperInfo kWrapperInfo;
    static v8::Persistent<v8::Function> constructor;
};

} // atom namespace

#endif // ELECTRON_BROWSER_API_API_WEB_REQUEST_H_
