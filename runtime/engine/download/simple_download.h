
#ifndef MINI_ELECTRON_ENGINE_DOWNLOAD_SIMPLE_DOWNLOAD_H_
#define MINI_ELECTRON_ENGINE_DOWNLOAD_SIMPLE_DOWNLOAD_H_

#include "runtime/engine/common/thread_call.h"
#include "base/files/file_path.h"
#include "base/synchronization/lock.h"
#include <process.h>
#include <shlwapi.h>

namespace base {
class File;
}

namespace download {

class SimpleDownload {
public:
    static SimpleDownload* create(mini_electron_web_view webView, const WCHAR* savePath, const mini_electron_dialog_options* dialogOpt, const mini_electron_download_options* downloadOpt,
        size_t expectedContentLength, const char* url, const char* mime, const char* disposition, mini_electron_net_job job, mini_electron_net_job_data_bind* dataBind,
        mini_electron_download_bind* callbackBind);

    ~SimpleDownload();

    static unsigned int WINAPI dialogThread(void* param);

    static int getDialogCount();

private:
    SimpleDownload(mini_electron_web_view mini_electron_view, size_t expectedContentLength, const char* url, const char* mime, const char* disposition, mini_electron_net_job job,
        mini_electron_net_job_data_bind* dataBind, mini_electron_download_bind* callbackBind);

    void startSave(/*std::vector<WCHAR>* path*/bool ok);
    void doSave();
    bool canSave();
    void onBeginSaveCallback();

    static void MINI_ELECTRON_CALL_TYPE onDataRecv(void* param, mini_electron_net_job job, const char* data, int length);
    static void MINI_ELECTRON_CALL_TYPE onDataFinish(void* param, mini_electron_net_job job, mini_electron_loading_result result);

    void onDataRecvImpl(mini_electron_net_job job, const char* data, int length);
    void onDataFinishImpl(mini_electron_net_job job, mini_electron_loading_result result);

    static int m_dialogCount;

    std::string m_url;
    std::string m_mime;
    std::string m_contentDisposition;

    base::Lock m_saveFullPathLock; // m_saveFullPath会被多线程使用
    std::u16string m_saveFullPath; // 用户设置的全路径
    
    base::FilePath m_saveTempFullPath; // 先放到临时路径，再改名成m_savePath

    base::File* m_handleOfSave;
    size_t m_totalSize;
    size_t m_downloadedSize;

    mini_electron_web_view m_engineView;

    std::vector<char> m_cacheData;
    bool m_hadCallDataFinish = false; // 是否调用过onDataFinishImpl
    bool m_hasStartSave = false;// 是否调用过startSave
    mini_electron_loading_result m_loadingResult;

    struct DialogOptions {
        std::string title;
        std::string defaultPath;
        std::string buttonLabel;
    };
    DialogOptions dialogOpt;

    mini_electron_download_bind m_callbackBind;
};

}

#endif // download_SimpleDownload_h