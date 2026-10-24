#ifndef MINI_ELECTRON_UTF16_H_
#define MINI_ELECTRON_UTF16_H_


// ugly hack to make UChar compatible with JSChar in API/JSStringRef.h
#if defined(WIN32) // || defined(Q_OS_WIN) || COMPILER(WINSCW) || (COMPILER(RVCT) && !OS(LINUX))
typedef char16_t UChar;
#define MINI_ELECTRON_U16(x) L##x
#else
#define MINI_ELECTRON_U16(x) u##x
//typedef uint16_t UChar;
typedef char16_t UChar;
#endif // WIN32

//typedef uint32_t UChar32;
typedef int32_t UChar32;

inline size_t u16len(const UChar* s)
{
    size_t i = 0;
    for (; s[i]; ++i) {
    }
    return i;
}
#endif  