// Copyright 2026 The mini-electron Authors
// Use of this source code is governed by the Apache-2.0 license.

#ifndef MINI_ELECTRON_ENGINE_PUBLIC_PLATFORM_TYPES_H_
#define MINI_ELECTRON_ENGINE_PUBLIC_PLATFORM_TYPES_H_

#include <stdint.h>

#ifdef __cplusplus
using mb_wchar_t = char16_t;
#else
#include <uchar.h>
typedef char16_t mb_wchar_t;
#endif

#if defined(INSIDE_BLINK) && !defined(_WIN32)
#include "platform/posix/win32/windows.h"
#else
typedef int32_t BOOL;
typedef uint32_t DWORD;
typedef uint32_t UINT;
typedef struct _STARTUPINFOW STARTUPINFOW;
typedef struct tagRECT {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} RECT;
typedef void* HANDLE;
typedef void* HDC;
typedef void* HMODULE;
typedef void* HWND;
typedef intptr_t LPARAM;
typedef intptr_t LRESULT;
typedef uintptr_t WPARAM;
typedef mb_wchar_t WCHAR;
#endif

#ifndef FALSE
#define FALSE 0
#endif

#ifndef TRUE
#define TRUE 1
#endif

#endif  // MINI_ELECTRON_ENGINE_PUBLIC_PLATFORM_TYPES_H_
