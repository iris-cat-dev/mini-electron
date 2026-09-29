#ifndef PLATFORM_MACOS_COMPAT_DARWIN_LEGACY_STAT_RENAME_H_
#define PLATFORM_MACOS_COMPAT_DARWIN_LEGACY_STAT_RENAME_H_

#include <stdlib.h>
#include <sys/stat.h>

// linux/linuxwindows.cpp implements glibc wrappers that must not replace
// Darwin's libc entry points when the engine and embedded Node share a binary.
#define atexit(...) mb_legacy_atexit(__VA_ARGS__)
#define fstat(...) mb_legacy_fstat(__VA_ARGS__)
#define fstat64(...) mb_legacy_fstat64(__VA_ARGS__)
#define lstat(...) mb_legacy_lstat(__VA_ARGS__)
#define lstat64(...) mb_legacy_lstat64(__VA_ARGS__)
#define stat(...) mb_legacy_stat(__VA_ARGS__)
#define stat64(...) mb_legacy_stat64(__VA_ARGS__)

#endif  // PLATFORM_MACOS_COMPAT_DARWIN_LEGACY_STAT_RENAME_H_
