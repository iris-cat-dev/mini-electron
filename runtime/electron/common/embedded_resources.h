
#ifndef MINI_ELECTRON_EMBEDDED_RESOURCES_H_
#define MINI_ELECTRON_EMBEDDED_RESOURCES_H_

#include <cstddef>
#include <string>
#include <vector>

namespace atom {

#define kEmbeddedResourcePrefix "mini-electron-resources"
#define kEmbeddedResourcePrefixW L"mini-electron-resources"

void setEmbeddedResourcePath(const std::string& path);
const std::string& getEmbeddedResourcePath();
bool checkEmbeddedResourceStat(const std::string& path, int* rc, std::string* result, std::size_t* size = nullptr);

}

#endif  