
#ifndef net_WebURLLoaderManagerSetupInfo_h
#define net_WebURLLoaderManagerSetupInfo_h

#include "third_party/libcurl/include/curl/curl.h"
#include "runtime/network/loader/flatten_http_body_element.h"

namespace mini_electron {

struct SetupDataInfo {
    CURLoption sizeOption = CURLOPT_POSTFIELDSIZE_LARGE;
    curl_off_t size = 0;
    bool chunkedTransfer = false;
    bool failed = false;
    std::vector<std::unique_ptr<FlattenHTTPBodyElement>> flattenElements;
};

struct SetupInfoBase {
    SetupDataInfo* data;

    SetupInfoBase()
    {
        data = nullptr;
    }

    ~SetupInfoBase()
    {
        if (data)
            delete data;
    }
};

struct SetupPutInfo : public SetupInfoBase { };

struct SetupPostInfo : public SetupInfoBase { };

struct SetupHttpMethodInfo {
    SetupHttpMethodInfo()
    {
        put = nullptr;
        post = nullptr;
    }

    ~SetupHttpMethodInfo()
    {
        if (put)
            delete put;
        if (post)
            delete post;
    }
    SetupPutInfo* put;
    SetupPostInfo* post;
    //SetupDeleteInfo* del;
};

}

#endif // net_WebURLLoaderManagerSetupInfo_h