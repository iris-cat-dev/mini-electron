
#include "runtime/network/cookies/cookie_jar_mgr.h"
#include "runtime/network/cookies/web_cookie_jar_curl_impl.h"
#include "third_party/libcurl/include/curl/curl.h"

namespace mini_electron {

CookieJarMgr* CookieJarMgr::m_inst = nullptr;

WebCookieJarImpl* CookieJarMgr::createOrGet(const std::string& fullPath)
{
    base::AutoLock lock(m_lock);
    WebCookieJarImpl* cookiejar = nullptr;
    std::map<std::string, WebCookieJarImpl*>::iterator it = m_pathToCookies.find(fullPath);
    if (m_pathToCookies.end() != it)
        return it->second;

    cookiejar = WebCookieJarImpl::create(fullPath);
    m_pathToCookies.insert(std::pair<std::string, WebCookieJarImpl*>(fullPath, cookiejar));
    return cookiejar;
}

bool CookieJarMgr::clear(const std::string& fullPath)
{
    WebCookieJarImpl* jar = createOrGet(fullPath);
    if (!jar)
        return false;
    CURL* handle = curl_easy_init();
    if (!handle)
        return false;
    curl_easy_setopt(handle, CURLOPT_SHARE, jar->getCurlShareHandle());
    CURLcode clearResult = curl_easy_setopt(handle, CURLOPT_COOKIELIST, "ALL");
    curl_easy_setopt(handle, CURLOPT_COOKIEJAR, fullPath.c_str());
    CURLcode flushResult = curl_easy_setopt(handle, CURLOPT_COOKIELIST, "FLUSH");
    curl_easy_cleanup(handle);
    return clearResult == CURLE_OK && flushResult == CURLE_OK;
}

}