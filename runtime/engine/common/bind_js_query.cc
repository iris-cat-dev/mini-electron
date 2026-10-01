
#include "runtime/engine/browser/web_view_host.h"
#include "runtime/engine/common/bind_js_query.h"
#include "runtime/engine/common/live_id_detect.h"
#include "runtime/engine/common/thread_call.h"
#include "runtime/engine/resources/inject_script.h"
#include "runtime/engine/bindings/js_value.h"
#include "base/strings/utf_string_conversions.h"
#include <string>
#include <string.h>

extern bool g_isElectronMode;

namespace content {

void printCallstackIsolate(v8::Isolate* isolate)
{
    const v8::StackTrace::StackTraceOptions options = static_cast<v8::StackTrace::StackTraceOptions>(v8::StackTrace::kLineNumber | v8::StackTrace::kColumnOffset
        | v8::StackTrace::kScriptId | v8::StackTrace::kScriptNameOrSourceURL | v8::StackTrace::kFunctionName);

    int stackNum = 50;
    v8::HandleScope handleScope(isolate);
    v8::Local<v8::StackTrace> stackTrace(v8::StackTrace::CurrentStackTrace(isolate, stackNum, options));
    int count = stackTrace->GetFrameCount();

    char* output = (char*)malloc(0x100);
    sprintf(output, "content::printCallstack: %d\n", count);
    OutputDebugStringA(output);
    free(output);

    for (int i = 0; i < count; ++i) {
        v8::Local<v8::StackFrame> stackFrame = stackTrace->GetFrame(isolate, i);
        int line = stackFrame->GetLineNumber();
        v8::Local<v8::String> scriptName = stackFrame->GetScriptNameOrSourceURL();
        v8::Local<v8::String> funcName = stackFrame->GetFunctionName();

        std::string scriptNameWTF;
        std::string funcNameWTF;

        if (!scriptName.IsEmpty()) {
            v8::String::Utf8Value scriptNameUtf8(isolate, scriptName);
            scriptNameWTF = *scriptNameUtf8;
        }

        if (!funcName.IsEmpty()) {
            v8::String::Utf8Value funcNameUtf8(isolate, funcName);
            funcNameWTF = *funcNameUtf8;
        }
        std::vector<char> output2;
        output2.resize(1000);
        sprintf(&output2[0], "line:%d, [", line);
        OutputDebugStringA(&output2[0]);

        if (!scriptNameWTF.empty()) {
            OutputDebugStringA(scriptNameWTF.c_str());
        }
        OutputDebugStringA("] , [");

        if (!funcNameWTF.empty()) {
            OutputDebugStringA(funcNameWTF.c_str());
        }
        OutputDebugStringA("]\n");
    }
    OutputDebugStringA("\n");
}

void printCallstack()
{
    printCallstackIsolate(v8::Isolate::GetCurrent());
}

static void mini_electron_console_log(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    v8::Isolate* isolate = info.GetIsolate();
    v8::Local<v8::Context> context = isolate->GetCurrentContext();

    v8::Local<v8::Value> param0 = info[0];
    v8::MaybeLocal<v8::String> param0Maybe = param0->ToString(context);
    if (param0Maybe.IsEmpty())
        return;
    v8::Local<v8::String> param0V8String = param0Maybe.ToLocalChecked();

    v8::String::Utf8Value param0String(isolate, param0V8String);
    std::string str = "mini_electron_console_log, ";
    str += *param0String;
    str += "\n";

    if (std::string::npos != str.find("__writre__")) {
        HANDLE hFile = CreateFileA("G:\\test\\web_test\\ggzuhao\\1.js", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD fileSizeHigh;
        const DWORD bufferSize = ::GetFileSize(hFile, &fileSizeHigh);
        DWORD numberOfBytesRead = 0;
        std::vector<char> buffer;
        buffer.resize(bufferSize);
        BOOL b = ::ReadFile(hFile, buffer.data(), bufferSize, &numberOfBytesRead, nullptr);
        ::CloseHandle(hFile);

        v8::Local<v8::String> ret = v8::String::NewFromUtf8(isolate, buffer.data(), v8::NewStringType::kNormal, bufferSize).ToLocalChecked();
        info.GetReturnValue().Set(ret);
        return;
    }

    if (std::string::npos != str.find("__alert__")) {
        MessageBoxA(0, str.c_str(), 0, 0);
    }

    if (std::string::npos != str.find("__callstack__")) {
        printCallstackIsolate(isolate);
    }

    std::u16string strW = base::UTF8ToUTF16(str);
    OutputDebugStringW((const WCHAR*)strW.c_str());
}

// 新增加一个不定参数的版本
void BindJsQuery::jsCallback2(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    v8::Local<v8::External> ext = info.Data().As<v8::External>();
    BindJsQuery* self = (BindJsQuery*)ext->Value();

    v8::Isolate* isolate = info.GetIsolate();
    v8::HandleScope handleScope(isolate);
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Context::Scope contextScope(context);

    void* host = (void*)common::LiveIdDetect::getWebViewIds()->getPtrLocked(self->m_webviewId);
    if (!host)
        return;
    common::LiveIdDetect::getWebViewIds()->unlock(self->m_webviewId, self);

    std::vector<mini_electron::engine::JsValueBridge*>* jsValues = new std::vector<mini_electron::engine::JsValueBridge*>(info.Length());
    for (int i = 0; i < info.Length(); ++i) {
        v8::Local<v8::Value> argIt = info[i];
        mini_electron::engine::JsValueBridge* jsValue = mini_electron::engine::JsValueBridge::v8ValueToEngineValue(isolate, context, argIt);
        jsValues->at(i) = jsValue;
    }

    self->onJsQuery2InBlinkThread(self->m_frameToken, jsValues);
}

void BindJsQuery::onJsQuery2InBlinkThread(const blink::LocalFrameToken& frameToken, std::vector<mini_electron::engine::JsValueBridge*>* jsValues)
{
    int64_t id = m_webviewId;
    BindJsQuery* self = this;

    size_t frameId = (blink::LocalFrameToken::Hasher()(m_frameToken));

    content::ThreadCall::callUiThreadAsync(FROM_HERE, [self, id, jsValues] {
        void* unuse = common::LiveIdDetect::getWebViewIds()->getPtrLocked(id);
        if (!unuse) {
            delete jsValues;
            return;
        }

        std::vector<mini_electron_js_value> vals;
        for (size_t i = 0; i < jsValues->size(); ++i) {
            mini_electron::engine::JsValueBridge* v = jsValues->at(i);
            vals.push_back(v->getId());
        }

        QueryFn2* closure = self->m_closure2;
        if (closure)
            (*closure)((mini_electron_js_exec_state) nullptr, vals.data(), vals.size());

        for (size_t i = 0; i < jsValues->size(); ++i) {
            mini_electron::engine::JsValueBridge* v = jsValues->at(i);
            v->deref();
        }

        common::LiveIdDetect::getWebViewIds()->unlock(id, unuse);
    });
}

void BindJsQuery::jsCallback(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    v8::Local<v8::External> ext = info.Data().As<v8::External>();
    BindJsQuery* self = (BindJsQuery*)ext->Value();

    if (3 != info.Length())
        return;

    v8::Isolate* isolate = info.GetIsolate();
    v8::HandleScope handleScope(isolate);
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Context::Scope contextScope(context);

    v8::Local<v8::Value> arg0 = info[0];
    int customMsg = arg0->ToInt32(context).ToLocalChecked()->Value();

    v8::Local<v8::Value> arg1 = info[1];
    v8::Local<v8::String> str = arg1->ToString(context).ToLocalChecked();
    v8::String::Utf8Value strUtf8(isolate, str);

    v8::Local<v8::Value> arg2 = info[2];
    int queryId = arg2->ToInt32(context).ToLocalChecked()->Value();

    void* host = (void*)common::LiveIdDetect::getWebViewIds()->getPtrLocked(self->m_webviewId);
    if (!host)
        return;
    common::LiveIdDetect::getWebViewIds()->unlock(self->m_webviewId, self);

    std::string* requestString = new std::string(*strUtf8, strUtf8.length());
    self->onJsQueryInBlinkThread(self->m_frameToken, customMsg, requestString, queryId);
}

void BindJsQuery::onJsQueryInBlinkThread(const blink::LocalFrameToken& frameToken, int customMsg, const std::string* requestString, int queryId)
{
    int64_t id = m_webviewId;
    BindJsQuery* self = this;

    size_t frameId = (blink::LocalFrameToken::Hasher()(m_frameToken));

    std::pair<size_t, int>* idInfo = new std::pair<size_t, int>(frameId, queryId);

    content::ThreadCall::callUiThreadAsync(FROM_HERE, [self, id, customMsg, requestString, idInfo] {
        void* unuse = common::LiveIdDetect::getWebViewIds()->getPtrLocked(id);
        if (!unuse) {
            delete idInfo;
            delete requestString;
            return;
        }

        QueryFn* closure = self->m_closure;
        if (closure)
            (*closure)((mini_electron_js_exec_state) nullptr,
                static_cast<int64_t>(reinterpret_cast<intptr_t>(idInfo)),
                customMsg, requestString->c_str());
        delete requestString;

        common::LiveIdDetect::getWebViewIds()->unlock(id, unuse);
    });
}

static void bindGlobalFunction(v8::Local<v8::Context> context, const char* name, v8::FunctionCallback callback)
{
    v8::Isolate* isolate = context->GetIsolate();
    v8::HandleScope handleScope(isolate);
    v8::TryCatch block(isolate);
    v8::Context::Scope contextScope(context);

    v8::Local<v8::Object> object = context->Global();
    v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(isolate);

    // Set the function handler callback.
    tmpl->SetCallHandler(callback);

    // Retrieve the function object and set the name.
    v8::Local<v8::Function> func = tmpl->GetFunction(context).ToLocalChecked();
    v8::Local<v8::String> nameV8 = v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal, -1).ToLocalChecked();
    func->SetName(nameV8);

    object->Set(context, nameV8, func);
}

void bindOnWebWorkers(v8::Local<v8::Context> context)
{
    v8::Isolate* isolate = context->GetIsolate();
    v8::HandleScope handleScope(isolate);
    v8::Context::Scope contextScope(context);
    v8::MicrotasksScope miscrotaskScope(context, v8::MicrotasksScope::Type::kRunMicrotasks);
    bindGlobalFunction(context, "mini_electron_console_log", mini_electron_console_log);

    std::string injectScriptStr(kInjectScript);

    v8::Local<v8::String> code = v8::String::NewFromUtf8(isolate, injectScriptStr.c_str(), v8::NewStringType::kNormal, injectScriptStr.size()).ToLocalChecked();
    v8::ScriptOrigin origin(v8::String::NewFromUtf8(isolate, "bindOnWebWorkers.js", v8::NewStringType::kNormal, -1).ToLocalChecked());
    v8::ScriptCompiler::Source source(code, origin);
    v8::MaybeLocal<v8::Script> script = v8::ScriptCompiler::Compile(context, &source, v8::ScriptCompiler::kNoCompileOptions);
    script.ToLocalChecked()->Run(context);
}

void BindJsQuery::bindFun(v8::Local<v8::Context> context, QueryFn* queryFn, QueryFn2* queryFn2, WebViewHost* webview, const blink::LocalFrameToken& frameToken)
{
    int64_t webviewId = webview->getId();
    v8::Isolate* isolate = context->GetIsolate();
    v8::HandleScope handleScope(isolate);
    v8::Context::Scope contextScope(context);
    v8::Local<v8::Object> globalObj = context->Global();
    v8::MicrotasksScope microtasksScope(context, v8::MicrotasksScope::Type::kDoNotRunMicrotasks);

    if (!g_isElectronMode)
        bindGlobalFunction(context, "mini_electron_console_log", mini_electron_console_log);

    size_t frameId = (blink::LocalFrameToken::Hasher()(frameToken));
    std::string injectScriptStr(kInjectScript);

    mini_electron::engine::JsValueBridge* mini_electron_val = webview->runJsOnBlinkThreadImpl((mini_electron_web_frame_handle)frameId, 0, &injectScriptStr, false, nullptr, nullptr);
    if (mini_electron_val)
        mini_electron_val->deref();

    // 构建func参数
    BindJsQuery* self = new BindJsQuery();
    self->m_webviewId = webviewId;
    self->m_frameToken = frameToken;
    self->m_closure = queryFn;
    self->m_closure2 = queryFn2;
    v8::Local<v8::Value> dataLocal = v8::External::New(isolate, self);

    v8::Local<v8::FunctionTemplate> functionTemplate = v8::FunctionTemplate::New(isolate, jsCallback, dataLocal);
    v8::Local<v8::Function> v8Function;
    if (!functionTemplate->GetFunction(context).ToLocal(&v8Function))
        DebugBreak();

    v8::Local<v8::FunctionTemplate> functionTemplate2 = v8::FunctionTemplate::New(isolate, jsCallback2, dataLocal);
    v8::Local<v8::Function> v8Function2;
    if (!functionTemplate2->GetFunction(context).ToLocal(&v8Function2))
        DebugBreak();

    v8::Local<v8::Object> global = context->Global();
    v8::Local<v8::Value> windowVal
        = global->Get(context, v8::String::NewFromUtf8(isolate, "window", v8::NewStringType::kNormal, -1).ToLocalChecked()).ToLocalChecked();
    v8::Local<v8::Object> windowObj = windowVal->ToObject(context).ToLocalChecked();

    v8::Local<v8::String> setEngineQueryStr = v8::String::NewFromUtf8(isolate, "__setMiniElectronQuery__", v8::NewStringType::kNormal, -1).ToLocalChecked();
    v8::Local<v8::Value> setEngineQueryStrValue = windowObj->Get(context, setEngineQueryStr).ToLocalChecked();
    if (!setEngineQueryStrValue->IsFunction()) // https://www.hao123.com/?tn=48021271_79_hao_pg 这网页不知道为啥有时绑定失败
        return;

    v8::Function* setEngineQueryFunc = v8::Function::Cast(*setEngineQueryStrValue);

    v8::Local<v8::Value> argv[2] = { v8Function, v8Function2 };
    setEngineQueryFunc->Call(context, v8::Undefined(isolate), 2, argv);
}

}