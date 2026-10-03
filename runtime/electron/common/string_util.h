
#ifndef atom_StringUtil_h
#define atom_StringUtil_h

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"

namespace atom {

class StringUtil {
public:
    static std::wstring UTF8ToUTF16(const std::string& utf8)
    {
        return base::UTF8ToWide(utf8);
    }

    static std::wstring MultiByteToUTF16(int, const std::string& value)
    {
        return base::UTF8ToWide(value);
    }

    static std::string UTF16ToUTF8(const std::wstring& utf16)
    {
        return base::WideToUTF8(utf16);
    }

    static std::string urlDecode(const char* pszEncodedIn, size_t pszEncodedInLen)
    {
        size_t nBufferSize = pszEncodedInLen * 2;
        char* pszDecodedOut = (char*)malloc(nBufferSize);
        memset(pszDecodedOut, 0, nBufferSize);

        enum DecodeState {
            STATE_SEARCH = 0, ///< searching for an ampersand to convert
            STATE_CONVERTING, ///< convert the two proceeding characters from hex
        };

        DecodeState state = STATE_SEARCH;

        for (unsigned int i = 0; i < pszEncodedInLen - 1; ++i) {
            switch (state) {
            case STATE_SEARCH: {
                if (pszEncodedIn[i] != '%') {
                    strncat(pszDecodedOut, &pszEncodedIn[i], 1);
                    //assert(strlen(pszDecodedOut) < nBufferSize);
                    break;
                }

                // We are now converting
                state = STATE_CONVERTING;
            } break;

            case STATE_CONVERTING: {
                // Conversion complete (i.e. don't convert again next iter)
                state = STATE_SEARCH;

                // Create a buffer to hold the hex. For example, if %20, this
                // buffer would hold 20 (in ASCII)
                char pszTempNumBuf[3] = { 0 };
                strncpy(pszTempNumBuf, &pszEncodedIn[i], 2);

                // Ensure both characters are hexadecimal
                bool bBothDigits = true;

                for (int j = 0; j < 2; ++j) {
                    if (!isxdigit(pszTempNumBuf[j]))
                        bBothDigits = false;
                }

                if (!bBothDigits)
                    break;

                // Convert two hexadecimal characters into one character
                int nAsciiCharacter;
                sscanf(pszTempNumBuf, "%x", &nAsciiCharacter);

                // Ensure we aren't going to overflow
                //assert(strlen(pszDecodedOut) < nBufferSize);

                // Concatenate this character onto the output
                strncat(pszDecodedOut, (char*)&nAsciiCharacter, 1);

                // Skip the next character
                i++;
            } break;
            }
        }
        std::string strDecodedOut(pszDecodedOut);
        free(pszDecodedOut);
        return strDecodedOut;
    }

    static unsigned int hashString(const char* p)
    {
        int prime = 25013;
        unsigned int h = 0;
        unsigned int g;
        for (; *p; p++) {
            h = (h << 4) + *p;
            g = h & 0xF0000000;
            if (g) {
                h ^= (g >> 24);
                h ^= g;
            }
        }
        return h % prime;
    }

    std::vector<std::string> splitstr(const std::string& str, char tag)
    {
        std::vector<std::string> li;
        std::string subStr;

        for (size_t i = 0; i < str.length(); i++) {
            if (tag == str[i]) {
                if (!subStr.empty()) {
                    li.push_back(subStr);
                    subStr.clear();
                }
            } else {
                subStr.push_back(str[i]);
            }
        }

        if (!subStr.empty())
            li.push_back(subStr);

        return li;
    }

    static std::string normalizePath(const std::string& path)
    {
#if BUILDFLAG(IS_WIN)
        std::string result;
        result.reserve(path.size());
        for (char value : path) {
            char c = value;
            if (c >= 'A' && c <= 'Z')
                c += ('a' - 'A');
            if (c == '/')
                c = '\\';
            result += c;
        }
        const char prefix[] = "file:\\\\\\";
        if (result.size() >= sizeof(prefix) - 1 &&
            result.compare(0, sizeof(prefix) - 1, prefix) == 0)
            result.erase(0, sizeof(prefix) - 1);
        return result;
#else
        return path;
#endif
    }

    //     static void readJsFile(const wchar_t* path, std::vector<char>* buffer)
    //     {
    //         HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    //         if (INVALID_HANDLE_VALUE == hFile) {
    //             DebugBreak();
    //             return;
    //         }
    //
    //         DWORD fileSizeHigh;
    //         const DWORD bufferSize = ::GetFileSize(hFile, &fileSizeHigh);
    //
    //         DWORD numberOfBytesRead = 0;
    //         buffer->resize(bufferSize);
    //         BOOL b = ::ReadFile(hFile, &buffer->at(0), bufferSize, &numberOfBytesRead, nullptr);
    //         ::CloseHandle(hFile);
    //         b = b;
    //     }
};

} // atom

#endif // atom_StringUtil_h