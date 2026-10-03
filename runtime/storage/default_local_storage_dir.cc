
#include "runtime/storage/default_local_storage_dir.h"
#include "third_party/libcurl/include/curl/curl.h"
#include "runtime/network/loader/web_url_loader_manager_util.h"
#include "third_party/blink/renderer/platform/wtf/threading_primitives.h"
#include "base/base_paths_mac.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_WIN)
#include <windows.h>
#include <shlwapi.h>
#elif BUILDFLAG(IS_MAC)
#include <stdio.h>
#elif BUILDFLAG(IS_LINUX)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace mini_electron {

static base::FilePath* kDefaultLocalStorageDir = nullptr;

namespace {

bool RenameExclusively(const base::FilePath& source,
    const base::FilePath& destination)
{
#if BUILDFLAG(IS_WIN)
    return ::MoveFileW(source.value().c_str(), destination.value().c_str()) != 0;
#elif BUILDFLAG(IS_MAC)
    return renamex_np(source.value().c_str(), destination.value().c_str(),
               RENAME_EXCL) == 0;
#elif BUILDFLAG(IS_LINUX) && defined(SYS_renameat2)
    constexpr unsigned int kRenameNoReplace = 1;
    return syscall(SYS_renameat2, AT_FDCWD, source.value().c_str(),
               AT_FDCWD, destination.value().c_str(),
               kRenameNoReplace) == 0;
#else
    return false;
#endif
}

} // namespace

base::FilePath migrateProfileDirectory(const base::FilePath& parent,
    const char* directory, const char* legacyDirectory)
{
    base::FilePath current = parent.AppendASCII(directory);
    base::FilePath legacy = parent.AppendASCII(legacyDirectory);
    if (!base::PathExists(current) && base::DirectoryExists(legacy)) {
        base::FileEnumerator probe(
            legacy, true, base::FileEnumerator::FILES);
        if (probe.Next().empty()) {
            LOG(WARNING) << "Legacy profile is empty; retaining " << legacy;
            return legacy;
        }

        // Keep an interrupted copy non-authoritative. The staging container is
        // a sibling, so publishing its profile tree is an atomic same-volume rename.
        base::ScopedTempDir stagingContainer;
        if (!stagingContainer.CreateUniqueTempDirUnderPath(parent)) {
            LOG(ERROR) << "Cannot create profile migration staging directory; "
                       << "retaining " << legacy;
            return legacy;
        }
        base::FilePath staging =
            stagingContainer.GetPath().AppendASCII("profile");
        if (!base::CopyDirectory(legacy, staging, true)) {
            LOG(ERROR) << "Cannot copy legacy profile; retaining " << legacy;
            return legacy;
        }
        if (!base::DirectoryExists(staging)) {
            LOG(ERROR) << "Profile copy produced no readable staging directory; "
                       << "retaining " << legacy;
            return legacy;
        }

        // Exclusive publication preserves a destination created by another
        // process while this copy was in progress.
        if (!RenameExclusively(staging, current)) {
            if (base::PathExists(current))
                return current;
            LOG(ERROR) << "Cannot publish migrated profile; retaining " << legacy;
            return legacy;
        }
    }
    return current;
}

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
        migrateProfileDirectory(appData, "mini-electron", "miniblink132")
            .AppendASCII("LocalStorage"));
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