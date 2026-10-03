
#ifndef net_CookieJarMgr_h
#define net_CookieJarMgr_h

#include <string>
#include <map>
#include "base/synchronization/lock.h"

namespace mini_electron {

class WebCookieJarImpl;

class CookieJarMgr {
public:
    static CookieJarMgr* getInst()
    {
        static CookieJarMgr instance;
        m_inst = &instance;
        return m_inst;
    }

    WebCookieJarImpl* createOrGet(const std::string& fullPath);
    bool clear(const std::string& fullPath);

private:
    static CookieJarMgr* m_inst;
    std::map<std::string, WebCookieJarImpl*> m_pathToCookies;
    base::Lock m_lock;
};

}

#endif // net_CookieJarMgr_h