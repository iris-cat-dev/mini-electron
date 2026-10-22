// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.
#ifndef MINI_UI_RUNTIME_UI_LOCAL_RESOURCES_H_
#define MINI_UI_RUNTIME_UI_LOCAL_RESOURCES_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file.h"
#include "mini_ui/ui.h"

namespace mini_ui {

struct Resource {
    std::vector<std::uint8_t> bytes;
    std::string mime_type;
    std::string url;
};

class LocalResources final {
public:
    static std::unique_ptr<LocalResources> Open(
        const std::filesystem::path& root, Error* error = nullptr);
    ~LocalResources();

    LocalResources(const LocalResources&) = delete;
    LocalResources& operator=(const LocalResources&) = delete;
    LocalResources(LocalResources&&) = delete;
    LocalResources& operator=(LocalResources&&) = delete;

    bool Read(std::string_view url, std::string_view base_url,
        Resource* out, Error* error = nullptr) const;
    // Native absolute paths only; metadata comes from the same checked open as
    // Read, without allocating a content buffer. Directories are permitted.
    bool GetFileInfo(const base::FilePath&, base::File::Info*) const;
    // Native absolute paths only. Returns an owned readable handle validated by
    // OpenChecked; callers must retain this handle rather than reopen the path.
    base::File OpenFile(const base::FilePath&, Error* error = nullptr) const;
    const std::filesystem::path& root() const { return root_; }

private:
    struct NativeRoot;

    LocalResources(std::filesystem::path root,
        std::unique_ptr<NativeRoot> native_root);
    bool OpenChecked(const std::vector<std::string>& segments, bool metadata,
        base::File* file, std::filesystem::path* final_path,
        Error* error) const;

    std::filesystem::path root_;
    std::unique_ptr<NativeRoot> native_root_;
};

} // namespace mini_ui

#endif // MINI_UI_RUNTIME_UI_LOCAL_RESOURCES_H_
