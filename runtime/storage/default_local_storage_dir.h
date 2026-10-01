
#ifndef MINI_ELECTRON_STORAGE_DEFAULT_LOCAL_STORAGE_DIR_H_
#define MINI_ELECTRON_STORAGE_DEFAULT_LOCAL_STORAGE_DIR_H_

#include "base/files/file_path.h"

namespace mini_electron {

base::FilePath migrateProfileDirectory(const base::FilePath& parent,
    const char* directory, const char* legacyDirectory);

base::FilePath getDefaultLocalStorageDir();
void setDefaultLocalStorageDir(const std::string& path);

}

#endif // MINI_ELECTRON_STORAGE_DEFAULT_LOCAL_STORAGE_DIR_H_