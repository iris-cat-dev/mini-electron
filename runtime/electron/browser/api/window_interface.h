
#ifndef browser_api_WindowInterface_h
#define browser_api_WindowInterface_h

#include <v8.h>
#include <string>
#include "base/values.h"
#if defined(_WIN32)
typedef struct HWND__* HWND;
typedef struct HMENU__* HMENU;
#else
// POSIX hosts use the opaque Win32-shaped handles from platform/posix/win32.
#include <windows.h>
#endif
using NativeWindowHandle = void*;

namespace atom {

class WebContents;

class WindowInterface {
public:
    virtual bool isClosed() = 0;
    virtual void destroy() = 0;
    virtual void close() = 0;
    virtual v8::Local<v8::Object> getWrapper() = 0;
    virtual int getId() const = 0;
    virtual WebContents* getWebContents() const = 0;
    virtual HWND getHWND() const { return nullptr; }
    // Portable window handle used by shared WebContents/platform adapters.
    // Windows subclasses may keep overriding getHWND() for existing Win32 APIs.
    virtual NativeWindowHandle getNativeWindowHandle() const
    {
        return reinterpret_cast<NativeWindowHandle>(getHWND());
    }
    // False means this embedding cannot perform the requested native action.
    virtual bool handleNativeWindowCommand(
        const std::string&, const base::Value::Dict&) { return false; }
#if defined(_WIN32)
    virtual void setNativeMenu(HMENU menu);
#else
    // Non-Windows hosts install application menus through their own Menu binding.
    virtual void setNativeMenu(HMENU) { }
#endif
    static v8::Local<v8::Value> getFocusedWindow(v8::Isolate* isolate);
    static v8::Local<v8::Value> getFocusedContents(v8::Isolate* isolate);

    static WebContents* onCreateNewWebview(v8::Local<v8::Object>);

    static const wchar_t kElectronClassName[];
    static const int kSingleInstanceMessage = 0x410;
};

} // atom

#endif // browser_api_WindowInterface_h