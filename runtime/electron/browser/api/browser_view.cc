// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "runtime/electron/browser/api/browser_view.h"

#include "runtime/electron/node_bindings.h"
#include "runtime/electron/common/node_register_help.h"
#include "runtime/electron/common/api/event_emitter.h"
#include "runtime/electron/common/options_switches.h"
#include "runtime/electron/common/id_live_detect.h"
#include "runtime/electron/common/gin_helper/object_template_builder.h"
#include "runtime/electron/common/gin_helper/wrappable.h"
#include "runtime/electron/common/gin_helper/dictionary.h"
#include "third_party/libnode/src/node.h"
#include "third_party/libnode/src/node_binding.h"
#include "third_party/libuv/include/uv.h"
#include "ui/display/win/screen_win.h"
#include "ui/gfx/geometry/rect.h"

namespace atom {

BrowserView::BrowserView(v8::Isolate* isolate, v8::Local<v8::Object> wrapper)
{
    gin_helper::Wrappable<BrowserView>::InitWith(isolate, wrapper);
    m_state = WindowUninited;
    m_webContents = nullptr;
    m_createWindowParam = nullptr;
    m_hWnd = nullptr;
    m_boundsInDips = { 0, 0, 0, 0 };
    m_clientRect = { 0, 0, 0, 0 };
    m_id = IdLiveDetect::get()->constructed(this);

    ::InitializeCriticalSection(&m_rectLock);
}

BrowserView::~BrowserView()
{
    destroy();
    delete m_createWindowParam;
    ::DeleteCriticalSection(&m_rectLock);
    IdLiveDetect::get()->deconstructed(m_id);
}

void BrowserView::init(v8::Isolate* isolate, v8::Local<v8::Object> target)
{
    v8::Local<v8::Context> context = isolate->GetCurrentContext();
    const char* className = "BrowserView";
    v8::Local<v8::FunctionTemplate> prototype = v8::FunctionTemplate::New(isolate, newFunction);

    prototype->SetClassName(v8::String::NewFromUtf8(isolate, className).ToLocalChecked());
    gin_helper::ObjectTemplateBuilder builder(isolate, prototype->InstanceTemplate());
    builder.SetMethod("_getWebContents", &BrowserView::_getWebContentsApi);
    builder.SetMethod("_setBounds", &BrowserView::_setBoundsApi);

    constructor.Reset(isolate, prototype->GetFunction(context).ToLocalChecked());
    target->Set(context, v8::String::NewFromUtf8(isolate, className).ToLocalChecked(), prototype->GetFunction(context).ToLocalChecked());
}

void BrowserView::newFunction(const v8::FunctionCallbackInfo<v8::Value>& args)
{
    v8::Local<v8::Context> context = args.GetIsolate()->GetCurrentContext();
    v8::Isolate* isolate = args.GetIsolate();
    if (!args.IsConstructCall())
        return;

    gin_helper::Dictionary* options = nullptr;
    if (0 != args.Length())
        options = new gin_helper::Dictionary(isolate, args[0]->ToObject(context).ToLocalChecked());
    else
        options = new gin_helper::Dictionary(gin_helper::Dictionary::CreateEmpty(isolate));

    BrowserView* self = newBrowserView(options, args.This());
    args.GetReturnValue().Set(args.This());
    delete options;
}

void BrowserView::destroyed()
{
    destroy();
}

v8::Local<v8::Value> BrowserView::_getWebContentsApi()
{
    if (!m_webContents)
        return v8::Null(isolate());

    return v8::Local<v8::Value>::New(isolate(), m_webContents->getWrapper());
}

void BrowserView::_setBoundsApi(int x, int y, int w, int h)
{
    RECT oldBounds = getClientRect();
    const RECT dipBounds = { x, y, x + w, y + h };
    const bool attached = ::IsWindow(m_hWnd);
    const float scale = attached
        ? display::win::ScreenWin::GetScaleFactorForHWND(m_hWnd)
        : 1.0f;
    const gfx::Rect pixelBounds = attached
        ? display::win::ScreenWin::DIPToClientRect(
            m_hWnd, gfx::Rect(x, y, w, h))
        : gfx::Rect(x, y, w, h);
    const RECT clientBounds = {
        pixelBounds.x(), pixelBounds.y(), pixelBounds.right(), pixelBounds.bottom()
    };
    ::EnterCriticalSection(&m_rectLock);
    m_boundsInDips = dipBounds;
    m_clientRect = clientBounds;
    ::LeaveCriticalSection(&m_rectLock);

    if (m_webContents && w > 0 && h > 0)
        m_webContents->resize(w, h, scale);
    if (::IsWindow(m_hWnd)) {
        ::InvalidateRect(m_hWnd, &oldBounds, FALSE);
        ::InvalidateRect(m_hWnd, &clientBounds, FALSE);
    }
}

BrowserView* BrowserView::newBrowserView(const gin_helper::Dictionary* options, v8::Local<v8::Object> wrapper)
{
    BrowserView* self = new BrowserView(options->isolate(), wrapper);
    WebContents::BrowserWindowConstructorOptions* createWindowParam = new WebContents::BrowserWindowConstructorOptions();
    createWindowParam->minWidth = 1;
    createWindowParam->minHeight = 1;
    createWindowParam->styles = 0;
    createWindowParam->styleEx = 0;
    createWindowParam->transparent = false;

    WebContents* webContents = nullptr;
    v8::Handle<v8::Object> webContentsV8;
    // If no WebContents was passed to the constructor, create it from options.
    if (!options->Get("webContents", &webContentsV8)) {
        // Use options.webPreferences to create WebContents.
        gin_helper::Dictionary webPreferences = gin_helper::Dictionary::CreateEmpty(options->isolate());
        bool b = options->Get(options::kWebPreferences, &webPreferences);

        v8::Local<v8::Value> preloadScriptValue;
        if (options->Get(options::kPreloadScript, &preloadScriptValue))
            webPreferences.Set(options::kPreloadScript, preloadScriptValue);

        // Copy the backgroundColor to webContents.
        v8::Local<v8::Value> backgroundColorValue;
        if (options->Get(options::kBackgroundColor, &backgroundColorValue))
            webPreferences.Set(options::kBackgroundColor, backgroundColorValue);

        webContents = WebContents::create(options->isolate(), webPreferences, self);

        //webPreferences.GetBydefaultVal("nodeIntegration", true, &webContents->m_isNodeIntegration);
    } else
        DebugBreak();
    self->m_webContents = webContents;

    options->GetBydefaultVal("x", kNotSetXYFlag, &createWindowParam->x);
    options->GetBydefaultVal("y", kNotSetXYFlag, &createWindowParam->y);
    options->GetBydefaultVal("width", 1, &createWindowParam->width);
    options->GetBydefaultVal("height", 1, &createWindowParam->height);

    if (createWindowParam->width < createWindowParam->minWidth)
        createWindowParam->width = createWindowParam->minWidth;

    if (createWindowParam->height < createWindowParam->minHeight)
        createWindowParam->height = createWindowParam->minHeight;

    self->m_boundsInDips = {
        createWindowParam->x,
        createWindowParam->y,
        createWindowParam->x + createWindowParam->width,
        createWindowParam->y + createWindowParam->height
    };
    webContents->resize(createWindowParam->width, createWindowParam->height);
    webContents->addObserver(self);

    self->m_createWindowParam = createWindowParam;
    webContents->setCreateWindowParam(createWindowParam);
    self->m_liveSelf.Reset(options->isolate(), wrapper);

    return self;
}

void BrowserView::attachBrowserWindow(HWND hWnd)
{
    if (!m_webContents || !::IsWindow(hWnd))
        return;
    m_hWnd = hWnd;
    onParentScaleFactorChanged();
    m_webContents->setFocus(true);
    m_state = WindowInited;
}

void BrowserView::detachBrowserWindow()
{
    if (::IsWindow(m_hWnd)) {
        RECT bounds = getClientRect();
        ::InvalidateRect(m_hWnd, &bounds, FALSE);
    }
    m_hWnd = nullptr;
    if (m_state != WindowDestroyed)
        m_state = WindowUninited;
}

void BrowserView::onParentScaleFactorChanged()
{
    if (!m_webContents || !::IsWindow(m_hWnd))
        return;
    const float scale = display::win::ScreenWin::GetScaleFactorForHWND(m_hWnd);
    int width = 0;
    int height = 0;
    ::EnterCriticalSection(&m_rectLock);
    const RECT dipBounds = m_boundsInDips;
    width = dipBounds.right - dipBounds.left;
    height = dipBounds.bottom - dipBounds.top;
    const gfx::Rect pixelBounds = display::win::ScreenWin::DIPToClientRect(
        m_hWnd, gfx::Rect(dipBounds.left, dipBounds.top, width, height));
    m_clientRect = {
        pixelBounds.x(), pixelBounds.y(), pixelBounds.right(), pixelBounds.bottom()
    };
    ::LeaveCriticalSection(&m_rectLock);
    if (width > 0 && height > 0)
        m_webContents->resize(width, height, scale);
    ::InvalidateRect(m_hWnd, nullptr, FALSE);
}

void BrowserView::onPaintInUiThread(HDC hdc, const RECT& parentPaintRect)
{
    if (!m_webContents)
        return;
    const RECT bounds = getClientRect();
    RECT paintRect;
    if (!::IntersectRect(&paintRect, &bounds, &parentPaintRect))
        return;
    m_webContents->paintFrame(hdc, paintRect.left, paintRect.top,
        paintRect.left - bounds.left, paintRect.top - bounds.top,
        paintRect.right - paintRect.left, paintRect.bottom - paintRect.top);
}

void BrowserView::onWebContentsPaint(WebContents* contents)
{
    if (contents != m_webContents || !::IsWindow(m_hWnd))
        return;
    RECT rect = getClientRect();
    ::InvalidateRect(m_hWnd, &rect, FALSE);
}

void BrowserView::onWebContentsDeleted(WebContents* contents)
{
    if (contents != m_webContents)
        return;
    HWND parent = m_hWnd;
    const RECT bounds = getClientRect();
    m_webContents = nullptr;
    m_hWnd = nullptr;
    m_state = WindowDestroyed;
    if (::IsWindow(parent))
        ::InvalidateRect(parent, &bounds, FALSE);
}

void BrowserView::handleMouseMsgInUiThread(unsigned int message, int xInParent, int yInParent, unsigned int flags)
{
    if (!m_webContents)
        return;
    RECT r = getClientRect();
    m_webContents->sendWindowsMouseEvent(
        message, xInParent - r.left, yInParent - r.top, flags);
}

bool BrowserView::isClosed()
{
    return m_state == WindowDestroyed;
}

void BrowserView::close()
{
    destroy();
}

void BrowserView::destroy()
{
    if (m_state == WindowDestroyed)
        return;
    WebContents* contents = m_webContents;
    HWND parent = m_hWnd;
    const RECT bounds = getClientRect();
    m_webContents = nullptr;
    m_hWnd = nullptr;
    m_state = WindowDestroyed;
    if (contents) {
        contents->removeObserver(this);
        contents->destroyed();
    }
    if (::IsWindow(parent))
        ::InvalidateRect(parent, &bounds, FALSE);
}

v8::Local<v8::Object> BrowserView::getWrapper()
{
    return GetWrapper(isolate());
}

int BrowserView::getId() const
{
    return m_id;
}

WebContents* BrowserView::getWebContents() const
{
    return m_webContents;
}

HWND BrowserView::getHWND() const
{
    return m_hWnd;
}

gin::WrapperInfo BrowserView::kWrapperInfo = { gin::kEmbedderNativeGin };
v8::Persistent<v8::Function> BrowserView::constructor;

void initializeBrowseviewApi(v8::Local<v8::Object> exports, v8::Local<v8::Value> unused, v8::Local<v8::Context> context, void* priv)
{
    BrowserView::init(context->GetIsolate(), exports);
}

static const char nativeBrowserViewNativeScript[] = "console.log('nativeBrowserViewNative');;";
static NodeNative nativeBrowserViewNative { "BrowserView", nativeBrowserViewNativeScript, sizeof(nativeBrowserViewNativeScript) - 1 };
NODE_MODULE_CONTEXT_AWARE_BUILTIN_SCRIPT_MANUAL(electron_browser_browserview, atom::initializeBrowseviewApi, &nativeBrowserViewNative)

} // atom namespace
