
#include "src/runtime/electron/node_bindings.h"
#include "src/runtime/electron/common/node_register_help.h"
#include "src/runtime/electron/common/string_util.h"
#include "src/runtime/electron/common/api/event_emitter.h"
#include "src/runtime/electron/browser/api/window_interface.h"
#include "src/runtime/electron/browser/api/window_list.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "third_party/libuv/include/uv.h"
#include "src/runtime/electron/common/gin_helper/promise.h"
#include "src/runtime/electron/common/gin_helper/object_template_builder.h"
#include "src/runtime/electron/common/gin_helper/dictionary.h"
#include "src/runtime/electron/common/gin_helper/public/gin_embedders.h"
#include "src/runtime/electron/common/gin_helper/public/wrapper_info.h"
#include "src/runtime/engine/common/thread_call.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include <CommCtrl.h>
#include <Shobjidl.h>
#include <Shlobj.h>
#include <vector>
#include <string>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

//struct __declspec(uuid("00000000-0000-0000-c000-000000000046")) IFileOpenDialog;
namespace content {
void printCallstack();
}

namespace atom {

class Dialog : public mate::EventEmitter<Dialog> {
public:
    explicit Dialog(v8::Isolate* isolate, v8::Local<v8::Object> wrapper)
    {
        gin_helper::Wrappable<Dialog>::InitWith(isolate, wrapper);
    }

    static void init(v8::Isolate* isolate, v8::Local<v8::Object> target)
    {
        v8::Local<v8::Context> context = isolate->GetCurrentContext();
        v8::Local<v8::FunctionTemplate> prototype = v8::FunctionTemplate::New(isolate, newFunction);

        prototype->SetClassName(v8::String::NewFromUtf8(isolate, "Dialog").ToLocalChecked());
        gin_helper::ObjectTemplateBuilder builder(isolate, prototype->InstanceTemplate());
        builder.SetMethod("_showOpenDialog", &Dialog::_showOpenDialogApi);
        builder.SetMethod("_showSaveDialog", &Dialog::_showSaveDialogApi);
        builder.SetMethod("_showOpenDialogSync", &Dialog::_showOpenDialogSyncApi);
        builder.SetMethod("_showSaveDialogSync", &Dialog::_showSaveDialogSyncApi);
        builder.SetMethod("_showMessageBox", &Dialog::_showMessageBoxApi);
        builder.SetMethod("_showMessageBoxSync", &Dialog::_showMessageBoxSyncApi);
        builder.SetMethod("_showErrorBox", &Dialog::_showErrorBoxApi);

        constructor.Reset(isolate, prototype->GetFunction(context).ToLocalChecked());
        target->Set(context, v8::String::NewFromUtf8(isolate, "Dialog").ToLocalChecked(), prototype->GetFunction(context).ToLocalChecked());
    }

    void nullFunction()
    {
    }

    static void newFunction(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        v8::Isolate* isolate = args.GetIsolate();
        if (args.IsConstructCall()) {
            new Dialog(isolate, args.This());
            args.GetReturnValue().Set(args.This());
            return;
        }
    }

    // showSaveDialog([browserWindow, ]options[, callback])
    void _showSaveDialogApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        _showOpenOrSaveDialogApi(false, false, args);
    }

    void _showOpenDialogApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        _showOpenOrSaveDialogApi(true, false, args);
    }

    void _showSaveDialogSyncApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        _showOpenOrSaveDialogApi(false, true, args);
    }

    void _showOpenDialogSyncApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        _showOpenOrSaveDialogApi(true, true, args);
    }

    // <description, extensions>
    typedef std::pair<std::string, std::vector<std::string>> Filter;
    typedef std::vector<Filter> Filters;

    // This state crosses the STA boundary; it deliberately contains no V8 handles.
    struct FileDialogState {
        bool isOpenOrSave = true;
        std::string title;
        std::string defaultPath;
        std::string buttonLabel;
        Filters filters;
        int fileDialogProperty = 0;
        base::Value::List paths;
        HRESULT error = E_FAIL;
        HWND owner = nullptr;
        HANDLE completed = nullptr;
        std::atomic<bool> canceled { false };
        std::mutex windowMutex;
        HWND cancelWindow = nullptr; // Protected against destruction/HWND reuse.
        IOleWindow* nativeWindow = nullptr; // Accessed only on the dialog's STA.

        ~FileDialogState()
        {
            if (completed)
                ::CloseHandle(completed);
        }

        void cancel()
        {
            canceled.store(true);
            std::lock_guard<std::mutex> lock(windowMutex);
            if (cancelWindow)
                ::PostMessageW(cancelWindow, WM_APP, 0, 0);
        }

        void closeIfCanceled()
        {
            if (!canceled.load() || !nativeWindow)
                return;
            HWND window = nullptr;
            if (SUCCEEDED(nativeWindow->GetWindow(&window)) && window)
                ::PostMessageW(window, WM_CLOSE, 0, 0);
        }

        void wait()
        {
            // Show(owner) sends messages to the owner's thread. Dispatch only
            // sent messages while joining, never queued JS/Node/completion work.
            while (::MsgWaitForMultipleObjectsEx(1, &completed, INFINITE,
                       QS_SENDMESSAGE, MWMO_INPUTAVAILABLE) != WAIT_OBJECT_0) {
                MSG message;
                ::PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
            }
        }

        static LRESULT CALLBACK cancelWindowProc(HWND window, UINT message,
            WPARAM wParam, LPARAM lParam)
        {
            if (message == WM_NCCREATE) {
                auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
                ::SetWindowLongPtrW(window, GWLP_USERDATA,
                    reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            }
            auto* state = reinterpret_cast<FileDialogState*>(
                ::GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == WM_APP && state) {
                state->closeIfCanceled();
                return 0;
            }
            return ::DefWindowProcW(window, message, wParam, lParam);
        }
    };

    // Shell initialization can pump cancellation before its dialog HWND exists.
    // Native dialog events retry at initialization boundaries, without timers.
    class FileDialogEvents final : public IFileDialogEvents {
    public:
        explicit FileDialogEvents(FileDialogState* state) : m_state(state) { }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override
        {
            if (!result)
                return E_POINTER;
            if (iid == IID_IUnknown || iid == __uuidof(IFileDialogEvents)) {
                *result = static_cast<IFileDialogEvents*>(this);
                AddRef();
                return S_OK;
            }
            *result = nullptr;
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG count = --m_references;
            if (!count)
                delete this;
            return count;
        }
        HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*) override
        {
            m_state->closeIfCanceled();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override
        {
            m_state->closeIfCanceled();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override
        {
            m_state->closeIfCanceled();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override
        {
            m_state->closeIfCanceled();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override
        {
            m_state->closeIfCanceled();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*,
            FDE_SHAREVIOLATION_RESPONSE* response) override
        {
            *response = FDESVR_DEFAULT;
            m_state->closeIfCanceled();
            return S_OK;
        }
        HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*,
            FDE_OVERWRITE_RESPONSE* response) override
        {
            *response = FDEOR_DEFAULT;
            m_state->closeIfCanceled();
            return S_OK;
        }

    private:
        std::atomic<ULONG> m_references { 1 };
        FileDialogState* m_state;
    };

    struct AsyncFileDialog : public std::enable_shared_from_this<AsyncFileDialog> {
        explicit AsyncFileDialog(v8::Isolate* isolate,
            const std::shared_ptr<FileDialogState>& state)
            : isolate(isolate), state(state) { }

        v8::Isolate* isolate;
        std::shared_ptr<FileDialogState> state;
        v8::Global<v8::Context> context;
        v8::Global<v8::Object> receiver;
        v8::Global<v8::Function> callback;
        HWND owner = nullptr;
        bool started = false;
        bool finished = false;

        static std::vector<std::shared_ptr<AsyncFileDialog>>& active()
        {
            static std::vector<std::shared_ptr<AsyncFileDialog>> dialogs;
            return dialogs;
        }

        static std::shared_ptr<AsyncFileDialog> find(const std::shared_ptr<FileDialogState>& state)
        {
            for (const auto& operation : active()) {
                if (operation->state == state)
                    return operation;
            }
            return nullptr;
        }

        static LRESULT CALLBACK ownerSubclass(HWND window, UINT message,
            WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
        {
            if (message == WM_DESTROY || message == WM_NCDESTROY) {
                auto* operation = reinterpret_cast<AsyncFileDialog*>(data);
                auto keepAlive = operation->shared_from_this();
                operation->state->cancel();
                ::RemoveWindowSubclass(window, ownerSubclass, id);
                operation->owner = nullptr;
                // Do not let DestroyWindow release/reuse the exact owner HWND
                // while a queued or active Show() can still reference it.
                if (operation->started)
                    operation->state->wait();
            }
            return ::DefSubclassProc(window, message, wParam, lParam);
        }

        void finish(bool deliver)
        {
            if (finished)
                return;
            auto keepAlive = shared_from_this();
            finished = true;
            if (started)
                state->wait();
            if (owner) {
                ::RemoveWindowSubclass(owner, ownerSubclass, reinterpret_cast<UINT_PTR>(this));
                owner = nullptr;
            }
            auto& dialogs = active();
            dialogs.erase(std::remove(dialogs.begin(), dialogs.end(), keepAlive), dialogs.end());
            if (deliver) {
                v8::HandleScope handles(isolate);
                auto localContext = context.Get(isolate);
                v8::Context::Scope scope(localContext);
                auto localReceiver = receiver.Get(isolate);
                auto localCallback = callback.Get(isolate);
                const bool canceled = state->canceled.load()
                    || state->error == HRESULT_FROM_WIN32(ERROR_CANCELLED);
                if (canceled)
                    state->paths.clear();
                v8::Local<v8::Value> argv[] = {
                    v8::Boolean::New(isolate, canceled),
                    gin_helper::Converter<base::Value::List>::ToV8(isolate, state->paths),
                    FAILED(state->error) && !canceled
                        ? gin_helper::StringToV8(isolate,
                              dialogErrorMessage("File dialog", state->error)).As<v8::Value>()
                        : v8::Undefined(isolate).As<v8::Value>()
                };
                // Detach all native and persistent state before calling user JS:
                // it may destroy the owner, open another dialog, or exit.
                context.Reset();
                receiver.Reset();
                callback.Reset();
                (void)localCallback->Call(localContext, localReceiver, 3, argv);
            }
            context.Reset();
            receiver.Reset();
            callback.Reset();
        }

        void begin()
        {
            if (finished)
                return;
            if (state->canceled.load()) {
                finish(true);
                return;
            }
            auto* parameter = new std::shared_ptr<FileDialogState>(state);
            HANDLE thread = ::CreateThread(nullptr, 0, showOpenOrSaveDialogThreadEntryPoint,
                parameter, 0, nullptr);
            if (!thread) {
                state->error = HRESULT_FROM_WIN32(::GetLastError());
                delete parameter;
                finish(true);
                return;
            }
            started = true;
            ::CloseHandle(thread);
        }
    };

    static void shutdownDialogs(v8::Isolate* isolate = nullptr)
    {
        shuttingDown = true;
        // Cancel every worker before waiting for any of them. Never deliver
        // completion callbacks while shutdown removes isolate-owned handles.
        const auto dialogs = AsyncFileDialog::active();
        for (const auto& operation : dialogs) {
            if (!isolate || operation->isolate == isolate)
                operation->state->cancel();
        }
        for (const auto& operation : dialogs) {
            if (!isolate || operation->isolate == isolate)
                operation->finish(false);
        }
    }

    static void cleanupDialogs(void* isolate)
    {
        shutdownDialogs(static_cast<v8::Isolate*>(isolate));
        constructor.Reset();
    }

    static std::string dialogErrorMessage(const char* operation, HRESULT error)
    {
        wchar_t* systemMessage = nullptr;
        DWORD length = ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr, error, 0, reinterpret_cast<wchar_t*>(&systemMessage), 0, nullptr);
        std::string message(operation);
        message += " failed";
        if (length && systemMessage) {
            while (length && (systemMessage[length - 1] == L'\r' || systemMessage[length - 1] == L'\n'))
                --length;
            message += ": ";
            message += base::UTF16ToUTF8(
                std::u16string(reinterpret_cast<const char16_t*>(systemMessage), length));
        } else {
            message += " (HRESULT ";
            message += std::to_string(static_cast<unsigned long>(error));
            message += ")";
        }
        if (systemMessage)
            ::LocalFree(systemMessage);
        return message;
    }

    static DWORD __stdcall showOpenOrSaveDialogThreadEntryPoint(void* parameter)
    {
        std::unique_ptr<std::shared_ptr<FileDialogState>> holder(
            static_cast<std::shared_ptr<FileDialogState>*>(parameter));
        auto state = *holder;
        if (!state->canceled.load()) {
            state->error = showOpenOrSaveDialog(state->isOpenOrSave, state->owner,
                state->title, state->buttonLabel, state->defaultPath,
                state->filters, state->fileDialogProperty, &state->paths, state.get());
        }
        content::ThreadCall::callUiThreadAsync(FROM_HERE, [state] {
            if (auto operation = AsyncFileDialog::find(state))
                operation->finish(true);
        });
        ::SetEvent(state->completed);
        return 0;
    }

    // showOpenDialog([browserWindow, ]options[, callback])
    void _showOpenOrSaveDialogApi(bool isOpenOrSave, bool isSync,
        const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        if (args.Length() != 3)
            return;
        v8::Isolate* isolate = args.GetIsolate();
        v8::Local<v8::Context> context = isolate->GetCurrentContext();
        if (!isSync && shuttingDown) {
            isolate->ThrowException(v8::Exception::Error(
                gin_helper::StringToV8(isolate, "File dialog is shutting down")));
            return;
        }

        int browserWindowId = -1;
        if (args[0]->IsInt32())
            browserWindowId = args[0]->ToInt32(context).ToLocalChecked()->Value();

        v8::Local<v8::Object> options;
        if (args[1]->IsObject())
            options = args[1]->ToObject(context).ToLocalChecked();

        auto state = std::make_shared<FileDialogState>();
        state->isOpenOrSave = isOpenOrSave;
        WindowInterface* face = WindowList::getInstance()->find(browserWindowId);
        if (face)
            state->owner = face->getHWND();
        // Only async calls gain the new lifetime contract; sync semantics stay
        // on the owner's thread, with the same options/result behavior.
        if (!isSync && args[0]->IsInt32()
            && (!face || face->isClosed() || !::IsWindow(state->owner))) {
            isolate->ThrowException(v8::Exception::Error(
                gin_helper::StringToV8(isolate, "File dialog owner is destroyed or unknown")));
            return;
        }

        getOptions(isolate, options, &state->title, &state->defaultPath,
            &state->filters, &state->buttonLabel, &state->fileDialogProperty);

        if (!isSync) {
            if (!args[2]->IsFunction()) {
                isolate->ThrowException(v8::Exception::TypeError(
                    gin_helper::StringToV8(isolate, "Dialog callback is required")));
                return;
            }
            state->completed = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!state->completed) {
                isolate->ThrowException(v8::Exception::Error(
                    gin_helper::StringToV8(isolate, "Failed to create dialog completion event")));
                return;
            }
            auto operation = std::make_shared<AsyncFileDialog>(isolate, state);
            operation->owner = state->owner;
            if (state->owner && !::SetWindowSubclass(state->owner,
                    AsyncFileDialog::ownerSubclass, reinterpret_cast<UINT_PTR>(operation.get()),
                    reinterpret_cast<DWORD_PTR>(operation.get()))) {
                isolate->ThrowException(v8::Exception::Error(
                    gin_helper::StringToV8(isolate, "Failed to observe file dialog owner")));
                return;
            }
            operation->context.Reset(isolate, context);
            operation->receiver.Reset(isolate, context->Global());
            operation->callback.Reset(isolate, args[2].As<v8::Function>());
            AsyncFileDialog::active().push_back(operation);
            // Establish owner guards now, but return the Promise before native
            // presentation. Immediate destroy() cancels even a queued operation.
            content::ThreadCall::callUiThreadAsync(FROM_HERE, [state] {
                if (auto operation = AsyncFileDialog::find(state))
                    operation->begin();
            });
            return;
        }

        state->error = showOpenOrSaveDialog(isOpenOrSave, state->owner,
            state->title, state->buttonLabel, state->defaultPath, state->filters,
            state->fileDialogProperty, &state->paths);

        const bool canceled = state->error == HRESULT_FROM_WIN32(ERROR_CANCELLED);
        if (FAILED(state->error) && !canceled) {
            isolate->ThrowException(v8::Exception::Error(
                gin_helper::StringToV8(isolate, dialogErrorMessage("File dialog", state->error))));
            return;
        }
        if (!canceled) {
            args.GetReturnValue().Set(
                gin_helper::Converter<base::Value::List>::ToV8(isolate, state->paths));
        }
    }

    void _showErrorBoxApi(const std::string& title, const std::string& content)
    {
        content::printCallstack();
        std::u16string titleW = base::UTF8ToUTF16(title);
        std::u16string contentW = base::UTF8ToUTF16(content);
        ::MessageBoxW(nullptr, (LPCWSTR)contentW.c_str(), (LPCWSTR)titleW.c_str(), MB_OK);
    }

    void _showMessageBoxApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        showMessageBoxSyncImpl(args, false);
    }

    void _showMessageBoxSyncApi(const v8::FunctionCallbackInfo<v8::Value>& args)
    {
        showMessageBoxSyncImpl(args, true);
    }

    struct ShowMessageBoxThreadInfo {
        std::u16string title;
        std::u16string message;
        std::u16string detail;
        std::u16string checkboxLabel;
        std::vector<std::u16string> buttons;
        std::string type;
        int defaultId;
        int cancelId;
        v8::Isolate* isolate;
        gin_helper::Promise<gin_helper::Dictionary>* promise;
        HWND hwnd;
        HANDLE completed;
        int response;
        bool checkboxChecked;
        HRESULT error;
    };

    static DWORD __stdcall showMessageBoxThreadEntryPoint(void* param)
    {
        ShowMessageBoxThreadInfo* info =
            static_cast<ShowMessageBoxThreadInfo*>(param);
        const int buttonIdBase = 1000;
        std::vector<TASKDIALOG_BUTTON> taskButtons(info->buttons.size());
        for (size_t i = 0; i < info->buttons.size(); ++i) {
            taskButtons[i].nButtonID = buttonIdBase + static_cast<int>(i);
            taskButtons[i].pszButtonText =
                reinterpret_cast<LPCWSTR>(info->buttons[i].c_str());
        }

        TASKDIALOGCONFIG config = { sizeof(config) };
        config.hwndParent = info->hwnd;
        config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
        if (info->hwnd)
            config.dwFlags |= TDF_POSITION_RELATIVE_TO_WINDOW;
        if (info->checkboxChecked)
            config.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
        config.pszWindowTitle = reinterpret_cast<LPCWSTR>(info->title.c_str());
        config.pszMainInstruction = reinterpret_cast<LPCWSTR>(info->message.c_str());
        if (!info->detail.empty())
            config.pszContent = reinterpret_cast<LPCWSTR>(info->detail.c_str());
        if (!info->checkboxLabel.empty()) {
            config.pszVerificationText =
                reinterpret_cast<LPCWSTR>(info->checkboxLabel.c_str());
        }
        if (info->type == "error")
            config.pszMainIcon = TD_ERROR_ICON;
        else if (info->type == "warning")
            config.pszMainIcon = TD_WARNING_ICON;
        else if (info->type == "question")
            config.pszMainIcon = TD_INFORMATION_ICON;
        else
            config.pszMainIcon = TD_INFORMATION_ICON;
        config.cButtons = static_cast<UINT>(taskButtons.size());
        config.pButtons = taskButtons.data();
        config.nDefaultButton = buttonIdBase + info->defaultId;

        int selectedButton = 0;
        BOOL verificationChecked = info->checkboxChecked ? TRUE : FALSE;
        info->error = ::TaskDialogIndirect(
            &config, &selectedButton, nullptr, &verificationChecked);
        info->checkboxChecked = verificationChecked != FALSE;
        if (SUCCEEDED(info->error)) {
            if (selectedButton >= buttonIdBase
                && selectedButton < buttonIdBase + static_cast<int>(info->buttons.size())) {
                info->response = selectedButton - buttonIdBase;
            } else {
                info->response = info->cancelId;
            }
        }

        if (info->completed) {
            ::SetEvent(info->completed);
            return 0;
        }

        content::ThreadCall::callUiThreadAsync(FROM_HERE, [info] {
            v8::HandleScope handleScope(info->isolate);
            if (FAILED(info->error)) {
                info->promise->RejectWithErrorMessage(
                    dialogErrorMessage("Message dialog", info->error));
            } else {
                gin_helper::Dictionary dict =
                    gin_helper::Dictionary::CreateEmpty(info->isolate);
                dict.Set("response", info->response);
                dict.Set("checkboxChecked", info->checkboxChecked);
                info->promise->Resolve(dict);
            }
            delete info->promise;
            delete info;
        });
        return 0;
    }

    void showMessageBoxSyncImpl(
        const v8::FunctionCallbackInfo<v8::Value>& args, bool isSync)
    {
        if (args.Length() != 3)
            return;

        v8::Isolate* isolate = args.GetIsolate();
        v8::Local<v8::Context> context = isolate->GetCurrentContext();
        int browserWindowId = -1;
        if (args[0]->IsInt32())
            browserWindowId = args[0]->ToInt32(context).ToLocalChecked()->Value();

        v8::Local<v8::Object> options;
        if (args[1]->IsObject())
            options = args[1]->ToObject(context).ToLocalChecked();

        std::string title;
        std::string message;
        std::string detail;
        std::string type;
        std::string checkboxLabel;
        std::vector<std::string> buttons;
        int defaultId = 0;
        int cancelId = 0;
        bool checkboxChecked = false;
        getMessageOptions(isolate, options, &title, &message, &detail, &type,
            &buttons, &defaultId, &cancelId, &checkboxLabel, &checkboxChecked);
        if (buttons.empty())
            buttons.push_back("OK");
        if (defaultId < 0 || defaultId >= static_cast<int>(buttons.size()))
            defaultId = 0;
        if (cancelId < 0 || cancelId >= static_cast<int>(buttons.size()))
            cancelId = 0;

        ShowMessageBoxThreadInfo* info = new ShowMessageBoxThreadInfo();
        info->title = base::UTF8ToUTF16(title);
        info->message = base::UTF8ToUTF16(message);
        info->detail = base::UTF8ToUTF16(detail);
        info->checkboxLabel = base::UTF8ToUTF16(checkboxLabel);
        for (const std::string& button : buttons)
            info->buttons.push_back(base::UTF8ToUTF16(button));
        info->type = type;
        info->defaultId = defaultId;
        info->cancelId = cancelId;
        info->promise = nullptr;
        info->isolate = isolate;
        info->hwnd = nullptr;
        info->completed = isSync ? ::CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
        info->response = cancelId;
        info->checkboxChecked = checkboxChecked;
        info->error = E_FAIL;

        WindowInterface* face = WindowList::getInstance()->find(browserWindowId);
        if (face)
            info->hwnd = face->getHWND();

        v8::Local<v8::Promise> handle;
        if (!isSync) {
            info->promise = new gin_helper::Promise<gin_helper::Dictionary>(isolate);
            handle = info->promise->GetHandle();
        } else if (!info->completed) {
            delete info;
            isolate->ThrowException(v8::Exception::Error(
                gin_helper::StringToV8(isolate, "Failed to create dialog completion event")));
            return;
        }

        if (isSync) {
            // Keep the owned modal window on its owner's UI thread.
            showMessageBoxThreadEntryPoint(info);
            ::WaitForSingleObject(info->completed, INFINITE);
            ::CloseHandle(info->completed);
        } else {
            DWORD threadIdentifier = 0;
            HANDLE threadHandle = ::CreateThread(
                nullptr, 0, showMessageBoxThreadEntryPoint, info, 0,
                &threadIdentifier);
            if (!threadHandle) {
                delete info->promise;
                delete info;
                isolate->ThrowException(v8::Exception::Error(
                    gin_helper::StringToV8(isolate, "Failed to create dialog thread")));
                return;
            }
            ::CloseHandle(threadHandle);
            args.GetReturnValue().Set(handle);
            return;
        }
        if (FAILED(info->error)) {
            std::string error = dialogErrorMessage("Message dialog", info->error);
            delete info;
            isolate->ThrowException(v8::Exception::Error(
                gin_helper::StringToV8(isolate, error)));
            return;
        }
        args.GetReturnValue().Set(v8::Integer::New(isolate, info->response));
        delete info;
    }

private:
    enum FileDialogProperty {
        FILE_DIALOG_OPEN_FILE = 1 << 0,
        FILE_DIALOG_OPEN_DIRECTORY = 1 << 1,
        FILE_DIALOG_MULTI_SELECTIONS = 1 << 2,
        FILE_DIALOG_CREATE_DIRECTORY = 1 << 3,
        FILE_DIALOG_SHOW_HIDDEN_FILES = 1 << 4,
        FILE_DIALOG_PROMPT_TO_CREATE = 1 << 5,
    };

    void getMessageOptions(
        v8::Isolate* isolate,
        v8::Local<v8::Object> options,
        std::string* title,
        std::string* message,
        std::string* detail,
        std::string* type,
        std::vector<std::string>* buttons,
        int* defaultId,
        int* cancelId,
        std::string* checkboxLabel,
        bool* checkboxChecked)
    {
        base::Value::Dict optionsDict;
        if (options.IsEmpty()
            || !gin_helper::Converter<base::Value::Dict>::FromV8(
                isolate, options, &optionsDict)) {
            return;
        }

        const std::string* tempStr = optionsDict.FindString("title");
        if (tempStr)
            *title = *tempStr;
        tempStr = optionsDict.FindString("message");
        if (tempStr)
            *message = *tempStr;
        tempStr = optionsDict.FindString("detail");
        if (tempStr)
            *detail = *tempStr;
        tempStr = optionsDict.FindString("type");
        if (tempStr)
            *type = *tempStr;
        tempStr = optionsDict.FindString("checkboxLabel");
        if (tempStr)
            *checkboxLabel = *tempStr;

        *defaultId = optionsDict.FindInt("defaultId").value_or(0);
        *cancelId = optionsDict.FindInt("cancelId").value_or(0);
        *checkboxChecked =
            optionsDict.FindBool("checkboxChecked").value_or(false);

        const base::Value::List* buttonsList = optionsDict.FindList("buttons");
        if (!buttonsList)
            return;
        for (const base::Value& value : *buttonsList) {
            if (value.is_string())
                buttons->push_back(*value.GetIfString());
        }
    }

    void getOptions(v8::Isolate* isolate, v8::Local<v8::Object> options, std::string* title, std::string* defaultPath, Filters* filters,
        std::string* buttonLabel, int* fileDialogProperty)
    {
        base::Value::Dict optionsDict;
        if (options.IsEmpty() || !gin_helper::Converter<base::Value::Dict>::FromV8(isolate, options, &optionsDict))
            return;

        base::Value::List* properties = nullptr;

        std::string* temp = optionsDict.FindString("title");
        if (temp)
            *title = *temp;
        temp = optionsDict.FindString("defaultPath");
        if (temp)
            *defaultPath = *temp;
        temp = optionsDict.FindString("buttonLabel");
        if (temp)
            *buttonLabel = *temp;
        properties = optionsDict.FindList("properties");

        *fileDialogProperty = propertiesToEnum(properties);

        base::Value::List* filtersList = nullptr;
        filtersList = optionsDict.FindList("filters");
        if (!filtersList)
            return;

        for (size_t i = 0; i < filtersList->size(); ++i) {
            const base::Value::Dict* filtersItem;
            const base::Value& filtersListVal = (*filtersList)[i];
            if (!filtersListVal.is_dict())
                continue;
            filtersItem = filtersListVal.GetIfDict();

            const std::string* name = filtersItem->FindString("name");
            if (!name)
                continue;
            const base::Value::List* extensionsList = filtersItem->FindList("extensions");
            if (!extensionsList)
                continue;

            Filter filter;
            std::vector<std::string> extensions;
            for (size_t j = 0; j < extensionsList->size(); ++j) {
                const base::Value& extensionsListVal = (*extensionsList)[j];
                if (!extensionsListVal.is_string())
                    continue;
                const std::string* extension = extensionsListVal.GetIfString();
                if (!extension)
                    continue;
                if (extension->empty() || ((size_t)-1) != extension->find(L'.'))
                    continue;
                extensions.push_back(*extension);
            }
            if (0 == extensions.size())
                extensions.push_back("*");
            filter.first = *name;
            filter.second = extensions;
            filters->push_back(filter);
        }
    }

    int propertiesToEnum(base::Value::List* properties)
    {
        int out = 0;
        if (!properties)
            return out;

        for (size_t i = 0; i < properties->size(); ++i) {
            const base::Value& propertieVal = (*properties)[i];
            if (!propertieVal.is_string())
                continue;
            const std::string* propertie = propertieVal.GetIfString();
            if (!propertie)
                continue;
            if ("openFile" == *propertie)
                out |= FILE_DIALOG_OPEN_FILE;
            else if ("openDirectory" == *propertie)
                out |= FILE_DIALOG_OPEN_DIRECTORY;
            else if ("multiSelections" == *propertie)
                out |= FILE_DIALOG_MULTI_SELECTIONS;
            else if ("showHiddenFiles" == *propertie)
                out |= FILE_DIALOG_SHOW_HIDDEN_FILES;
            else if ("createDirectory" == *propertie)
                out |= FILE_DIALOG_CREATE_DIRECTORY;
            else if ("promptToCreate" == *propertie)
                out |= FILE_DIALOG_PROMPT_TO_CREATE;
        }
        return out;
    }

    static void setDefaultPath(IFileDialog* dialog, const std::u16string& defaultPath)
    {
        if (defaultPath.empty())
            return;

        std::u16string folderPath = defaultPath;
        std::u16string fileName;
        DWORD attributes = ::GetFileAttributesW(
            reinterpret_cast<LPCWSTR>(defaultPath.c_str()));
        if (attributes == INVALID_FILE_ATTRIBUTES
            || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            size_t separator = defaultPath.find_last_of(
                reinterpret_cast<const char16_t*>(L"\\/"));
            if (separator != std::u16string::npos) {
                folderPath = defaultPath.substr(0, separator);
                fileName = defaultPath.substr(separator + 1);
            }
        }

        IShellItem* folder = nullptr;
        HRESULT hr = ::SHCreateItemFromParsingName(
            reinterpret_cast<LPCWSTR>(folderPath.c_str()), nullptr,
            IID_PPV_ARGS(&folder));
        if (SUCCEEDED(hr)) {
            dialog->SetFolder(folder);
            folder->Release();
        }
        if (!fileName.empty())
            dialog->SetFileName(reinterpret_cast<LPCWSTR>(fileName.c_str()));
    }

    static HRESULT appendShellItemPath(
        IShellItem* item, base::Value::List* paths)
    {
        PWSTR path = nullptr;
        HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        if (FAILED(hr))
            return hr;
        paths->Append(base::UTF16ToUTF8(
            std::u16string(reinterpret_cast<const char16_t*>(path))));
        ::CoTaskMemFree(path);
        return S_OK;
    }

    static HRESULT showOpenOrSaveDialog(
        bool isOpenOrSave,
        HWND parentWindow,
        const std::string& title,
        const std::string& buttonLabel,
        const std::string& defaultPath,
        const Filters& filters,
        int properties,
        base::Value::List* paths, FileDialogState* state = nullptr)
    {
        HRESULT initializeResult =
            ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initializeResult))
            return initializeResult;

        IFileDialog* dialog = nullptr;
        HRESULT hr = ::CoCreateInstance(
            isOpenOrSave ? CLSID_FileOpenDialog : CLSID_FileSaveDialog,
            nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        if (FAILED(hr)) {
            ::CoUninitialize();
            return hr;
        }

        IOleWindow* nativeWindow = nullptr;
        DWORD eventCookie = 0;
        bool eventsAdvised = false;
        if (state) {
            hr = dialog->QueryInterface(IID_PPV_ARGS(&nativeWindow));
            if (SUCCEEDED(hr)) {
                state->nativeWindow = nativeWindow;
                auto* events = new FileDialogEvents(state);
                hr = dialog->Advise(events, &eventCookie);
                events->Release();
                eventsAdvised = SUCCEEDED(hr);
            }
        }

        if (state && SUCCEEDED(hr)) {
            static const ATOM windowClass = [] {
                WNDCLASSW type = {};
                type.lpfnWndProc = FileDialogState::cancelWindowProc;
                type.hInstance = ::GetModuleHandleW(nullptr);
                type.lpszClassName = L"MiniElectronFileDialogCancellation";
                return ::RegisterClassW(&type);
            }();
            std::lock_guard<std::mutex> lock(state->windowMutex);
            if (windowClass) {
                state->cancelWindow = ::CreateWindowExW(0,
                    L"MiniElectronFileDialogCancellation", L"", 0, 0, 0, 0, 0,
                    HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), state);
            }
            if (!state->cancelWindow) {
                const DWORD error = ::GetLastError();
                hr = error ? HRESULT_FROM_WIN32(error) : E_FAIL;
            }
        }

        DWORD dialogOptions = 0;
        if (SUCCEEDED(hr))
            hr = dialog->GetOptions(&dialogOptions);
        if (SUCCEEDED(hr)) {
            dialogOptions |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
            if (properties & FILE_DIALOG_SHOW_HIDDEN_FILES)
                dialogOptions |= FOS_FORCESHOWHIDDEN;
            if (isOpenOrSave) {
                if (properties & FILE_DIALOG_OPEN_DIRECTORY)
                    dialogOptions |= FOS_PICKFOLDERS;
                else
                    dialogOptions |= FOS_FILEMUSTEXIST;
                if (properties & FILE_DIALOG_MULTI_SELECTIONS)
                    dialogOptions |= FOS_ALLOWMULTISELECT;
                // The modern picker exposes its standard New Folder command;
                // FILE_DIALOG_CREATE_DIRECTORY therefore needs no extra flag.
            } else {
                dialogOptions |= FOS_OVERWRITEPROMPT;
                if (properties & FILE_DIALOG_PROMPT_TO_CREATE)
                    dialogOptions |= FOS_CREATEPROMPT;
            }
            hr = dialog->SetOptions(dialogOptions);
        }

        std::u16string titleW = base::UTF8ToUTF16(title);
        std::u16string buttonLabelW = base::UTF8ToUTF16(buttonLabel);
        if (SUCCEEDED(hr) && !titleW.empty())
            hr = dialog->SetTitle(reinterpret_cast<LPCWSTR>(titleW.c_str()));
        if (SUCCEEDED(hr) && !buttonLabelW.empty()) {
            hr = dialog->SetOkButtonLabel(
                reinterpret_cast<LPCWSTR>(buttonLabelW.c_str()));
        }

        std::vector<std::u16string> filterNames;
        std::vector<std::u16string> filterPatterns;
        std::vector<COMDLG_FILTERSPEC> filterSpecs;
        if (SUCCEEDED(hr)
            && !(properties & FILE_DIALOG_OPEN_DIRECTORY)
            && !filters.empty()) {
            filterNames.reserve(filters.size());
            filterPatterns.reserve(filters.size());
            filterSpecs.reserve(filters.size());
            for (const Filter& filter : filters) {
                filterNames.push_back(base::UTF8ToUTF16(filter.first));
                std::u16string pattern;
                for (const std::string& extension : filter.second) {
                    if (!pattern.empty())
                        pattern += reinterpret_cast<const char16_t*>(L";");
                    pattern += reinterpret_cast<const char16_t*>(L"*.");
                    pattern += base::UTF8ToUTF16(extension);
                }
                filterPatterns.push_back(pattern);
            }
            for (size_t i = 0; i < filterNames.size(); ++i) {
                COMDLG_FILTERSPEC spec = {
                    reinterpret_cast<LPCWSTR>(filterNames[i].c_str()),
                    reinterpret_cast<LPCWSTR>(filterPatterns[i].c_str())
                };
                filterSpecs.push_back(spec);
            }
            hr = dialog->SetFileTypes(
                static_cast<UINT>(filterSpecs.size()), filterSpecs.data());
            if (SUCCEEDED(hr))
                hr = dialog->SetFileTypeIndex(1);
        }

        if (SUCCEEDED(hr)) {
            setDefaultPath(dialog, base::UTF8ToUTF16(defaultPath));
            hr = state && state->canceled.load()
                ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : dialog->Show(parentWindow);
        }

        if (SUCCEEDED(hr) && isOpenOrSave) {
            IFileOpenDialog* openDialog = nullptr;
            hr = dialog->QueryInterface(IID_PPV_ARGS(&openDialog));
            if (SUCCEEDED(hr)) {
                IShellItemArray* results = nullptr;
                hr = openDialog->GetResults(&results);
                if (SUCCEEDED(hr)) {
                    DWORD count = 0;
                    hr = results->GetCount(&count);
                    for (DWORD i = 0; SUCCEEDED(hr) && i < count; ++i) {
                        IShellItem* item = nullptr;
                        hr = results->GetItemAt(i, &item);
                        if (SUCCEEDED(hr)) {
                            hr = appendShellItemPath(item, paths);
                            item->Release();
                        }
                    }
                    results->Release();
                }
                openDialog->Release();
            }
        } else if (SUCCEEDED(hr)) {
            IShellItem* result = nullptr;
            hr = dialog->GetResult(&result);
            if (SUCCEEDED(hr)) {
                hr = appendShellItemPath(result, paths);
                result->Release();
            }
        }

        if (eventsAdvised)
            dialog->Unadvise(eventCookie);

        if (state) {
            std::lock_guard<std::mutex> lock(state->windowMutex);
            if (state->cancelWindow)
                ::DestroyWindow(state->cancelWindow);
            state->cancelWindow = nullptr;
            state->nativeWindow = nullptr;
        }
        if (nativeWindow)
            nativeWindow->Release();

        dialog->Release();
        ::CoUninitialize();
        return hr;
    }

public:
    static gin_helper::WrapperInfo kWrapperInfo;
    static v8::Persistent<v8::Function> constructor;
    static bool shuttingDown;
};


v8::Persistent<v8::Function> Dialog::constructor;
bool Dialog::shuttingDown = false;
gin_helper::WrapperInfo Dialog::kWrapperInfo = { gin_helper::GinEmbedder::kEmbedderNativeGin };

void shutdownFileDialogs()
{
    Dialog::shutdownDialogs();
}

static void initializeDialogApi(v8::Local<v8::Object> target, v8::Local<v8::Value> unused, v8::Local<v8::Context> context, const NodeNative* native)
{
    Dialog::init(context->GetIsolate(), target);
    node::AddEnvironmentCleanupHook(context->GetIsolate(),
        Dialog::cleanupDialogs, context->GetIsolate());
}

static const char BrowserDialogNative[] = "console.log('BrowserDialogNative');"
                                          "exports = function {};";

static NodeNative nativeBrowserDialogNative { "Dialog", BrowserDialogNative, sizeof(BrowserDialogNative) - 1 };

NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_MANUAL(electron_browser_dialog, initializeDialogApi, &nativeBrowserDialogNative)

}