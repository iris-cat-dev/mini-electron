
#ifndef browser_api_WindowInterface_h
#define browser_api_WindowInterface_h

#include <v8.h>
typedef struct HWND__* HWND;
typedef struct HMENU__* HMENU;
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
    virtual void setNativeMenu(HMENU menu);
    static v8::Local<v8::Value> getFocusedWindow(v8::Isolate* isolate);
    static v8::Local<v8::Value> getFocusedContents(v8::Isolate* isolate);

    static WebContents* onCreateNewWebview(v8::Local<v8::Object>);

    static const wchar_t kElectronClassName[];
    static const int kSingleInstanceMessage = 0x410;
};

} // atom

#endif // browser_api_WindowInterface_h