
#ifndef download_DownloadUtil_h
#define download_DownloadUtil_h

#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "src/runtime/engine/common/string_util.h"
#include "src/runtime/engine/common/url_util.h"

namespace download {

static int fromHex(unsigned char x)
{
    if (x >= 'A' && x <= 'F')
        return x - 'A' + 10;
    if (x >= 'a' && x <= 'f')
        return x - 'a' + 10;
    if (x >= '0' && x <= '9')
        return x - '0';
    return -1;
}

static std::string urlDecode(const std::string& str)
{
    std::string strTemp = "";
    size_t length = str.length();
    for (size_t i = 0; i < length; i++) {
        if (str[i] == '+')
            strTemp += ' ';
        else if (str[i] == '%') {
            int high = i + 2 < length ? fromHex(str[i + 1]) : -1;
            int low = i + 2 < length ? fromHex(str[i + 2]) : -1;
            if (high < 0 || low < 0) {
                strTemp += str[i];
                continue;
            }
            strTemp += static_cast<char>(high * 16 + low);
            i += 2;
        } else
            strTemp += str[i];
    }
    return strTemp;
}

// attachment; filename="11??1.docx"; filename*=UTF-8''11%E6%B5%8B%E8%AF%951.docx
static std::u16string getSaveNameFromContentDisposition(const std::string& str)
{
    if (0 == str.size())
        return std::u16string();

    if (std::string::npos == str.find("attachment"))
        return std::u16string();

    if (std::string::npos == str.find("name"))
        return std::u16string();

    size_t pos1 = str.find("UTF-8''");
    if (std::string::npos == pos1) {
        pos1 = str.find('=');
        if (std::string::npos == pos1)
            return std::u16string();
        pos1++;
    } else {
        pos1 += 7; // UTF-8''
    }

    size_t pos2 = str.size();
    std::string path = str.substr(pos1, pos2 - pos1);
    if (path.empty())
        return std::u16string();

    if ('"' == path[0])
        path.erase(0, 1);
    if (path.empty())
        return std::u16string();

    //     if ('"' == path[path.size() - 1])
    //         path.erase(path.size() - 1, 1);
    size_t doubleQuotationPos = path.find('"'); // 文件名不允许有双引号
    if (std::string::npos != doubleQuotationPos) {
        path.erase(doubleQuotationPos);
    }

    if (path.empty())
        return std::u16string();

    path = urlDecode(path);
    return content::mulByteToUtf16(path, content::isTextUTF8(path.c_str(), path.size()) ? CP_UTF8 : CP_ACP);
}

static std::u16string getSaveName(std::string contentDisposition, std::string url)
{
    std::u16string result = getSaveNameFromContentDisposition(contentDisposition);
    if (!result.empty())
        return result;
    result = content::UrlUtil::getSaveNameFromUrl(url);

    String str = blink::DecodeURLEscapeSequences(WTF::String(base::span<const UChar>((const UChar*)result.c_str(), result.size())), url::DecodeURLMode::kUTF8);
    result = content::utf8ToUtf16(str.Utf8());

    return result;
}

}

#endif // download_DownloadUtil_h