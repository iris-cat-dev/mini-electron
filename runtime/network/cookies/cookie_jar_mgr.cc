
#include "runtime/network/cookies/cookie_jar_mgr.h"
#include "runtime/network/cookies/web_cookie_jar_curl_impl.h"

namespace mini_electron {

CookieJarMgr* CookieJarMgr::m_inst = nullptr;

WebCookieJarImpl* CookieJarMgr::createOrGet(const std::string& fullPath)
{
    WebCookieJarImpl* cookiejar = nullptr;
    std::map<std::string, WebCookieJarImpl*>::iterator it = m_pathToCookies.find(fullPath);
    if (m_pathToCookies.end() != it)
        return it->second;

    cookiejar = WebCookieJarImpl::create(fullPath);
    m_pathToCookies.insert(std::pair<std::string, WebCookieJarImpl*>(fullPath, cookiejar));
    return cookiejar;
}

}