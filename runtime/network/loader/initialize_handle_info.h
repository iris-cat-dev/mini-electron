
#ifndef net_InitializeHandleInfo_h
#define net_InitializeHandleInfo_h

#include "runtime/network/loader/proxy_type.h"
#include "runtime/network/loader/page_net_extra_data.h"
#include "third_party/libcurl/include/curl/curl.h"
#include "base/memory/scoped_refptr.h"
#include <string>

namespace mini_electron {

struct SetupHttpMethodInfo;

struct InitializeHandleInfo {
    std::string url;
    std::string method;
    curl_slist* headers;
    scoped_refptr<PageNetExtraData> pageNetExtraData;
    //CURLSH* pageCurlSH = nullptr;
    std::string proxy;
    std::string proxyUserNamePassword;

    std::string range;
    std::string networkInterface;
    ProxyType proxyType;
    SetupHttpMethodInfo* methodInfo;

    InitializeHandleInfo()
    {
        methodInfo = nullptr;
    }

    ~InitializeHandleInfo();
};

}

#endif // net_InitializeHandleInfo_h