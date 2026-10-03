// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef RUNTIME_STORAGE_STOCK_PROFILE_MIGRATOR_H_
#define RUNTIME_STORAGE_STOCK_PROFILE_MIGRATOR_H_

#include <string>

#include "base/files/file_path.h"

namespace mini_electron {

// Imports stock Electron cookie/localStorage formats into mini-electron's
// profile formats. Source databases are opened read-only or through private
// snapshots and are never modified or deleted. Returns false on a
// present-but-unsupported format rather than silently accepting an empty
// migration.
bool MigrateStockElectronProfile(const base::FilePath& user_data_root,
    const base::FilePath& profile_path, std::string* error);

} // namespace mini_electron

#endif // RUNTIME_STORAGE_STOCK_PROFILE_MIGRATOR_H_
