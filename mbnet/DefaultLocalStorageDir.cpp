
#include "mbnet/DefaultLocalStorageDir.h"
#include "third_party/libcurl/include/curl/curl.h"
#include "mbnet/WebURLLoaderManagerUtil.h"
#include "third_party/blink/renderer/platform/wtf/threading_primitives.h"
#include "base/base_paths_mac.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_WIN)
#include <shlwapi.h>
#elif BUILDFLAG(IS_LINUX)
#include <unistd.h>
#endif

namespace mbnet {

static base::FilePath* kDefaultLocalStorageDir = nullptr;

void setDefaultLocalStorageDir(const std::string& path)
{
    WTF::RecursiveMutex* mutex = sharedResourceMutex(CURL_LOCK_DATA_COOKIE);
    WTF::Locker<WTF::RecursiveMutex> locker(*mutex);

    if (path.empty())
        return;

    if (kDefaultLocalStorageDir)
        delete kDefaultLocalStorageDir;
    kDefaultLocalStorageDir = new base::FilePath(base::FilePath::FromUTF8Unsafe(path));

    //     if (kDefaultLocalStorageDir->empty()) {
    //         delete kDefaultLocalStorageDir;
    //         kDefaultLocalStorageDir = nullptr;
    //         return;
    //     }
    //
    //     if (!base::EndsWith(*kDefaultLocalStorageDir, "\\"))
    //         kDefaultLocalStorageDir->push_back('\\');
}

base::FilePath getDefaultLocalStorageDir()
{
    WTF::RecursiveMutex* mutex = sharedResourceMutex(CURL_LOCK_DATA_COOKIE);
    WTF::Locker<WTF::RecursiveMutex> locker(*mutex);

    if (kDefaultLocalStorageDir)
        return *kDefaultLocalStorageDir;

#if BUILDFLAG(IS_WIN)
    std::vector<WCHAR> path;
    path.resize(2 * (MAX_PATH + 1));
    memset(&path.at(0), 0, sizeof(WCHAR) * (2 * (MAX_PATH + 1)));
    ::GetModuleFileNameW(nullptr, &path.at(0), MAX_PATH);
    ::PathRemoveFileSpecW(&path.at(0));
    ::PathAppendW(&path.at(0), L"LocalStorage");

    size_t size = wcslen(&path.at(0));

    kDefaultLocalStorageDir = new base::FilePath(std::wstring(path.data(), size));
#elif BUILDFLAG(IS_MAC)
    base::FilePath appData;
    if (!base::PathService::Get(base::DIR_APP_DATA, &appData))
        return base::FilePath();
    kDefaultLocalStorageDir = new base::FilePath(
        appData.AppendASCII("miniblink132").AppendASCII("LocalStorage"));
#else
    std::vector<char> buf(8025, 0);
    int n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0)
        return base::FilePath();
    kDefaultLocalStorageDir =
        new base::FilePath(base::BasicStringPiece(buf.data(), n));
    *kDefaultLocalStorageDir = kDefaultLocalStorageDir->DirName();
    *kDefaultLocalStorageDir =
        kDefaultLocalStorageDir->AppendASCII("LocalStorage");
#endif
    //     UChar c = kDefaultLocalStorageDir->characters16()[size - 1];
    //     if (L'\\' != c && L'/' != c)
    //         kDefaultLocalStorageDir->append(L'\\');

    return *kDefaultLocalStorageDir;
}

}