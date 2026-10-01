
#ifndef MINI_ELECTRON_NETWORK_LOADER_PAGE_NET_EXTRA_DATA_H_
#define MINI_ELECTRON_NETWORK_LOADER_PAGE_NET_EXTRA_DATA_H_

#include "base/memory/ref_counted.h"
#include "base/files/file_path.h"
#include "runtime/storage/storage_def.h"
#include <string>

typedef void CURL;
typedef void CURLSH;
typedef struct mini_electron_proxy_impl mini_electron_proxy;

namespace blink {
class WebStorageNamespace;
}

namespace mini_electron {

class WebCookieJarImpl;
class WebStorageNamespaceImpl;

class PageNetExtraData : public base::RefCountedThreadSafe<PageNetExtraData> {
public:
    PageNetExtraData();
    ~PageNetExtraData();

    void setCookieJarFullPath(const std::string& fullPathUtf8);
    CURLSH* getCurlShareHandle();
    std::string getCookieJarFullPath();
    base::FilePath getDownloadDirPath();

    WebCookieJarImpl* getCookieJar() const
    {
        return m_cookieJar;
    }

    void setLocalStorageDir(const std::string& localStorageDir);
    base::FilePath getLocalStorageDir();

    void setProxy(const mini_electron_proxy* proxy);
    const mini_electron_proxy* getProxy() const;

private:
    mini_electron_proxy* m_proxy = nullptr;

    WebCookieJarImpl* m_cookieJar;
    base::FilePath m_localStotageFullPath;
    base::FilePath m_downloadDirPath;
};

}

#endif // net_PageCookie_h