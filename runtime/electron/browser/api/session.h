// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ELECTRON_BROWSER_API_API_SESSION_H_
#define ELECTRON_BROWSER_API_API_SESSION_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <utility>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/synchronization/lock.h"
#include "runtime/electron/common/renderer_client.h"
#include "base/values.h"
#include "runtime/electron/common/api/event_emitter.h"
#include "runtime/engine/public/engine_api.h"

namespace atom {

class ApiWebRequest;
class EngineWebRequestPipeline;
class ProtocolInterface;
class BrokerRequestPipeline;
class WebContents;
class BrokerWebSocket;
struct BrokerSocketRegistry;

class ApiSession : public mate::EventEmitter<ApiSession> {
public:
    using BrokerReply = std::function<void(base::Value::Dict, std::string)>;
    struct BrokerFrameContext {
        uint64_t frame_id = 0;
        std::string committed_url;
        std::string committed_origin;
        std::string top_origin;
        // Set only by the browser's one-shot navigation authorization registry.
        std::string approved_navigation_url;
        std::string navigation_initiator_origin;
        std::string approved_navigation_method;
        bool browser_approved_navigation = false;
        // True only for an exact native main-process loadURL authorization.
        bool browser_initiated_navigation = false;
        bool web_security = true;
        bool allow_running_insecure_content = false;
        std::vector<base::FilePath> application_resource_roots;
    };


    static const char* kDefaultSessionName;
    static const char* kDefaultDir;

    static ApiSession* create(v8::Isolate* isolate, const std::string& partition,
        const base::FilePath& path, bool persistent);
    static void init(v8::Isolate* isolate, v8::Local<v8::Object> target);
    static void newFunction(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void fromPartitionApi(const v8::FunctionCallbackInfo<v8::Value>& args);

    std::string getPath() const { return m_path.AsUTF8Unsafe(); }
    std::string getName() const { return m_partition; }
    std::string getPartition() const { return m_partition; }
    bool isPersistent() const { return m_persistent; }

    std::vector<std::string> getPreloadsApi();
    void onLoadUrlBeginInBlinkThread(mini_electron_web_view webView, const char* url, void* job);
    void downloadURL(WebContents*, const std::string& url);

    // Main-process side of the sandbox renderer broker. The owning WebContents
    // selects the ApiSession; the child cannot choose a partition.
    void handleBrokerRequest(int contentsId,
        const BrokerFrameContext& frameContext,
        const base::Value::Dict& payload, BrokerReply reply);
    bool checkPermission(int contentsId, const std::string& permission,
        const base::Value::Dict& details);
    void requestPermission(int contentsId, const std::string& permission,
        const base::Value::Dict& details, std::function<void(bool)> callback);
    std::string authorizeFile(int contentsId, const base::FilePath& path);
    bool authorizeUploadPaths(int contents_id,
        const std::vector<std::string>& paths, std::string* error);
    base::Value::List authorizeSelectedFiles(int contentsId,
        const std::vector<std::string>& paths, std::string* error);
    void revokeFile(const std::string& token);
    bool checkDevicePermission(int contentsId,
        const base::Value::Dict& details);
    std::vector<RendererPrivilegedScheme> getPrivilegedSchemes() const;

    void onWebContentsDestroyed(int contentsId);
    friend class BrokerRequestPipeline;

private:
    friend class EngineWebRequestPipeline;
    ApiSession(v8::Isolate* isolate, v8::Local<v8::Object> wrapper);
    ~ApiSession();

    void webRequestApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    void protocolApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    void getStoragePathApi(const v8::FunctionCallbackInfo<v8::Value>& args) const;
    void setDownloadPathApi(const std::string& path);
    void setPermissionRequestHandlerApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    void setPermissionCheckHandlerApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    void setDevicePermissionHandlerApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    void setPreloadsApi(const std::vector<std::string>& paths);
    void clearStorageDataApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    void clearCacheApi(const v8::FunctionCallbackInfo<v8::Value>& args);
    void clearAuthCacheApi(const v8::FunctionCallbackInfo<v8::Value>& args);


    bool clearStorage(const std::vector<std::string>& storages, std::string* error);
    bool clearDirectories(const std::vector<const char*>& names, std::string* error);
    bool loadBrokerStorage(std::string* error);
    bool saveBrokerStorage(std::string* error);
    void handleStorageBroker(int contentsId,
        const BrokerFrameContext& frameContext,
        const base::Value::Dict& payload, BrokerReply reply);
    void handleFileBroker(int contentsId,
        const base::Value::Dict& payload, BrokerReply reply);
    void handleNetworkBroker(int contentsId,
        const BrokerFrameContext& frameContext,
        const base::Value::Dict& payload, BrokerReply reply);
#if !defined(MINI_ELECTRON_DISABLE_DEVTOOLS)
    void handleDevToolsResource(const base::Value::Dict& payload,
        BrokerReply reply);
#endif
    void* brokerCookieShare();

    ApiWebRequest* m_webRequest = nullptr;
    ProtocolInterface* m_protocol = nullptr;
    base::FilePath m_path;
    std::string m_partition;
    bool m_persistent = false;
    v8::Persistent<v8::Object> m_liveSelf;
    v8::Persistent<v8::Function> m_permissionRequestHandler;
    v8::Persistent<v8::Function> m_permissionCheckHandler;
    v8::Persistent<v8::Function> m_devicePermissionHandler;
    std::vector<std::string> m_preloadPaths;
    std::string m_downloadPath;


    base::Lock m_brokerLock;
    bool m_brokerStorageLoaded = false;
    base::Value::Dict m_brokerLocalStorage;
    std::map<std::string, base::Value::Dict> m_brokerSessionStorage;
    std::map<int, std::vector<std::string>> m_sessionStorageNamespaces;
    struct AuthorizedFile {
        base::FilePath path;
        int64_t size = 0;
        int contents_id = 0;
    };
    std::map<std::string, AuthorizedFile> m_authorizedFiles;
    std::shared_ptr<BrokerSocketRegistry> m_brokerSocketRegistry;
    std::map<std::pair<int, std::string>, size_t> m_pendingUploadGrants;
    std::map<int, std::vector<std::weak_ptr<std::atomic_bool>>>
        m_brokerRequestCancellations;
    void* m_brokerCookieShare = nullptr;
    base::Lock m_brokerCookieShareLock;
    bool m_removePathOnDestroy = false;

public:
    static gin::WrapperInfo kWrapperInfo;
    static v8::Persistent<v8::Function> constructor;
};

class SessionMgr {
public:
    static SessionMgr* get();
    // Must be called by App after userData has been selected and before any
    // Session/WebContents is created. Returns false rather than silently using
    // an empty or fallback profile.
    bool setRootDir(const base::FilePath& root);
    ApiSession* findOrCreateSession(v8::Isolate* isolate,
        const std::string& partition, bool createIfNotExist);
    base::FilePath createSessionDirname(const std::string& partition,
        bool* persistent = nullptr) const;
    base::FilePath getRootDir() const;

private:
    SessionMgr();
    bool ensureRootDir();

    mutable base::Lock m_lock;
    std::map<std::string, ApiSession*> m_map;
    base::FilePath m_rootDir;
    static SessionMgr* m_inst;
};

} // namespace atom

#endif // ELECTRON_BROWSER_API_API_SESSION_H_
