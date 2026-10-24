// Copyright (c) 2015 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "src/runtime/electron/common/asar/asar_util.h"

#include <map>
#include <string>

#include "src/runtime/electron/common/asar/archive.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/strings/escape.h"
#include "base/synchronization/lock.h"

namespace asar {

namespace {

// Process-lifetime archive cache shared by the native filesystem consumers.
typedef std::map<base::FilePath, Archive*> ArchiveMap;
static ArchiveMap* g_archive_map = nullptr;
static base::Lock g_archive_map_lock;

const base::FilePath::CharType kAsarExtension[] = FILE_PATH_LITERAL(".asar");

} // namespace

Archive* getOrCreateAsarArchive(const base::FilePath& path)
{
    base::AutoLock locker(g_archive_map_lock);
    if (!g_archive_map)
        g_archive_map = new ArchiveMap();
    ArchiveMap& archive_map = *g_archive_map;
    ArchiveMap::iterator it = archive_map.find(path);
    if (it == archive_map.end()) {
        Archive* archive(new Archive(path));
        if (!archive->Init()) {
            delete archive;
            return nullptr;
        }
        archive_map[path] = archive;
    }
    return archive_map[path];
}

bool getAsarArchivePath(const base::FilePath& full_path, base::FilePath* asar_path, base::FilePath* relative_path)
{
    base::FilePath iter = full_path;
    while (true) {
        base::FilePath dirname = iter.DirName();
        if (iter.MatchesExtension(kAsarExtension))
            break;
        else if (iter == dirname)
            return false;
        iter = dirname;
    }
    if (base::DirectoryExists(iter))
        return false;

    base::FilePath tail;
    if (!iter.AppendRelativePath(full_path, &tail))
        return false;

    *asar_path = iter;
    *relative_path = tail;
    return true;
}

static bool readFileToStringImpl(const base::FilePath& path, std::string* contents)
{
    base::FilePath asar_path, relative_path;
    if (!getAsarArchivePath(path, &asar_path, &relative_path))
        return base::ReadFileToString(path, contents);

    Archive* archive = getOrCreateAsarArchive(asar_path);
    if (!archive)
        return false;

    Archive::FileInfo info;
    if (!archive->GetFileInfo(relative_path, &info))
        return false;

    if (info.unpacked) {
        base::FilePath real_path;
        // For unpacked file it will return the real path instead of doing the copy.
        archive->CopyFileOut(relative_path, &real_path);
        return base::ReadFileToString(real_path, contents);
    }

    base::File src(asar_path, base::File::FLAG_OPEN | base::File::FLAG_READ);
    if (!src.IsValid())
        return false;

    contents->resize(info.size);
    return static_cast<int>(info.size) == src.Read(info.offset, const_cast<char*>(contents->data()), contents->size());
}

bool readFileToString(const base::FilePath& path, std::string* contents)
{
    base::FilePath pathTemp = path;
    if (!readFileToStringImpl(pathTemp, contents) || 0 == contents->size()) {
        pathTemp = normalizeFile(pathTemp);
        if (!readFileToStringImpl(pathTemp, contents) || 0 == contents->size())
            return false;
    }
    return true;
}

// 把%xx这种符号替换成正常的
base::FilePath normalizeFile(const base::FilePath& path)
{
    if (base::PathExists(path))
        return path;

    std::string newPathStr = base::UnescapeURLComponent(std::string_view(path.AsUTF8Unsafe()), base::UnescapeRule::SPACES);
    return base::FilePath::FromUTF8Unsafe(newPathStr);
}

} // namespace asar
