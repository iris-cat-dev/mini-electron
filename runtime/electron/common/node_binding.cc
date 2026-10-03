
#include "runtime/electron/common/node_binding.h"

#include "runtime/electron/node_bindings.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libuv/include/uv.h"
#include "runtime/electron/common/string_util.h"
#include "runtime/electron/common/atom_version.h"
#include "runtime/electron/common/chrome_version.h"
#include "runtime/electron/common/atom_command_line.h"
#include "runtime/electron/common/base_macros.h"
#include "runtime/electron/common/api/event_emitter_caller.h"
//#include "electron/common/TracingControllerImpl.h"
#include "runtime/electron/common/locker.h"
#include "runtime/electron/common/embedded_resources.h"
#include "runtime/electron/common/node_thread.h"
//#include "base/task/thread_pool/initialization_util.h"
#include "gin/v8_platform_page_allocator.h"
#include "gin/dictionary.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/path_service.h"
#include <xstring>
#include <vector>
#include <memory>
#include <shlwapi.h>

namespace content {
void* v8ContextToEngineWebFrameHandle(v8::Local<v8::Context> context);
void printCallstack();
}

namespace atom {

bool isMainThread();

namespace {

void crash(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    DebugBreak();
}

void hang(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    for (;;) {
        ::Sleep(1000);
    };
}

void getProcessMemoryInfo(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    v8::Isolate* isolate = info.GetIsolate();
    //std::unique_ptr<base::ProcessMetrics> metrics(base::ProcessMetrics::CreateCurrentProcessMetrics());

    gin::Dictionary dict = gin::Dictionary::CreateEmpty(isolate);
    dict.Set("workingSetSize", /*static_cast<double>(metrics->GetWorkingSetSize() >> 10)*/ 1000);
    dict.Set("peakWorkingSetSize", /*static_cast<double>(metrics->GetPeakWorkingSetSize() >> 10)*/ 1000);

    //size_t private_bytes, shared_bytes;
    //if (metrics->GetMemoryBytes(&private_bytes, &shared_bytes)) {
    dict.Set("privateBytes", /*static_cast<double>(private_bytes >> 10)*/ 1000);
    dict.Set("sharedBytes", /*static_cast<double>(shared_bytes >> 10)*/ 1000);
    //}

    info.GetReturnValue().Set(gin::Converter<gin::Dictionary>::ToV8(isolate, dict));
}

void getSystemMemoryInfo(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    v8::Isolate* isolate = info.GetIsolate();
    //     base::SystemMemoryInfoKB mem_info;
    //     if (!base::GetSystemMemoryInfo(&mem_info)) {
    //         args->ThrowError("Unable to retrieve system memory information");
    //         return v8::Undefined(isolate);
    //     }

    gin::Dictionary dict = gin::Dictionary::CreateEmpty(isolate);
    dict.Set("total", /*mem_info.total*/ 10);
    dict.Set("free", /*mem_info.free*/ 10);

    // NB: These return bogus values on macOS
#if !defined(OS_MACOSX)
    dict.Set("swapTotal", /*mem_info.swap_total*/ 10);
    dict.Set("swapFree", /*mem_info.swap_free*/ 10);
#endif

    info.GetReturnValue().Set(gin::Converter<gin::Dictionary>::ToV8(isolate, dict));
}

// Called when there is a fatal error in V8, we just crash the process here so
// we can get the stack trace.
void fatalErrorCallback(const char* location, const char* message)
{
    //crash(info);
    DebugBreak();
}

void log(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    //std::cout << message << std::flush;
    crash(info);
}

void getSystemVersion(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    OSVERSIONINFOEXW os = { 0 };
    os.dwOSVersionInfoSize = sizeof(os);
    ::GetVersionEx(reinterpret_cast<LPOSVERSIONINFOW>(&os));

    std::vector<char> buf(201);
    sprintf(buf.data(), "%d.%d.%d\n", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);

    v8::Local<v8::String> result = v8::String::NewFromUtf8(args.GetIsolate(), buf.data()).ToLocalChecked();
    args.GetReturnValue().Set(result);
}

} // namespace

NodeBindings::NodeBindings(bool isBrowser)
    : m_isBrowser(isBrowser)
    , m_uvLoop(nullptr)
    , m_uvEnv(nullptr)
{
}

NodeBindings::~NodeBindings()
{
    if (m_uvEnv) { // 渲染进程里，这个有可能在WebContents::onWillReleaseScriptContextCallback就被销毁了
        nodeDeleteNodeEnvironment(m_uvEnv);
    }

    if (m_isolateData)
        node::FreeIsolateData(m_isolateData);
}

std::wstring getResourcesPath(const std::wstring& name)
{
    static const std::wstring root = [] {
        base::FilePath executable;
        base::PathService::Get(base::FILE_EXE, &executable);
        const base::FilePath directory = executable.DirName();
        const base::FilePath installed = directory.AppendASCII("resources").AppendASCII("mini-electron").AppendASCII("lib");
        if (base::DirectoryExists(installed))
            return installed.value() + L"\\";
        const std::wstring embedded = directory.AppendASCII(kEmbeddedResourcePrefix).AppendASCII("lib").value() + L"\\";
        setEmbeddedResourcePath(base::WideToUTF8(embedded));
        return embedded;
    }();
    return root + name;
}

void loadNodeScriptFromRes(void* path)
{
    NodeNative* nativePath = (NodeNative*)path;
    std::wstring sourceW;
    for (size_t i = 0; i < nativePath->sourceLen; ++i)
        sourceW += nativePath->source[i];

    if (L'.' == sourceW[0] && L'/' == sourceW[1])
        sourceW = sourceW.substr(2, sourceW.size() - 2);
    sourceW += L".js";
    sourceW = getResourcesPath(sourceW);

    WIN32_FILE_ATTRIBUTE_DATA attrs;
    if (::GetFileAttributesExW(sourceW.c_str(), GetFileExInfoStandard, &attrs) == 0)
        return;

    HANDLE fileHandle = ::CreateFileW(sourceW.c_str(), FILE_READ_DATA, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
    if (fileHandle == INVALID_HANDLE_VALUE)
        return;

    std::vector<char>* buffer = new std::vector<char>(); // 内存泄漏
    buffer->resize(attrs.nFileSizeLow);

    DWORD bytesRead;
    int retval = ::ReadFile(fileHandle, &buffer->at(0), attrs.nFileSizeLow, &bytesRead, 0);
    ::CloseHandle(fileHandle);

    if (retval == 0 || bytesRead != attrs.nFileSizeLow)
        return;

    nativePath->source = &buffer->at(0);
    nativePath->sourceLen = bytesRead;
}

// Convert the given vector to an array of C-strings. The strings in the
// returned vector are only guaranteed valid so long as the vector of strings
// is not modified.
std::unique_ptr<const char*[]> stringVectorToArgArray(const std::vector<std::string>& vector)
{
    std::unique_ptr<const char*[]> argsArray(new const char*[vector.size()]);
    for (size_t i = 0; i < vector.size(); ++i) {
        argsArray[i] = vector[i].c_str();
    }
    return argsArray;
}


static void MethodCallbackWrap(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    v8::Local<v8::External> v8Holder;
    gin::ConvertFromV8(info.GetIsolate(), info.Data(), &v8Holder);
    std::function<void(const v8::FunctionCallbackInfo<v8::Value>&)>* func = (std::function<void(const v8::FunctionCallbackInfo<v8::Value>&)>*)v8Holder->Value();
    (*func)(info);
}

static void bindMethod(
    v8::Isolate* isolate, v8::Local<v8::Object> object, const char* name, const std::function<void(const v8::FunctionCallbackInfo<v8::Value>&)>&& callback)
{
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Local<v8::External> wrap = v8::External::New(isolate, new std::function<void(const v8::FunctionCallbackInfo<v8::Value>&)>(callback));
    v8::Local<v8::Function> func = v8::FunctionTemplate::New(isolate, MethodCallbackWrap, wrap)->GetFunction(context).ToLocalChecked();

    // kInternalized strings are created in the old space.
    const v8::NewStringType type = v8::NewStringType::kInternalized;
    v8::Local<v8::String> nameString = v8::String::NewFromUtf8(isolate, name, type).ToLocalChecked();
    object->Set(context, nameString, func);
    func->SetName(nameString); // NODE_SET_METHOD() compatibility.
}

static void isInElectronEnv(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    v8::Isolate* isolate = info.GetIsolate();
    info.GetReturnValue().Set(v8::Boolean::New(isolate, true).As<v8::Value>());
}

void NodeBindings::bindFunction(v8::Isolate* isolate, v8::Local<v8::Object> object)
{
    bindMethod(isolate, object, "crash", &crash);
    bindMethod(isolate, object, "hang", &hang);
    bindMethod(isolate, object, "log", &log);
    bindMethod(isolate, object, "getProcessMemoryInfo", &getProcessMemoryInfo);
    bindMethod(isolate, object, "getSystemMemoryInfo", &getSystemMemoryInfo);
#if defined(OS_POSIX)
    bindMethod(isolate, object, "setFdLimit", &base::SetFdLimit);
#endif

#if defined(MAS_BUILD)
    bindMethod(isolate, object, "mas", true);
#endif
    bindMethod(isolate, object, "getSystemVersion", getSystemVersion); // M:\chromium\electron14\electron\shell\common\api\electron_bindings.cc

    gin::Dictionary processObject = gin::Dictionary(isolate, object);
    processObject.Set("sandboxed", false);
    if (!m_processObjInfo.isBrowserProcess)
        processObject.Set("contextIsolated", m_processObjInfo.isContextIsolated);

    // processObject == dict
}

// third_party\libnode\src\node_process_object.cc的PatchProcessObject会重新设置versions，所以我们这个函数的时机也要注意一下
void patchProcessObject(v8::Local<v8::Object> object)
{
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    gin::Dictionary versions = gin::Dictionary::CreateEmpty(isolate);
    gin::Dictionary processObject = gin::Dictionary(isolate, object);
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    v8::Context::Scope contextScope(context);

    if (processObject.Get("versions", &versions)) {
        versions.DefineOwnProperty("chrome", std::string(CHROME_VERSION_STRING));
        versions.DefineOwnProperty("electron", std::string(ATOM_VERSION_STRING));
        versions.DefineOwnProperty("miniElectron", std::string(MINI_ELECTRON_VERSION_STRING));

        base::FilePath exePath;
        base::PathService::Get(base::BasePathKey::FILE_EXE, &exePath);
        versions.DefineOwnProperty("execPath", exePath.AsUTF8Unsafe());

        object->Delete(context, gin::StringToV8(isolate, "versions"));
        processObject.DefineOwnProperty("versions", versions);
    }
}

static void mini_electron_get_v8_name_id_hash(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    if (info.Length() != 1) {
        info.GetReturnValue().Set(0);
        return;
    }
    v8::Local<v8::Value> arg0 = info[0];
    if (!arg0->IsName()) {
        info.GetReturnValue().Set(0);
        return;
    }
    v8::Local<v8::Name> name = arg0.As<v8::Name>();
    info.GetReturnValue().Set(name->GetIdentityHash());
}

struct MiniElectronConsoleLogInfo {
    MiniElectronConsoleLogInfo(bool isMainNode)
        : m_isMainNode(isMainNode)
    {
    }

    bool getIsMainNode() const
    {
        return m_isMainNode;
    };

private:
    bool m_isMainNode;
};

static void mini_electron_console_log(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    v8::Isolate* isolate = info.GetIsolate();
    v8::Local<v8::Context> context = isolate->GetCurrentContext();

    MiniElectronConsoleLogInfo* consoleLogInfo = static_cast<MiniElectronConsoleLogInfo*>(v8::External::Cast(*info.Data())->Value());

    v8::Local<v8::Value> param0 = info[0];
    v8::MaybeLocal<v8::String> param0Maybe = param0->ToString(context);
    //v8::Local<v8::String> param0V8String = param0->ToString(isolate);
    if (param0Maybe.IsEmpty())
        return;
    v8::Local<v8::String> param0V8String = param0Maybe.ToLocalChecked();

    v8::String::Utf8Value param0String(isolate, param0V8String);
    char temp[150] = { 0 };
    const char* strTemp = (consoleLogInfo->getIsMainNode() ? ("mini_electron_console_log, Main(%p):") : ("mini_electron_console_log, Render(%p):"));
    void* frameId = consoleLogInfo->getIsMainNode() ? nullptr : content::v8ContextToEngineWebFrameHandle(context);
    sprintf(temp, strTemp, frameId);

    std::string str = temp;
    str += *param0String;
    str += "\n";

    if (std::string::npos != str.find("__alert__"))
        MessageBoxA(0, str.c_str(), 0, 0);

    if (std::string::npos != str.find("__callstack__")) {
        content::printCallstack();
    }

    std::wstring strW = StringUtil::UTF8ToUTF16(str);
//     if (std::wstring::npos != strW.find(L"下载文件"))
//         OutputDebugStringA("");

    OutputDebugStringW(strW.c_str());
}

static void addFunction(v8::Local<v8::Context> context, const char* name, v8::FunctionCallback callback, bool isMainNode)
{
    v8::Isolate* isolate = context->GetIsolate();
    //     if (!isolate->InContext())
    //         return;
    v8::HandleScope handleScope(isolate);
    v8::TryCatch block(isolate);
    v8::Context::Scope contextScope(context);

    v8::Local<v8::Object> object = context->Global();
    v8::Local<v8::FunctionTemplate> tmpl = v8::FunctionTemplate::New(isolate);
    v8::Local<v8::Value> data = v8::External::New(isolate, new MiniElectronConsoleLogInfo(isMainNode)); // TODO: 内存泄露

    // Set the function handler callback.
    tmpl->SetCallHandler(callback, data);

    // Retrieve the function object and set the name.
    v8::Local<v8::Function> func = tmpl->GetFunction(context).ToLocalChecked();
    if (func.IsEmpty())
        return;

    v8::MaybeLocal<v8::String> nameV8 = v8::String::NewFromUtf8(isolate, name, v8::NewStringType::kNormal, -1);
    if (nameV8.IsEmpty())
        return;
    v8::Local<v8::String> nameV8Local = nameV8.ToLocalChecked();
    func->SetName(nameV8Local);

    object->Set(context, nameV8Local, func);
}

void bindEngineConsoleLog(v8::Local<v8::Context> context)
{
    bool isBrowserProcess = isMainThread();
    addFunction(context, "mini_electron_console_log", mini_electron_console_log, isBrowserProcess);
    addFunction(context, "mini_electron_get_v8_name_id_hash", mini_electron_get_v8_name_id_hash, isBrowserProcess);
    addFunction(context, "_isInElectronEnv", isInElectronEnv, isBrowserProcess);
}


} // atom