
#ifndef content_BindJsQuery_h
#define content_BindJsQuery_h

#include "runtime/engine/public/engine_api.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include <functional>
#include "v8.h"

namespace content {

class WebViewHost;

class BindJsQuery /*: public jsData*/ {
public:
    typedef std::function<void(mini_electron_js_exec_state es, int64_t idInfo, int customMsg, const utf8* request)> QueryFn;
    typedef std::function<void(mini_electron_js_exec_state es, const mini_electron_js_value* val, int count)> QueryFn2;


    static void bindFun(v8::Local<v8::Context> context, QueryFn* queryFn, QueryFn2* queryFn2, WebViewHost* webview, const blink::LocalFrameToken& frameToken);

private:
    void onJsQueryInBlinkThread(const blink::LocalFrameToken& m_frameToken, int customMsg, const std::string* request, int queryId);
    void onJsQuery2InBlinkThread(const blink::LocalFrameToken& frameToken, std::vector<mini_electron::engine::JsValueBridge*>* jsValues);
    static void jsCallback(const v8::FunctionCallbackInfo<v8::Value>& info);
    static void jsCallback2(const v8::FunctionCallbackInfo<v8::Value>& info);

    QueryFn* m_closure = nullptr;
    QueryFn2* m_closure2 = nullptr;
    int64_t m_webviewId = 0;
    blink::LocalFrameToken m_frameToken;
};

}

#endif // content_BindJsQuery_h