
#include "runtime/network/loader/page_net_extra_data.h"
#include "runtime/network/loader/web_url_loader_manager_util.h"
#include "runtime/network/cookies/web_cookie_jar_curl_impl.h"
#include "runtime/network/cookies/cookie_jar_mgr.h"
#include "runtime/storage/default_local_storage_dir.h"
#include "third_party/blink/renderer/platform/wtf/threading_primitives.h"
#include "runtime/engine/public/engine_api.h"
#include "base/files/file_util.h"
#include <shlwapi.h>

namespace mini_electron {

PageNetExtraData::PageNetExtraData()
{
    m_cookieJar = nullptr;
}

PageNetExtraData::~PageNetExtraData()
{
    // 不删除了，因为是CookieJarMgr管理的
    //     if (m_cookieJar)
    //         delete m_cookieJar;

    if (m_proxy)
        delete m_proxy;
}

base::FilePath PageNetExtraData::getDownloadDirPath()
{
    if (m_downloadDirPath.empty()) {
        base::FilePath downloadPath = getLocalStorageDir();
        downloadPath = downloadPath.AppendASCII("download");
        base::CreateDirectory(downloadPath); // 在local storage里加个下载目录
        m_downloadDirPath = downloadPath;
    }
    return m_downloadDirPath;
}

void PageNetExtraData::setCookieJarFullPath(const std::string& fullPathUtf8)
{
    WTF::RecursiveMutex* mutex = sharedResourceMutex(CURL_LOCK_DATA_COOKIE);
    WTF::Locker<WTF::RecursiveMutex> locker(*mutex);

    if (m_cookieJar) {
        OutputDebugStringA("PageNetExtraData::setCookieJarPath has been set");
        return;
    }

    WebCookieJarImpl* cookieJar = CookieJarMgr::getInst()->createOrGet(fullPathUtf8);
    m_cookieJar = cookieJar;
}

CURLSH* PageNetExtraData::getCurlShareHandle()
{
    if (m_cookieJar)
        return m_cookieJar->getCurlShareHandle();
    return nullptr;
}

std::string PageNetExtraData::getCookieJarFullPath()
{
    if (m_cookieJar)
        return m_cookieJar->getCookieJarFullPath();
    return "";
}

base::FilePath PageNetExtraData::getLocalStorageDir()
{
    WTF::RecursiveMutex* mutex = sharedResourceMutex(CURL_LOCK_DATA_COOKIE);
    WTF::Locker<WTF::RecursiveMutex> locker(*mutex);

    if (!m_localStotageFullPath.empty())
        return m_localStotageFullPath;
    m_localStotageFullPath = getDefaultLocalStorageDir();
    return m_localStotageFullPath;
}

void PageNetExtraData::setLocalStorageDir(const std::string& localStorageDir)
{
    WTF::RecursiveMutex* mutex = sharedResourceMutex(CURL_LOCK_DATA_COOKIE);
    WTF::Locker<WTF::RecursiveMutex> locker(*mutex);

    if (!m_localStotageFullPath.empty()) {
        OutputDebugStringA("PageNetExtraData::setStorageAreaFullPath has been set");
        return;
    }
    m_localStotageFullPath = base::FilePath::FromUTF8Unsafe(localStorageDir);
}

void PageNetExtraData::setProxy(const mini_electron_proxy* proxy)
{
    if (m_proxy)
        delete m_proxy;
    m_proxy = (mini_electron_proxy*)proxy;
}

const mini_electron_proxy* PageNetExtraData::getProxy() const
{
    return m_proxy;
}

}