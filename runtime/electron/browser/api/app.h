#ifndef browser_api_ApiApp_h
#define browser_api_ApiApp_h

#include "runtime/electron/common/api/event_emitter.h"
#include "runtime/electron/common/gin_helper/public/wrapper_info.h"
#include <map>

typedef struct HWND__* HWND;
typedef struct tagCOPYDATASTRUCT COPYDATASTRUCT;
typedef void* HANDLE;

namespace gin_helper {
class Arguments;
}

namespace atom {

int RunRelauncher(int argc, wchar_t* argv[]);

class App : public mate::EventEmitter<App> {
public:
    explicit App(v8::Isolate* isolate, v8::Local<v8::Object> wrapper);

    ~App();

    static void init(v8::Local<v8::Object> target, v8::Isolate* isolate);
    static App* getInstance();

    void nullFunction();

    void quitApi();
    void exitApi(gin_helper::Arguments* args);
    void focusApi();
    bool isReadyApi() const;
    void _setIsReadyApi();
    bool isPackagedApi();
    void _setAppPathApi(const std::string& path);
    const std::string& getAppPath() const { return m_appPath; }
    void _setIsPackagedApi(bool packaged) { m_isPackaged = packaged; }
    bool isOnlineApi();
    void getFileIconApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    void addRecentDocumentApi(const std::string& path);
    void clearRecentDocumentsApi();
    void setAppUserModelIdApi(const std::string& id);
    bool requestSingleInstanceLockApi(gin_helper::Arguments* args);
    bool hasSingleInstanceLockApi() const { return m_singleInstanceHandle != nullptr; }
    bool isDefaultProtocolClientApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    bool setAsDefaultProtocolClientApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    bool removeAsDefaultProtocolClientApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    bool setBadgeCountApi(int count);
    int getBadgeCountApi();
    void getLoginItemSettingsApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    void setLoginItemSettingsApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    bool setUserTasksApi(const v8::FunctionCallbackInfo<v8::Value>& args);

    bool isAccessibilitySupportEnabled()
    {
        return false;
    }
    void disableHardwareAcceleration()
    {
    }

    void setVersionApi(const std::string& version)
    {
        m_version = version;
    }
    std::string getVersionApi() const
    {
        return m_version;
    }

    void setNameApi(const std::string& name)
    {
        m_name = name;
    }
    std::string getNameApi() const
    {
        return m_name;
    }

    void setPathApi(const std::string& name, const std::string& path);
    std::string getPathApi(const std::string& name) const;

    void setDesktopNameApi(const std::string& desktopName);

    v8::Local<v8::Value> getJumpListSettingsApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    void setJumpListApi(const v8::FunctionCallbackInfo<v8::Value>& args);

    std::string getLocaleApi();

    void releaseSingleInstanceLockApi();

    void relaunchApi(const base::Value::Dict& options);

    static void newFunction(const v8::FunctionCallbackInfo<v8::Value>& args);

    void onWindowAllClosed();
    void onWindowCloseCancelled();
    static int getExitCode() { return m_exitCode; }

public:
    void onCopyData(const COPYDATASTRUCT* copyData);

    static gin_helper::WrapperInfo kWrapperInfo;
    static v8::Persistent<v8::Function> constructor;

    HWND m_hiddenWindow = nullptr;
    HANDLE m_singleInstanceHandle = nullptr;

private:
    enum class QuitState { Running, BeforeQuit, ClosingWindows, WillQuit, Exiting };
    void finishQuit(int exitCode, bool force);
    void throwPathError(const std::string& message) const;

    static App* m_instance;
    static int m_exitCode;
    QuitState m_quitState = QuitState::Running;
    bool m_isReady = false;
    std::string m_version;
    std::string m_name;
    std::string m_appPath;
    bool m_isPackaged = false;
    std::map<std::string, std::string> m_pathMap;
};

} // atom

#endif // browser_api_ApiApp_h