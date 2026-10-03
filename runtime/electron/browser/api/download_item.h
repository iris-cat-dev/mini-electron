// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ELECTRON_BROWSER_API_API_DOWNLOAD_ITEM_H_
#define ELECTRON_BROWSER_API_API_DOWNLOAD_ITEM_H_

#include <atomic>
#include <memory>
#include <map>
#include <string>
#include <vector>

#include "runtime/electron/common/api/event_emitter.h"
#include "runtime/engine/public/engine_api.h"

namespace atom {

class ApiDownloadItem : public mate::EventEmitter<ApiDownloadItem> {
public:
    static ApiDownloadItem* create(v8::Isolate* isolate);

    std::string getSavePath() const { return m_savePath; }
    bool isCancelled() const { return m_state == kCancelled; }
    void finishCancelledBeforeStart();

    static void init(v8::Isolate* isolate, v8::Local<v8::Object> target);
    void updateProgress(size_t received);
    void finish(mini_electron_loading_result result);

private:
    ApiDownloadItem(v8::Isolate* isolate, v8::Local<v8::Object> wrapper);
    ~ApiDownloadItem();

    void setSavePathApi(const std::string path);
    std::string getSavePathApi() const;
    void setSaveDialogOptionsApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    void getSaveDialogOptionsApi(const v8::FunctionCallbackInfo<v8::Value>& args) const;
    void pauseApi();
    bool isPausedApi() const;
    void resumeApi();
    bool canResumeApi() const;
    void cancelApi();
    std::string getURLApi() const;
    std::string getMimeTypeApi() const;
    bool hasUserGestureApi() const;
    std::string getFilenameApi() const;
    int getTotalBytesApi() const;
    int getReceivedBytesApi() const;
    std::string getContentDispositionApi() const;
    std::string getStateApi() const;
    std::vector<std::string> getURLChainApi() const;
    std::string getLastModifiedTimeApi() const;
    std::string getETagApi() const;
    std::string getStartTimeApi() const;

public:
    int m_id;
    static gin::WrapperInfo kWrapperInfo;
    static v8::Persistent<v8::Function> constructor;
    std::string m_url;
    std::string m_mime;
    size_t m_recvSize;
    size_t m_allSize;
    std::string m_savePath;
    std::string m_disposition;
    v8::Persistent<v8::Object> m_liveSelf;
    std::shared_ptr<std::atomic_bool> m_canceled;
    std::shared_ptr<std::atomic_bool> m_paused;
    bool m_done = false;

    enum State { kProgressing, kCompleted, kCancelled, kInterrupted };
    State m_state;
    bool m_isPaused;

private:
    static void newFunction(const v8::FunctionCallbackInfo<v8::Value>& args);
};

} // namespace atom

namespace gin {
v8::Local<v8::Value> ConvertToV8(
    v8::Isolate* isolate, const atom::ApiDownloadItem& item);
} // namespace gin

#endif // ELECTRON_BROWSER_API_API_DOWNLOAD_ITEM_H_
