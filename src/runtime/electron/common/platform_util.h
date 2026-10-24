// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ATOM_COMMON_PLATFORM_UTIL_H_
#define ATOM_COMMON_PLATFORM_UTIL_H_

#include <string>

#include "build/build_config.h"

#if defined(OS_WIN)
#include <windows.h>
#include <xstring>
#endif

class GURL;
class SkBitmap;

namespace base {
class FilePath;
}

namespace platform_util {

// Show the given file in a file manager. If possible, select the file.
// Must be called from the UI thread.
void showItemInFolder(const base::FilePath& full_path);

// Open the given file in the desktop's default manner.
// Must be called from the UI thread.
#if defined(OS_WIN)
// Returns true when Windows accepted the open request. On failure, |error|
// receives a user-readable description.
bool openPath(const base::FilePath& full_path, std::string* error);
#else
void openItem(const base::FilePath& full_path);
#endif

// Open the given external protocol URL in the desktop's default manner.
// (For example, mailto: URLs in the default mail user agent.)
#if defined(OS_WIN)
// Returns true when Windows accepted the open request. On failure, |error|
// receives a user-readable description.
bool openExternal(const std::u16string& url, bool activate, std::string* error);
#else
bool openExternal(const GURL& url, bool activate);
#endif

#if defined(OS_WIN)
void moveToCenter(HWND hWnd);
#endif

// Move a file to trash.
bool moveItemToTrash(const base::FilePath& full_path);

void beep();

bool parseBMPToSkBitmap(const uint8_t* bmpData, size_t bmpSize, SkBitmap* outBitmap);
bool loadIconFromICOToSkBitmap(const uint8_t* data, size_t dataSize, SkBitmap* outBitmap);

void* loadIconFromMemory(const uint8_t* pData, size_t dwSize, void* hIcon);
void loadIconFromMemoryFree(void* picture);

} // namespace platform_util

#endif // ATOM_COMMON_PLATFORM_UTIL_H_
