// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef electron_browser_api_ApiBrowserView_h
#define electron_browser_api_ApiBrowserView_h

#include "runtime/electron/node_bindings.h"
#include "runtime/electron/browser/api/web_contents.h"
#include "runtime/electron/browser/api/window_interface.h"
#include "runtime/electron/browser/api/window_state.h"
#include "runtime/electron/common/api/event_emitter.h"
#include "runtime/electron/common/gin_helper/wrappable.h"
#include "runtime/electron/common/gin_helper/dictionary.h"
#include "runtime/engine/public/engine_api.h"
#include <windows.h>

namespace atom {

class WebContents;

class BrowserView : public mate::EventEmitter<BrowserView>, public WindowInterface, public WebContentsObserver {
public:
    BrowserView(v8::Isolate* isolate, v8::Local<v8::Object> wrapper);
    ~BrowserView();

    static void init(v8::Isolate* isolate, v8::Local<v8::Object> target);

    static void newFunction(const v8::FunctionCallbackInfo<v8::Value>& args);

    void destroyed();

    v8::Local<v8::Value> _getWebContentsApi();
    void _setBoundsApi(int x, int y, int w, int h);

    static BrowserView* newBrowserView(const gin_helper::Dictionary* options, v8::Local<v8::Object> wrapper);

    void attachBrowserWindow(HWND hWnd);
    void detachBrowserWindow();
    void handleMouseMsgInUiThread(unsigned int message, int xInParent, int yInParent, unsigned int flags);
    void onParentScaleFactorChanged();
    // WindowInterface
    bool isClosed() override;
    void close() override;
    void destroy() override;
    v8::Local<v8::Object> getWrapper() override;
    int getId() const override;
    WebContents* getWebContents() const override;
    HWND getHWND() const override;
    void onWebContentsPaint(WebContents*) override;
    void onWebContentsDeleted(WebContents*) override;

    RECT getClientRect() const
    {
        RECT r;
        ::EnterCriticalSection(&m_rectLock);
        r = m_clientRect;
        ::LeaveCriticalSection(&m_rectLock);
        return r;
    }

    void onPaintInUiThread(HDC hdc, const RECT& parentPaintRect);

    WebContents* m_webContents;
    WebContents::BrowserWindowConstructorOptions* m_createWindowParam;
    HWND m_hWnd;
    WindowState m_state;
    int m_id;

    mutable CRITICAL_SECTION m_rectLock;
    RECT m_boundsInDips;
    RECT m_clientRect;
    v8::Persistent<v8::Object> m_liveSelf;

    static gin::WrapperInfo kWrapperInfo;
    static v8::Persistent<v8::Function> constructor;
};

} // atom namespace

#endif // electron_browser_api_ApiBrowserView_h
