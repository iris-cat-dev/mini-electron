#include "platform/posix/win32/windows.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

struct PropertyKey {
  HWND window;
  std::u16string name;

  bool operator==(const PropertyKey& other) const {
    return window == other.window && name == other.name;
  }
};

struct PropertyKeyHash {
  size_t operator()(const PropertyKey& key) const {
    return std::hash<void*>()(key.window) ^
           (std::hash<std::u16string>()(key.name) << 1);
  }
};

std::mutex g_properties_lock;
std::unordered_map<PropertyKey, HANDLE, PropertyKeyHash> g_properties;
int g_window_sentinel;

}  // namespace

extern "C" {

HDC BeginPaint(HWND, LPPAINTSTRUCT paint) {
  if (paint)
    *paint = {};
  return nullptr;
}

BOOL EndPaint(HWND, const PAINTSTRUCT*) { return TRUE; }
BOOL ClientToScreen(HWND, LPPOINT) { return TRUE; }
BOOL ScreenToClient(HWND, LPPOINT) { return TRUE; }

HWND CreateWindowExW(DWORD,
                     LPCWSTR,
                     LPCWSTR,
                     DWORD,
                     int,
                     int,
                     int,
                     int,
                     HWND,
                     HMENU,
                     HINSTANCE,
                     LPVOID) {
  return reinterpret_cast<HWND>(&g_window_sentinel);
}

LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM) { return 0; }
BOOL DestroyWindow(HWND) { return TRUE; }

BOOL GetClientRect(HWND, LPRECT rect) {
  if (!rect)
    return FALSE;
  *rect = {0, 0, 0, 0};
  return TRUE;
}

BOOL GetCursorPos(POINT* point) {
  if (!point)
    return FALSE;
  *point = {0, 0};
  return TRUE;
}

DWORD GetFileSize(HANDLE, LPDWORD high) {
  if (high)
    *high = 0;
  return 0;
}

SHORT GetKeyState(int) { return 0; }
LONG GetWindowLongW(HWND, int) { return 0; }
BOOL IsWindow(HWND window) { return window != nullptr; }
HCURSOR LoadCursorW(HINSTANCE, LPCWSTR) {
  return reinterpret_cast<HCURSOR>(&g_window_sentinel);
}
BOOL PostMessageW(HWND, UINT, WPARAM, LPARAM) { return TRUE; }
ATOM RegisterClassExW(const WNDCLASSEXW*) { return 1; }

BOOL SetPropW(HWND window, LPCWSTR name, HANDLE data) {
  if (!name)
    return FALSE;
  std::lock_guard<std::mutex> lock(g_properties_lock);
  g_properties[{window, std::u16string(name)}] = data;
  return TRUE;
}

HANDLE GetPropW(HWND window, LPCWSTR name) {
  if (!name)
    return nullptr;
  std::lock_guard<std::mutex> lock(g_properties_lock);
  auto found = g_properties.find({window, std::u16string(name)});
  return found == g_properties.end() ? nullptr : found->second;
}

BOOL SetWindowPos(HWND, HWND, int, int, int, int, UINT) { return TRUE; }
BOOL SetWindowTextW(HWND, LPCWSTR) { return TRUE; }
BOOL ShowWindow(HWND, int) { return TRUE; }

int __xstat(int, const char* path, struct stat* result) {
  return fstatat(AT_FDCWD, path, result, 0);
}

int __fxstat(int, int fd, struct stat* result) {
  char path[1024];
  if (fcntl(fd, F_GETPATH, path) != 0)
    return -1;
  return fstatat(AT_FDCWD, path, result, 0);
}

}  // extern "C"

namespace base {

bool IsStringASCII(std::wstring_view value) {
  for (wchar_t character : value) {
    if (static_cast<unsigned int>(character) > 0x7f)
      return false;
  }
  return true;
}

}  // namespace base

namespace allocator_shim {

bool IsDefaultAllocatorPartitionRootInitialized() {
  return false;
}

}  // namespace allocator_shim
