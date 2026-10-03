// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "runtime/electron/common/asar/scoped_temporary_file.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"

namespace {

constexpr size_t kMaxExtensionReservationAttempts = 64;

bool CreateAsarTemporaryFile(base::FilePath* path)
{
    base::FilePath temp_dir;
    return base::GetTempDir(&temp_dir)
        && base::CreateTemporaryFileInDir(temp_dir, path);
}

}

namespace asar {

ScopedTemporaryFile::ScopedTemporaryFile()
{
}

ScopedTemporaryFile::~ScopedTemporaryFile()
{
    // Loaded native modules and other open files remain protected by Windows'
    // normal sharing rules. Delete every owned extraction that is no longer in
    // use instead of leaking all non-native ASAR reads for the process lifetime.
    if (!path_.empty())
        base::DeleteFile(path_);

    if (!reservation_path_.empty())
        base::DeleteFile(reservation_path_);
}

bool ScopedTemporaryFile::Init(const base::FilePath::StringType& ext)
{
    if (!path_.empty())
        return true;

#if defined(OS_WIN)
    if (!ext.empty()) {

        for (size_t attempt = 0; attempt < kMaxExtensionReservationAttempts; ++attempt) {
            base::FilePath reservation_path;
            if (!CreateAsarTemporaryFile(&reservation_path)) {
                return false;
            }

            // Keep reservation_path present continuously. The extension-bearing
            // path is also created exclusively so concurrent extractions cannot
            // claim the same file.
            base::FilePath candidate_path = reservation_path.AddExtension(ext);
            base::File candidate(
                candidate_path, base::File::FLAG_CREATE | base::File::FLAG_WRITE);
            if (candidate.IsValid()) {
                reservation_path_ = std::move(reservation_path);
                path_ = std::move(candidate_path);
                return true;
            }

            base::DeleteFile(reservation_path);
            if (candidate.error_details() != base::File::FILE_ERROR_EXISTS)
                return false;
        }
        return false;
    }
#endif

    return CreateAsarTemporaryFile(&path_);
}

bool ScopedTemporaryFile::InitFromFile(base::File* src, const base::FilePath::StringType& ext, uint64_t offset, uint64_t size)
{
    if (!src->IsValid())
        return false;

    if (!Init(ext))
        return false;

    std::vector<char> buf(static_cast<size_t>(size));
    size_t bytes_read = 0;
    while (bytes_read < buf.size()) {
        const size_t remaining = buf.size() - bytes_read;
        const int requested = static_cast<int>(std::min(
            remaining, static_cast<size_t>(std::numeric_limits<int>::max())));
        const int len = src->Read(
            offset + bytes_read, buf.data() + bytes_read, requested);
        if (len <= 0)
            return false;
        bytes_read += static_cast<size_t>(len);
    }

    base::File dest(path_, base::File::FLAG_OPEN | base::File::FLAG_WRITE);
    if (!dest.IsValid())
        return false;

    size_t bytes_written = 0;
    while (bytes_written < buf.size()) {
        const size_t remaining = buf.size() - bytes_written;
        const int requested = static_cast<int>(std::min(
            remaining, static_cast<size_t>(std::numeric_limits<int>::max())));
        const int len = dest.WriteAtCurrentPos(
            buf.data() + bytes_written, requested);
        if (len <= 0)
            return false;
        bytes_written += static_cast<size_t>(len);
    }
    return true;
}

} // namespace asar
