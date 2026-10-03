
#define _CRT_NON_CONFORMING_SWPRINTFS

#include "runtime/electron/browser/api/app.h"

#include "runtime/electron/common/node_register_help.h"
#include "runtime/electron/common/atom_command_line.h"
#include "runtime/electron/common/string_util.h"
#include "runtime/electron/common/api/event_emitter.h"
#include "runtime/electron/common/icon_util.h"
#include "runtime/electron/common/gin_helper/promise.h"
#include "runtime/electron/common/gin_helper/converter_util.h"
#include "runtime/electron/common/gin_helper/object_template_builder.h"
#include "runtime/electron/common/gin_helper/arguments.h"
#include "runtime/electron/common/gin_helper/wrappable.h"
#include "runtime/electron/common/gin_helper/dictionary.h"
#include "runtime/electron/browser/api/session.h"
#include "runtime/electron/browser/api/window_list.h"
#include "runtime/electron/browser/api/window_interface.h"
#include "runtime/electron/browser/api/app_browser.h"
#include "runtime/electron/node_bindings.h"
#include "base/values.h"
#include "base/base_paths.h"
#include "base/path_service.h"
#include "base/command_line.h"
#include "base/process/launch.h"
#include "base/win/scoped_handle.h"
#include "base/json/json_writer.h"
#include "base/json/json_reader.h"
#include "base/files/file_util.h"
#include "base/files/file_path.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/win/scoped_co_mem.h"
#include "base/win/registry.h"
#include "third_party/openssl/openssl/include/openssl/sha.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "third_party/libuv/include/uv.h"
#include "runtime/engine/common/utf16.h"
#include "runtime/engine/common/thread_call.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>
#include <shlobj.h>
#include <Shlwapi.h>
#include <shellapi.h>
#include <Netlistmgr.h>


namespace content {
void printCallstack();
}

namespace mini_electron {
int checkIsNetwork(INetworkListManager* pNetworkListManager);
static INetworkListManager* getNetworkList(IUnknown** ppUnknown);
}

namespace {

static void MINI_ELECTRON_CALL_TYPE onOnUvCreateProcessCallback(
    mini_electron_web_view webView, void* param, const WCHAR* applicationPath, const WCHAR* arguments, STARTUPINFOW* startup)
{
    OutputDebugStringW(L"onOnUvCreateProcessCallback:");
    OutputDebugStringW(applicationPath);
    OutputDebugStringW(L"\n");

    if (nullptr != wcsstr(applicationPath, L"git.exe"))
        startup->wShowWindow = SW_HIDE;
    if (nullptr != wcsstr(applicationPath, L"Microsoft.VSCode.CPP.Extension.exe"))
        startup->wShowWindow = SW_HIDE;
    if (nullptr != wcsstr(applicationPath, L"watcher\\win32\\CodeHelper.exe"))
        startup->wShowWindow = SW_HIDE;
}

}

namespace atom {

App* App::m_instance = nullptr;
int App::m_exitCode = 0;
const WCHAR kHiddenWindowPropName[] = L"mb_app_hidden_window";
static void registerHiddenWindowClass(LPCWSTR className);
static void notifySingleProcess(HWND window, base::Value::Dict additionalData);

App* App::getInstance()
{
    return m_instance;
}

App::App(v8::Isolate* isolate, v8::Local<v8::Object> wrapper)
{
    gin_helper::Wrappable<App>::InitWith(isolate, wrapper);
    m_instance = this;
    m_version = "1.3.3";
    m_name = "Electron";
    m_singleInstanceHandle = nullptr;

}

App::~App()
{
    releaseSingleInstanceLockApi();
    if (m_instance == this)
        m_instance = nullptr;
}

void App::init(v8::Local<v8::Object> target, v8::Isolate* isolate)
{
    v8::Local<v8::FunctionTemplate> prototype = v8::FunctionTemplate::New(isolate, newFunction);

    prototype->SetClassName(v8::String::NewFromUtf8(isolate, "App").ToLocalChecked());
    gin_helper::ObjectTemplateBuilder builder(isolate, prototype->InstanceTemplate());
    builder.SetMethod("quit", &App::quitApi);
    builder.SetMethod("exit", &App::exitApi);
    builder.SetMethod("focus", &App::focusApi);
    builder.SetMethod("getVersion", &App::getVersionApi);
    builder.SetMethod("setVersion", &App::setVersionApi);
    builder.SetMethod("getName", &App::getNameApi);
    builder.SetMethod("setName", &App::setNameApi);
    builder.SetMethod("isReady", &App::isReadyApi);
    builder.SetProperty("isPackaged", &App::isPackagedApi);
    builder.SetMethod("_setAppPath", &App::_setAppPathApi);
    builder.SetMethod("_setIsPackaged", &App::_setIsPackagedApi);
    builder.SetMethod("_setIsReady", &App::_setIsReadyApi);
    builder.SetMethod("isOnline", &App::isOnlineApi);
    builder.SetMethod("addRecentDocument", &App::addRecentDocumentApi);
    builder.SetMethod("clearRecentDocuments", &App::clearRecentDocumentsApi);
    builder.SetMethod("setAppUserModelId", &App::setAppUserModelIdApi);
    builder.SetMethod("requestSingleInstanceLock", &App::requestSingleInstanceLockApi);
    builder.SetMethod("hasSingleInstanceLock", &App::hasSingleInstanceLockApi);
    builder.SetMethod("isDefaultProtocolClient", &App::isDefaultProtocolClientApi);
    builder.SetMethod("setAsDefaultProtocolClient", &App::setAsDefaultProtocolClientApi);
    builder.SetMethod("removeAsDefaultProtocolClient", &App::removeAsDefaultProtocolClientApi);
    builder.SetMethod("setBadgeCount", &App::setBadgeCountApi);
    builder.SetMethod("getBadgeCount", &App::getBadgeCountApi);
    builder.SetMethod("getLoginItemSettings", &App::getLoginItemSettingsApi);
    builder.SetMethod("setLoginItemSettings", &App::setLoginItemSettingsApi);
    builder.SetMethod("setUserTasks", &App::setUserTasksApi);
    builder.SetMethod("getJumpListSettings", &App::getJumpListSettingsApi);
    builder.SetMethod("setJumpList", &App::setJumpListApi);
    builder.SetMethod("setPath", &App::setPathApi);
    builder.SetMethod("getPath", &App::getPathApi);
    builder.SetMethod("setDesktopName", &App::setDesktopNameApi);
    builder.SetMethod("getLocale", &App::getLocaleApi);
    builder.SetMethod("releaseSingleInstanceLock", &App::releaseSingleInstanceLockApi);
    builder.SetMethod("_relaunch", &App::relaunchApi);
    builder.SetMethod("isAccessibilitySupportEnabled", &App::isAccessibilitySupportEnabled);
    builder.SetMethod("disableHardwareAcceleration", &App::disableHardwareAcceleration);
    builder.SetMethod("getFileIcon", &App::getFileIconApi);

    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    constructor.Reset(isolate, prototype->GetFunction(context).ToLocalChecked());
    target->Set(context, v8::String::NewFromUtf8(isolate, "App").ToLocalChecked(), prototype->GetFunction(context).ToLocalChecked());
}

void App::nullFunction()
{
    OutputDebugStringA("nullFunction\n");
}

void App::quitApi()
{
    if (!content::ThreadCall::isUiThread()) {
        content::ThreadCall::callUiThreadAsync(FROM_HERE, [this] { quitApi(); });
        return;
    }
    if (m_quitState != QuitState::Running)
        return;
    m_quitState = QuitState::BeforeQuit;
    const bool prevented = emit("before-quit");
    if (m_quitState != QuitState::BeforeQuit)
        return;
    if (prevented) {
        m_quitState = QuitState::Running;
        return;
    }
    m_quitState = QuitState::ClosingWindows;
    WindowList::closeAllWindows();
    if (WindowList::getInstance()->empty() && m_quitState == QuitState::ClosingWindows)
        finishQuit(0, false);
}

void App::exitApi(gin_helper::Arguments* args)
{
    int exitCode = 0;
    if (args->Length() && !args->GetNext(&exitCode)) {
        args->ThrowTypeError("exitCode must be an integer");
        return;
    }
    finishQuit(exitCode, true);
}

void App::finishQuit(int exitCode, bool force)
{
    if (m_quitState == QuitState::Exiting)
        return;
    if (!force) {
        m_quitState = QuitState::WillQuit;
        const bool prevented = emit("will-quit");
        if (m_quitState != QuitState::WillQuit)
            return;
        if (prevented) {
            m_quitState = QuitState::Running;
            return;
        }
    }
    m_quitState = QuitState::Exiting;
    m_exitCode = exitCode;
    if (force)
        WindowList::destroyAllWindows();
    emit("quit", exitCode);
    releaseSingleInstanceLockApi();
    content::ThreadCall::exitUiThreadMessageLoop();
}

void App::onWindowCloseCancelled()
{
    if (m_quitState == QuitState::ClosingWindows)
        m_quitState = QuitState::Running;
}

void App::focusApi()
{
    WindowList* windows = WindowList::getInstance();
    if (windows->empty())
        return;
    HWND window = windows->get(windows->size() - 1)->getHWND();
    if (!window)
        return;
    if (::IsIconic(window))
        ::ShowWindow(window, SW_RESTORE);
    ::SetForegroundWindow(window);
    ::SetFocus(window);
}

bool App::isReadyApi() const
{
    return m_isReady;
}

void App::_setIsReadyApi()
{
    m_isReady = true;
}

bool App::isPackagedApi()
{
    return m_isPackaged;
}

void App::_setAppPathApi(const std::string& path)
{
    m_appPath = path;
    if (!SessionMgr::get()->setRootDir(base::FilePath::FromUTF8Unsafe(getPathApi("userData"))))
        throwPathError("Unable to initialize userData before creating sessions");
}

bool App::isOnlineApi()
{
    IUnknown* pUnknown = nullptr;
    static INetworkListManager* pNetworkListManager = nullptr;
    if (!pNetworkListManager)
        pNetworkListManager = mini_electron::getNetworkList(&pUnknown);

    BOOL checkNetwork = mini_electron::checkIsNetwork(pNetworkListManager);
    return !!checkNetwork;
}

// const std::string& path, gin_helper::Arguments* args
void App::getFileIconApi(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    std::string path;
    if (!gin_helper::ConvertFromV8(args.GetIsolate(), args[0], &path))
        return;

    if (path.empty())
        return;

    std::string sizeStr("small"); // small | normal | large
    base::Value::Dict obj;
    if (gin_helper::ConvertFromV8(args.GetIsolate(), args[1], &obj)) {
        const std::string* sizeStrPtr = obj.FindString("size");
        if (sizeStrPtr && !sizeStrPtr->empty())
            sizeStr = *sizeStrPtr;
    }

    gin_helper::Promise<v8::Local<v8::Object>>* promise = new gin_helper::Promise<v8::Local<v8::Object>>(args.GetIsolate());
    v8::Local<v8::Promise> ret = promise->GetHandle();

    int size = 0;
    if (sizeStr == "small")
        size = SHGFI_SMALLICON;
    else if (sizeStr == "normal")
        size = 0;
    else if (sizeStr == "large")
        size = SHGFI_LARGEICON;
    else
        return;

    std::string* pathStr = new std::string(path);

    App* self = this;
    content::ThreadCall::callUiThreadAsync(FROM_HERE, [self, promise, pathStr, size] {
        SHFILEINFO fileInfo = { 0 };
        v8::Isolate* isolate = v8::Isolate::GetCurrent();
        std::u16string path = base::UTF8ToUTF16(*pathStr);
        delete pathStr;

        if (!::SHGetFileInfo((const WCHAR*)path.c_str(), FILE_ATTRIBUTE_NORMAL, &fileInfo, sizeof(SHFILEINFO), SHGFI_ICON | size | SHGFI_USEFILEATTRIBUTES)) {
            promise->Reject();
            return;
        }

        v8::Local<v8::Object> bitmap = IconUtil::CreateSkBitmapFromHICON(isolate, fileInfo.hIcon);
        ::DestroyIcon(fileInfo.hIcon);
        if (bitmap.IsEmpty()) {
            promise->Reject();
            return;
        }

        promise->Resolve(bitmap);
    });

    args.GetReturnValue().Set(ret);
}

void App::addRecentDocumentApi(const std::string& path)
{
    OutputDebugStringA("addRecentDocumentApi\n");
}

void App::clearRecentDocumentsApi()
{
    OutputDebugStringA("clearRecentDocumentsApi\n");
}

void App::setAppUserModelIdApi(const std::string& id)
{
    SetAppUserModelID(base::UTF8ToWide(id));
}

bool App::requestSingleInstanceLockApi(gin_helper::Arguments* args)
{
    if (m_singleInstanceHandle)
        return true;
    base::Value::Dict additionalData;
    if (args->Length() && !args->GetNext(&additionalData)) {
        args->ThrowTypeError("additionalData must be an object");
        return false;
    }
    const base::FilePath root = base::MakeAbsoluteFilePath(base::FilePath::FromUTF8Unsafe(getPathApi("userData")));
    if (root.empty())
        return false;
    std::wstring normalized = root.value();
    ::CharLowerBuffW(normalized.data(), static_cast<DWORD>(normalized.size()));
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(normalized.data()), normalized.size() * sizeof(wchar_t), digest);
    static const wchar_t hex[] = L"0123456789abcdef";
    std::wstring suffix(SHA256_DIGEST_LENGTH * 2, L'0');
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        suffix[i * 2] = hex[digest[i] >> 4];
        suffix[i * 2 + 1] = hex[digest[i] & 15];
    }
    const std::wstring mutexName = L"Local\\MiniElectronProfile_" + suffix;
    const std::wstring className = L"MiniElectronSingleton_" + suffix;
    HANDLE mutex = ::CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (!mutex)
        return false;
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        const DWORD ownership = ::WaitForSingleObject(mutex, 0);
        if (ownership != WAIT_OBJECT_0 && ownership != WAIT_ABANDONED) {
            const ULONGLONG deadline = ::GetTickCount64() + 5000;
            HWND primary = nullptr;
            while (!primary && ::GetTickCount64() < deadline) {
                primary = ::FindWindowW(className.c_str(), nullptr);
                if (!primary)
                    ::Sleep(10);
            }
            if (primary)
                notifySingleProcess(primary, std::move(additionalData));
            ::CloseHandle(mutex);
            return false;
        }
    }
    registerHiddenWindowClass(className.c_str());
    m_hiddenWindow = ::CreateWindowExW(0, className.c_str(), L"", WS_OVERLAPPED,
        0, 0, 1, 1, nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
    if (!m_hiddenWindow) {
        ::ReleaseMutex(mutex);
        ::CloseHandle(mutex);
        return false;
    }
    m_singleInstanceHandle = mutex;
    ::ChangeWindowMessageFilterEx(m_hiddenWindow, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    return true;
}

static bool getProtocolRegistration(const v8::FunctionCallbackInfo<v8::Value>& args,
    std::wstring* schemeName, std::wstring* command)
{
    std::string scheme;
    if (!args.Length() || !gin_helper::ConvertFromV8(args.GetIsolate(), args[0], &scheme) || scheme.empty())
        return false;
    if (!base::IsAsciiAlpha(scheme[0]) || std::any_of(scheme.begin() + 1, scheme.end(), [](char c) {
        return !base::IsAsciiAlpha(c) && !base::IsAsciiDigit(c) && c != '+' && c != '-' && c != '.';
    }))
        return false;
    base::FilePath program;
    std::string executable;
    if (args.Length() > 1 && !args[1]->IsUndefined()) {
        if (!gin_helper::ConvertFromV8(args.GetIsolate(), args[1], &executable) || executable.empty())
            return false;
        program = base::FilePath::FromUTF8Unsafe(executable);
    } else if (!base::PathService::Get(base::FILE_EXE, &program)) {
        return false;
    }
    base::CommandLine invocation(program);
    if (args.Length() > 2 && !args[2]->IsUndefined()) {
        std::vector<std::string> arguments;
        if (!gin_helper::ConvertFromV8(args.GetIsolate(), args[2], &arguments))
            return false;
        for (const auto& argument : arguments)
            invocation.AppendArg(argument);
    }
    *schemeName = base::UTF8ToWide(base::ToLowerASCII(scheme));
    *command = invocation.GetCommandLineString() + L" \"%1\"";
    return true;
}

bool App::isDefaultProtocolClientApi(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    std::wstring scheme, expected, actual;
    if (!getProtocolRegistration(args, &scheme, &expected))
        return false;
    base::win::RegKey key(HKEY_CLASSES_ROOT, (scheme + L"\\shell\\open\\command").c_str(), KEY_READ);
    return key.ReadValue(nullptr, &actual) == ERROR_SUCCESS && actual == expected;
}

bool App::setAsDefaultProtocolClientApi(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    std::wstring scheme, command;
    if (!getProtocolRegistration(args, &scheme, &command))
        return false;
    const std::wstring keyPath = L"Software\\Classes\\" + scheme;
    base::win::RegKey protocol;
    if (protocol.Create(HKEY_CURRENT_USER, keyPath.c_str(), KEY_WRITE) != ERROR_SUCCESS
        || protocol.WriteValue(nullptr, L"URL:Custom Protocol") != ERROR_SUCCESS
        || protocol.WriteValue(L"URL Protocol", L"") != ERROR_SUCCESS)
        return false;
    base::win::RegKey handler;
    if (handler.Create(HKEY_CURRENT_USER, (keyPath + L"\\shell\\open\\command").c_str(), KEY_WRITE) != ERROR_SUCCESS
        || handler.WriteValue(nullptr, command.c_str()) != ERROR_SUCCESS)
        return false;
    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

bool App::removeAsDefaultProtocolClientApi(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    std::wstring scheme, expected, actual;
    if (!getProtocolRegistration(args, &scheme, &expected))
        return false;
    const std::wstring handlerPath = L"Software\\Classes\\" + scheme + L"\\shell\\open\\command";
    base::win::RegKey handler(HKEY_CURRENT_USER, handlerPath.c_str(), KEY_READ);
    if (handler.ReadValue(nullptr, &actual) != ERROR_SUCCESS || actual != expected)
        return false;
    base::win::RegKey classes(HKEY_CURRENT_USER, L"Software\\Classes", KEY_WRITE);
    if (classes.DeleteKey(scheme.c_str()) != ERROR_SUCCESS)
        return false;
    ::SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

bool App::setBadgeCountApi(int count)
{
    OutputDebugStringA("setBadgeCountApi\n");
    return false;
}

int App::getBadgeCountApi()
{
    OutputDebugStringA("getBadgeCountApi\n");
    return 0;
}

#if defined(_WIN32)
template <> struct gin_helper::Converter<LaunchItem> {
    static bool FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val, LaunchItem* out)
    {
        gin_helper::Dictionary dict(isolate);
        if (!ConvertFromV8(isolate, val, &dict))
            return false;

        dict.Get("name", &(out->name));
        dict.Get("path", &(out->path));
        dict.Get("args", &(out->args));
        dict.Get("scope", &(out->scope));
        dict.Get("enabled", &(out->enabled));
        return true;
    }

    static v8::Local<v8::Value> ToV8(v8::Isolate* isolate, LaunchItem val)
    {
        gin_helper::Dictionary dict = gin_helper::Dictionary::CreateEmpty(isolate);
        dict.Set("name", val.name);
        dict.Set("path", val.path);
        dict.Set("args", val.args);
        dict.Set("scope", val.scope);
        dict.Set("enabled", val.enabled);
        return dict.GetHandle();
    }
};
#endif

template <> struct gin_helper::Converter<LoginItemSettings> {
    static bool FromV8(v8::Isolate* isolate, v8::Local<v8::Value> val, LoginItemSettings* out)
    {
        gin_helper::Dictionary dict(isolate);
        if (!ConvertFromV8(isolate, val, &dict))
            return false;

        dict.Get("openAtLogin", &(out->open_at_login));
        dict.Get("openAsHidden", &(out->open_as_hidden));
        dict.Get("path", &(out->path));
        dict.Get("args", &(out->args));
#if defined(_WIN32)
        dict.Get("enabled", &(out->enabled));
        dict.Get("name", &(out->name));
#endif
        return true;
    }

    static v8::Local<v8::Value> ToV8(v8::Isolate* isolate, LoginItemSettings val)
    {
        gin_helper::Dictionary dict = gin_helper::Dictionary::CreateEmpty(isolate);
        dict.Set("openAtLogin", val.open_at_login);
        dict.Set("openAsHidden", val.open_as_hidden);
        dict.Set("restoreState", val.restore_state);
        dict.Set("wasOpenedAtLogin", val.opened_at_login);
        dict.Set("wasOpenedAsHidden", val.opened_as_hidden);
#if BUILDFLAG(IS_WIN)
        dict.Set("launchItems", val.launch_items);
        dict.Set("executableWillLaunchAtLogin", val.executable_will_launch_at_login);
#endif
        return dict.GetHandle();
    }
};

void App::getLoginItemSettingsApi(const v8::FunctionCallbackInfo<v8::Value>& info)
{
    gin_helper::Arguments args(info);
    LoginItemSettings options;
    args.GetNext(&options);
    LoginItemSettings setting = getLoginItemSettings(options);

    v8::Local<v8::Value> ret = gin_helper::Converter<LoginItemSettings>::ToV8(info.GetIsolate(), setting);
    info.GetReturnValue().Set(ret);
}

void App::setLoginItemSettingsApi(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    LoginItemSettings settings;
    if (args.Length() != 1)
        return;
    if (!gin_helper::Converter<LoginItemSettings>::FromV8(args.GetIsolate(), args[0], &settings))
        return;
    setLoginItemSettings(settings);
}

bool App::setUserTasksApi(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    OutputDebugStringA("setUserTasksApi\n");
    return true;
}

void App::setDesktopNameApi(const std::string& desktopName)
{
    OutputDebugStringA("App::setDesktopNameApi\n");
}

v8::Local<v8::Value> App::getJumpListSettingsApi(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    OutputDebugStringA("getJumpListSettingsApi\n");
    base::Value::Dict obj;

    obj.Set("minItems", 1);

    base::Value::List removedItems;
    obj.Set("removedItems", std::move(removedItems));

    v8::Local<v8::Value> result = gin_helper::Converter<base::Value::Dict>::ToV8(args.GetIsolate(), obj);
    return result;
}

// const base::DictionaryValue&
void App::setJumpListApi(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    OutputDebugStringA("setJumpListApi\n");
}

std::string App::getLocaleApi()
{
    wchar_t locale[LOCALE_NAME_MAX_LENGTH];
    if (!::GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH))
        return std::string();
    return base::WideToUTF8(locale);
}

static LRESULT CALLBACK staticWindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
static void registerHiddenWindowClass(LPCWSTR lpszClassName)
{
    WNDCLASS wndClass = { 0 };
    if (!GetClassInfoW(NULL, lpszClassName, &wndClass)) {
        wndClass.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
        wndClass.lpfnWndProc = &staticWindowProc;
        wndClass.cbClsExtra = 200;
        wndClass.cbWndExtra = 200;
        wndClass.hInstance = GetModuleHandleW(NULL);
        wndClass.hCursor = LoadCursor(NULL, IDC_ARROW);
        wndClass.hbrBackground = NULL;
        wndClass.lpszMenuName = NULL;
        wndClass.lpszClassName = lpszClassName;
        ATOM r = RegisterClass(&wndClass);
        r = r;
    }
}

static LRESULT CALLBACK staticWindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    App* self = (App*)::GetPropW(hWnd, kHiddenWindowPropName);
    if (!self && message == WM_CREATE) {
        LPCREATESTRUCTW cs = (LPCREATESTRUCTW)lParam;
        self = (App*)cs->lpCreateParams;
        ::SetPropW(hWnd, kHiddenWindowPropName, (HANDLE)self);
        return 0;
    }

    if (!self)
        return ::DefWindowProcW(hWnd, message, wParam, lParam);

    if (message == WM_COPYDATA) {
        COPYDATASTRUCT* copyData = (COPYDATASTRUCT*)lParam;
        if (copyData->dwData == WindowInterface::kSingleInstanceMessage) {
            self->onCopyData(copyData);
            return TRUE;
        }
    }

    return ::DefWindowProcW(hWnd, message, wParam, lParam);
}

void App::onCopyData(const COPYDATASTRUCT* copyData)
{
    if (!copyData || copyData->dwData != WindowInterface::kSingleInstanceMessage
        || !copyData->lpData || !copyData->cbData || copyData->cbData > 1024 * 1024)
        return;
    std::string json(static_cast<const char*>(copyData->lpData), copyData->cbData);
    content::ThreadCall::callUiThreadAsync(FROM_HERE, [this, json = std::move(json)] {
        auto message = base::JSONReader::ReadDict(json);
        if (!message)
            return;
        const auto* argv = message->FindList("argv");
        const auto* cwd = message->FindString("cwd");
        const auto* data = message->FindDict("additionalData");
        if (!argv || !cwd || !data || std::any_of(argv->begin(), argv->end(), [](const base::Value& argument) {
            return !argument.is_string();
        }))
            return;
        emit("second-instance", *argv, *cwd, *data);
    });
}

static void notifySingleProcess(HWND window, base::Value::Dict additionalData)
{
    base::FilePath cwd;
    if (!base::GetCurrentDirectory(&cwd))
        return;
    base::Value::List argv;
    for (const auto& argument : AtomCommandLine::argv())
        argv.Append(argument);
    base::Value::Dict message;
    message.Set("argv", std::move(argv));
    message.Set("cwd", cwd.AsUTF8Unsafe());
    message.Set("additionalData", std::move(additionalData));
    std::string json;
    if (!base::JSONWriter::Write(message, &json) || json.size() > 1024 * 1024)
        return;
    COPYDATASTRUCT copyData = {};
    copyData.dwData = WindowInterface::kSingleInstanceMessage;
    copyData.cbData = static_cast<DWORD>(json.size());
    copyData.lpData = json.data();
    DWORD_PTR result = 0;
    ::SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copyData),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &result);
}

void App::releaseSingleInstanceLockApi()
{
    if (m_hiddenWindow) {
        ::DestroyWindow(m_hiddenWindow);
        m_hiddenWindow = nullptr;
    }
    if (m_singleInstanceHandle) {
        ::ReleaseMutex(m_singleInstanceHandle);
        ::CloseHandle(m_singleInstanceHandle);
        m_singleInstanceHandle = nullptr;
    }
}

int RunRelauncher(int argc, wchar_t* argv[])
{
    if (argc < 5 || wcscmp(argv[1], L"--type=relauncher") || wcscmp(argv[3], L"--"))
        return 1;
    wchar_t* end = nullptr;
    const unsigned long long value = wcstoull(argv[2], &end, 10);
    if (!value || !end || *end || value > UINTPTR_MAX)
        return 1;
    base::win::ScopedHandle parent(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value)));
    if (::WaitForSingleObject(parent.Get(), INFINITE) != WAIT_OBJECT_0)
        return 1;
    base::CommandLine target { base::FilePath(argv[4]) };
    for (int i = 5; i < argc; ++i)
        target.AppendArgNative(argv[i]);
    return base::LaunchProcess(target, base::LaunchOptions()).IsValid() ? 0 : 1;
}

void App::relaunchApi(const base::Value::Dict& options)
{
    base::FilePath executable;
    if (!base::PathService::Get(base::FILE_EXE, &executable)) {
        throwPathError("Unable to locate the current executable for relaunch");
        return;
    }
    base::FilePath target = executable;
    if (const auto* execPath = options.Find("execPath")) {
        if (!execPath->is_string() || execPath->GetString().empty()) {
            throwPathError("execPath must be a nonempty string");
            return;
        }
        target = base::FilePath::FromUTF8Unsafe(execPath->GetString());
    }
    HANDLE handle = nullptr;
    if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentProcess(), ::GetCurrentProcess(),
        &handle, SYNCHRONIZE, TRUE, 0)) {
        throwPathError("Unable to create the relaunch wait handle");
        return;
    }
    base::win::ScopedHandle parent(handle);
    base::CommandLine helper(executable);
    helper.AppendSwitchASCII("type", "relauncher");
    helper.AppendArg(std::to_string(reinterpret_cast<std::uintptr_t>(parent.Get())));
    helper.AppendArg("--");
    helper.AppendArgPath(target);
    if (const auto* arguments = options.Find("args")) {
        if (!arguments->is_list()) {
            throwPathError("args must be an array of strings");
            return;
        }
        for (const auto& argument : arguments->GetList()) {
            if (!argument.is_string()) {
                throwPathError("args must be an array of strings");
                return;
            }
            helper.AppendArg(argument.GetString());
        }
    } else {
        const auto defaultArguments = AtomCommandLine::argv();
        for (size_t i = 1; i < defaultArguments.size(); ++i)
            helper.AppendArg(defaultArguments[i]);
    }
    base::LaunchOptions launch;
    launch.handles_to_inherit.push_back(parent.Get());
    if (!base::LaunchProcess(helper, launch).IsValid())
        throwPathError("Unable to start the relaunch helper");
}

void App::setPathApi(const std::string& name, const std::string& path)
{
    static const char* const names[] = { "home", "appData", "userData", "sessionData", "temp",
        "exe", "module", "desktop", "documents", "downloads", "music", "pictures", "videos",
        "recent", "logs", "crashDumps", "cache", "userCache" };
    if (std::find(std::begin(names), std::end(names), name) == std::end(names)) {
        throwPathError("Unknown path name: " + name);
        return;
    }
    const base::FilePath directory = base::FilePath::FromUTF8Unsafe(path);
    if (!directory.IsAbsolute() || !base::CreateDirectory(directory)) {
        throwPathError("Path must be an absolute, creatable directory: " + path);
        return;
    }
    if ((name == "userData" || name == "sessionData") && !SessionMgr::get()->setRootDir(directory)) {
        throwPathError("Cannot change the session root after sessions have been created: " + path);
        return;
    }
    m_pathMap[name] = directory.AsUTF8Unsafe();
}

bool getTempDir(base::FilePath* path)
{
    WCHAR temp_path[MAX_PATH + 1];
    DWORD path_len = ::GetTempPath(MAX_PATH, temp_path);
    if (path_len >= MAX_PATH || path_len <= 0)
        return false;
    // TODO(evanm): the old behavior of this function was to always strip the
    // trailing slash.  We duplicate this here, but it shouldn't be necessary
    // when everyone is using the appropriate FilePath APIs.
    *path = base::FilePath(temp_path).StripTrailingSeparators();
    return true;
}

base::FilePath getHomeDir()
{
    WCHAR result[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPath(NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, result)) && result[0])
        return base::FilePath(result);

    // Fall back to the temporary directory on failure.
    base::FilePath temp;
    if (getTempDir(&temp))
        return temp;

    // Last resort.
    return base::FilePath(L"C:\\");
}

// Generic function to call SHGetFolderPath().
bool getUserDirectory(int csidl_folder, base::FilePath* result) {
    // We need to go compute the value. It would be nice to support paths
    // with names longer than MAX_PATH, but the system functions don't seem
    // to be designed for it either, with the exception of GetTempPath
    // (but other things will surely break if the temp path is too long,
    // so we don't bother handling it.
    wchar_t pathBuf[MAX_PATH];
    pathBuf[0] = 0;
    if (FAILED(::SHGetFolderPath(nullptr, csidl_folder, nullptr,
        SHGFP_TYPE_CURRENT, pathBuf))) {
        return false;
    }
    *result = base::FilePath(pathBuf);
    return true;
}

bool getUserDocumentsDirectory(base::FilePath* result) 
{
    return getUserDirectory(CSIDL_MYDOCUMENTS, result);
}

bool getUserMusicDirectory(base::FilePath* result) 
{
    return getUserDirectory(CSIDL_MYMUSIC, result);
}

bool getUserPicturesDirectory(base::FilePath* result)
{
    return getUserDirectory(CSIDL_MYPICTURES, result);
}

bool getUserVideosDirectory(base::FilePath* result) 
{
    return getUserDirectory(CSIDL_MYVIDEO, result);
}

bool getUserRecentDirectory(base::FilePath* result)
{
    return getUserDirectory(CSIDL_RECENT, result);
}

// Return a default path for downloads that is safe.
// We just use 'Downloads' under DIR_USER_DOCUMENTS. Localizing
// 'downloads' is not a good idea because Chrome's UI language
// can be changed.
bool getUserDownloadsDirectorySafe(base::FilePath* result) 
{
    if (!getUserDocumentsDirectory(result))
        return false;

    *result = result->Append(L"Downloads");
    return true;
}

// Get the downloads known folder. Since it can be relocated to point to a
// "dangerous" folder, callers should validate that the returned path is not
// dangerous before using it.
bool getUserDownloadsDirectory(base::FilePath* result)
{
    base::win::ScopedCoMem<wchar_t> pathBuf;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &pathBuf))) {
        *result = base::FilePath(std::wstring(pathBuf));
        return true;
    }
    return getUserDownloadsDirectorySafe(result);
}

std::string App::getPathApi(const std::string& name) const
{
    const auto override = m_pathMap.find(name);
    if (override != m_pathMap.end())
        return override->second;
    base::FilePath path;
    if (name == "appData")
        getUserDirectory(CSIDL_APPDATA, &path);
    else if (name == "userData")
        path = base::FilePath::FromUTF8Unsafe(getPathApi("appData")).Append(base::FilePath::FromUTF8Unsafe(m_name));
    else if (name == "sessionData")
        return getPathApi("userData");
    else if (name == "cache" || name == "userCache")
        path = base::FilePath::FromUTF8Unsafe(getPathApi("userData")).AppendASCII("Cache");
    else if (name == "logs")
        path = base::FilePath::FromUTF8Unsafe(getPathApi("userData")).AppendASCII("logs");
    else if (name == "crashDumps")
        path = base::FilePath::FromUTF8Unsafe(getPathApi("userData")).AppendASCII("Crashpad");
    else if (name == "home")
        path = getHomeDir();
    else if (name == "temp")
        getTempDir(&path);
    else if (name == "desktop")
        getUserDirectory(CSIDL_DESKTOPDIRECTORY, &path);
    else if (name == "exe" || name == "module")
        base::PathService::Get(base::FILE_EXE, &path);
    else if (name == "documents")
        getUserDocumentsDirectory(&path);
    else if (name == "music")
        getUserMusicDirectory(&path);
    else if (name == "pictures")
        getUserPicturesDirectory(&path);
    else if (name == "videos")
        getUserVideosDirectory(&path);
    else if (name == "recent")
        getUserRecentDirectory(&path);
    else if (name == "downloads")
        getUserDownloadsDirectory(&path);
    if (path.empty())
        throwPathError("Unable to resolve path: " + name);
    return path.AsUTF8Unsafe();
}

void App::throwPathError(const std::string& message) const
{
    isolate()->ThrowException(v8::Exception::Error(gin_helper::StringToV8(isolate(), message)));
}

void App::newFunction(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Isolate* isolate = args.GetIsolate();
    if (args.IsConstructCall()) {
        new App(isolate, args.This());
        args.GetReturnValue().Set(args.This());
        return;
    }
}

void App::onWindowAllClosed()
{
    if (!content::ThreadCall::isUiThread()) {
        content::ThreadCall::callUiThreadAsync(FROM_HERE, [this] { onWindowAllClosed(); });
        return;
    }
    if (m_quitState == QuitState::ClosingWindows)
        finishQuit(0, false);
    else if (m_quitState == QuitState::Running)
        emit("window-all-closed");
}

v8::Persistent<v8::Function> App::constructor;
gin::WrapperInfo App::kWrapperInfo = { gin::kEmbedderNativeGin };

static void initializeAppApi(v8::Local<v8::Object> target, v8::Local<v8::Value> unused, v8::Local<v8::Context> context, const NodeNative* native)
{
    //node::Environment* env = node::Environment::GetCurrent(context);
    App::init(target, context->GetIsolate());
}

static const char BrowserAppNative[] = "console.log('BrowserAppNative');;";
static NodeNative nativeBrowserAppNative { "App", BrowserAppNative, sizeof(BrowserAppNative) - 1 };

NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_MANUAL(electron_browser_app, initializeAppApi, &nativeBrowserAppNative)

}